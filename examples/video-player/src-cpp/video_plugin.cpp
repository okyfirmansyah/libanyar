// VideoPlugin — FFmpeg-based multimedia analysis + raw-frame decode pipeline
//
// Uses libavformat for container probing + packet iteration (bitrate),
// libavcodec + libswresample for audio waveform extraction, and
// libavcodec + optional libswscale for frame decode → SharedBuffer
// zero-copy delivery via anyar-shm:// URI scheme.
//
// When the decoded pixel format is directly supported by the WebGL
// renderer (YUV420P, NV12, NV21, RGBA, RGB24, GRAY8), frames are
// copied into shared memory WITHOUT swscale conversion — saving CPU
// and using less memory (e.g. YUV420P = 1.5 bytes/pixel vs RGBA = 4).

#include "video_plugin.h"

#include <anyar/types.h>
#include <anyar/event_bus.h>
#include <anyar/pinhole.h>
#include <anyar/window.h>

#include <libasyik/service.hpp>
#include <libasyik/http.hpp>

#include <nlohmann/json.hpp>

#include <boost/fiber/operations.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifdef __linux__
#include <epoxy/gl.h>
#endif

// ── FFmpeg C headers ────────────────────────────────────────────────────────
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
}

using json = nlohmann::json;

namespace videoplayer {

// ── Helpers ─────────────────────────────────────────────────────────────────

static double ts_to_sec(int64_t ts, AVRational tb) {
    if (ts == AV_NOPTS_VALUE) return 0.0;
    return static_cast<double>(ts) * av_q2d(tb);
}

/// Map a webgl_format string (used by the existing decode loop) to the
/// public Pinhole pixel_format enum.  Falls back to rgba on unknown input
/// so we never throw from the GTK main-thread render callback.
static anyar::pixel_format pixel_format_from_str(const std::string& s) {
    if (s == "yuv420")    return anyar::pixel_format::yuv420;
    if (s == "nv12")      return anyar::pixel_format::nv12;
    if (s == "nv21")      return anyar::pixel_format::nv21;
    if (s == "rgb")       return anyar::pixel_format::rgb;
    if (s == "grayscale") return anyar::pixel_format::grayscale;
    if (s == "bgra")      return anyar::pixel_format::bgra;
    return anyar::pixel_format::rgba;
}

// ── Open / Close ────────────────────────────────────────────────────────────

void VideoPlugin::open_file(const std::string& path) {
    close_file(); // clean previous state

    AVFormatContext* fc = nullptr;
    int ret = avformat_open_input(&fc, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(ret, err, sizeof(err));
        throw std::runtime_error("Cannot open media file: " + std::string(err));
    }

    ret = avformat_find_stream_info(fc, nullptr);
    if (ret < 0) {
        avformat_close_input(&fc);
        throw std::runtime_error("Cannot find stream info");
    }

    fmt_ctx_ = fc;
    file_path_ = path;

    // Probe metadata
    probe_ = {};
    probe_.duration = (fc->duration != AV_NOPTS_VALUE)
        ? static_cast<double>(fc->duration) / AV_TIME_BASE
        : 0.0;
    probe_.fileSizeBytes = std::filesystem::file_size(path);

    // Find best video / audio streams
    video_stream_ = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_ = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (video_stream_ >= 0) {
        auto* par = fc->streams[video_stream_]->codecpar;
        probe_.width  = par->width;
        probe_.height = par->height;
        const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id);
        probe_.videoCodec = desc ? desc->name : "unknown";
        AVRational fr = fc->streams[video_stream_]->avg_frame_rate;
        if (fr.den > 0) probe_.fps = av_q2d(fr);
    }

    if (audio_stream_ >= 0) {
        auto* par = fc->streams[audio_stream_]->codecpar;
        const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id);
        probe_.audioCodec  = desc ? desc->name : "unknown";
        probe_.sampleRate  = par->sample_rate;
        probe_.channels    = par->channels;
    }

    // Reset cached analysis
    bitrate_ready_  = false;
    waveform_ready_ = false;
}

void VideoPlugin::close_file() {
    stop_streaming();
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }
    file_path_.clear();
    video_stream_ = -1;
    audio_stream_ = -1;
    bitrate_ready_  = false;
    waveform_ready_ = false;
    bitrate_ = {};
    waveform_ = {};
}

void VideoPlugin::stop_streaming() {
    playing_   = false;
    streaming_ = false;
    release_latest_frame();
}

// Recycle the pinhole "latest frame" slot back to the pool.  Called from
// stop_streaming() and from set_pinhole() if the prior pinhole is being
// replaced.  Safe to call multiple times.
void VideoPlugin::release_latest_frame() {
    anyar::SharedBuffer* prev = nullptr;
    {
        std::lock_guard<std::mutex> lk(latest_mu_);
        prev        = latest_buf_;
        latest_buf_ = nullptr;
    }
    if (prev && frame_pool_) {
        frame_pool_->release_write(*prev, "{}");
        frame_pool_->release_read(prev->name());
    }
}

// ── set_pinhole ─────────────────────────────────────────────────────────────
//
// Wires the on_render callback that draws the most-recently published frame
// onto the native overlay surface.  Runs on the GTK main thread (NOT a fiber).
void VideoPlugin::set_pinhole(std::shared_ptr<anyar::Pinhole> pin) {
    pinhole_ = std::move(pin);
    if (!pinhole_) return;

    pinhole_->on_render([this](anyar::PinholeRenderContext& ctx) {
        anyar::SharedBuffer* buf = nullptr;
        int w = 0, h = 0;
        std::string fmt;
        {
            std::lock_guard<std::mutex> lk(latest_mu_);
            buf = latest_buf_;
            w   = latest_w_;
            h   = latest_h_;
            fmt = latest_fmt_;
        }
        if (!buf || w <= 0 || h <= 0) {
            ctx.clear(0.0f, 0.0f, 0.0f, 1.0f);
            return;
        }

        // Fill the surface with opaque black so letterbox/pillarbox bars
        // are black rather than transparent (which would show the desktop).
        ctx.clear(0.0f, 0.0f, 0.0f, 1.0f);

        // Compute a letterbox viewport that preserves the video's aspect ratio.
        auto [vw, vh] = ctx.size_px();
        const float video_ar  = static_cast<float>(w) / static_cast<float>(h);
        const float canvas_ar = static_cast<float>(vw) / static_cast<float>(vh);
        int lx, ly, lw, lh;
        if (video_ar >= canvas_ar) {
            // Wider than canvas → pillarbox (black top/bottom)
            lw = vw;
            lh = static_cast<int>(vw / video_ar + 0.5f);
            lx = 0;
            ly = (vh - lh) / 2;
        } else {
            // Taller than canvas → letterbox (black left/right)
            lh = vh;
            lw = static_cast<int>(vh * video_ar + 0.5f);
            lx = (vw - lw) / 2;
            ly = 0;
        }
#ifdef __linux__
        glViewport(lx, ly, lw, lh);
#endif
        ctx.draw_image(reinterpret_cast<const uint8_t*>(buf->data()),
                       buf->size(), w, h, pixel_format_from_str(fmt));
#ifdef __linux__
        glViewport(0, 0, vw, vh);  // restore for any subsequent draws
#endif
    });
}

// ── Bitrate analysis ────────────────────────────────────────────────────────
//
// Iterate all packets, bucket their byte sizes by time window.

void VideoPlugin::compute_bitrate(double step) {
    if (!fmt_ctx_) throw std::runtime_error("No file open");
    if (step <= 0) step = 0.5;

    // Seek back to beginning
    av_seek_frame(fmt_ctx_, -1, 0, AVSEEK_FLAG_BACKWARD);

    double dur = probe_.duration;
    if (dur <= 0) dur = 1.0;
    int n_buckets = static_cast<int>(std::ceil(dur / step));
    if (n_buckets < 1) n_buckets = 1;

    std::vector<int64_t> v_bytes(n_buckets, 0);
    std::vector<int64_t> a_bytes(n_buckets, 0);

    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt_ctx_, pkt) >= 0) {
        int idx = pkt->stream_index;
        double t = 0.0;
        if (idx >= 0 && idx < static_cast<int>(fmt_ctx_->nb_streams)) {
            t = ts_to_sec(pkt->pts, fmt_ctx_->streams[idx]->time_base);
        }
        int bucket = std::clamp(static_cast<int>(t / step), 0, n_buckets - 1);

        if (idx == video_stream_) {
            v_bytes[bucket] += pkt->size;
        } else if (idx == audio_stream_) {
            a_bytes[bucket] += pkt->size;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    // Convert to bits per second
    bitrate_.timestamps.resize(n_buckets);
    bitrate_.videoBps.resize(n_buckets);
    bitrate_.audioBps.resize(n_buckets);
    for (int i = 0; i < n_buckets; ++i) {
        bitrate_.timestamps[i] = i * step;
        bitrate_.videoBps[i]   = (v_bytes[i] * 8.0) / step;
        bitrate_.audioBps[i]   = (a_bytes[i] * 8.0) / step;
    }

    bitrate_ready_ = true;
}

// ── Waveform extraction ─────────────────────────────────────────────────────
//
// Decode audio → resample to mono 8kHz → downsample to N peak pairs.

void VideoPlugin::compute_waveform(int num_samples) {
    if (!fmt_ctx_) throw std::runtime_error("No file open");
    if (audio_stream_ < 0) {
        waveform_.peaks.clear();
        waveform_ready_ = true;
        return;
    }
    if (num_samples <= 0) num_samples = 2000;

    // Open audio decoder
    auto* par = fmt_ctx_->streams[audio_stream_]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (!dec) throw std::runtime_error("No decoder for audio codec");

    AVCodecContext* dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dec_ctx, par);
    if (avcodec_open2(dec_ctx, dec, nullptr) < 0) {
        avcodec_free_context(&dec_ctx);
        throw std::runtime_error("Cannot open audio decoder");
    }

    // Set up resampler → mono, 8000 Hz, FLT
    SwrContext* swr = swr_alloc_set_opts(nullptr,
        AV_CH_LAYOUT_MONO,  AV_SAMPLE_FMT_FLT, 8000,
        par->channel_layout ? par->channel_layout : av_get_default_channel_layout(par->channels),
        static_cast<AVSampleFormat>(par->format), par->sample_rate,
        0, nullptr);
    if (!swr || swr_init(swr) < 0) {
        avcodec_free_context(&dec_ctx);
        if (swr) swr_free(&swr);
        throw std::runtime_error("Cannot initialize resampler");
    }

    // Collect all resampled audio into a buffer
    std::vector<float> all_samples;
    all_samples.reserve(8000 * static_cast<int>(probe_.duration + 1));

    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame  = av_frame_alloc();
    AVFrame*  resampled = av_frame_alloc();
    resampled->format      = AV_SAMPLE_FMT_FLT;
    resampled->channel_layout = AV_CH_LAYOUT_MONO;
    resampled->sample_rate = 8000;

    // Seek to beginning
    av_seek_frame(fmt_ctx_, audio_stream_, 0, AVSEEK_FLAG_BACKWARD);

    while (av_read_frame(fmt_ctx_, pkt) >= 0) {
        if (pkt->stream_index != audio_stream_) {
            av_packet_unref(pkt);
            continue;
        }
        avcodec_send_packet(dec_ctx, pkt);
        while (avcodec_receive_frame(dec_ctx, frame) == 0) {
            // Estimate output size
            int out_count = swr_get_out_samples(swr, frame->nb_samples);
            if (out_count <= 0) out_count = frame->nb_samples * 2;

            // Re-set frame properties (av_frame_unref clears them)
            resampled->format         = AV_SAMPLE_FMT_FLT;
            resampled->channel_layout = AV_CH_LAYOUT_MONO;
            resampled->sample_rate    = 8000;
            resampled->nb_samples     = out_count;
            if (av_frame_get_buffer(resampled, 0) < 0) {
                av_frame_unref(frame);
                continue;
            }

            int got = swr_convert(swr,
                resampled->data, out_count,
                (const uint8_t**)frame->data, frame->nb_samples);

            if (got > 0 && resampled->data[0]) {
                const float* fdata = reinterpret_cast<const float*>(resampled->data[0]);
                all_samples.insert(all_samples.end(), fdata, fdata + got);
            }
            av_frame_unref(resampled);
        }
        av_packet_unref(pkt);
    }

    // Flush decoder
    avcodec_send_packet(dec_ctx, nullptr);
    while (avcodec_receive_frame(dec_ctx, frame) == 0) {
        int out_count = swr_get_out_samples(swr, frame->nb_samples);
        if (out_count <= 0) out_count = frame->nb_samples * 2;
        resampled->format         = AV_SAMPLE_FMT_FLT;
        resampled->channel_layout = AV_CH_LAYOUT_MONO;
        resampled->sample_rate    = 8000;
        resampled->nb_samples     = out_count;
        if (av_frame_get_buffer(resampled, 0) < 0) {
            av_frame_unref(frame);
            continue;
        }
        int got = swr_convert(swr,
            resampled->data, out_count,
            (const uint8_t**)frame->data, frame->nb_samples);
        if (got > 0 && resampled->data[0]) {
            const float* fdata = reinterpret_cast<const float*>(resampled->data[0]);
            all_samples.insert(all_samples.end(), fdata, fdata + got);
        }
        av_frame_unref(resampled);
    }

    // Flush resampler (buffered samples)
    {
        int out_count = swr_get_delay(swr, 8000) + 64;
        if (out_count > 0) {
            resampled->format         = AV_SAMPLE_FMT_FLT;
            resampled->channel_layout = AV_CH_LAYOUT_MONO;
            resampled->sample_rate    = 8000;
            resampled->nb_samples     = out_count;
            if (av_frame_get_buffer(resampled, 0) >= 0) {
                int got = swr_convert(swr, resampled->data, out_count, nullptr, 0);
                if (got > 0 && resampled->data[0]) {
                    const float* fdata = reinterpret_cast<const float*>(resampled->data[0]);
                    all_samples.insert(all_samples.end(), fdata, fdata + got);
                }
            }
            av_frame_unref(resampled);
        }
    }

    av_frame_free(&frame);
    av_frame_free(&resampled);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&dec_ctx);

    // Downsample to peak pairs: for each segment produce (min, max)
    int total = static_cast<int>(all_samples.size());
    if (total == 0) {
        waveform_.peaks.clear();
        waveform_ready_ = true;
        return;
    }

    int actual_samples = std::min(num_samples, total / 2);
    if (actual_samples < 1) actual_samples = 1;
    int per_seg = total / actual_samples;

    waveform_.peaks.resize(actual_samples * 2);
    for (int i = 0; i < actual_samples; ++i) {
        int start = i * per_seg;
        int end   = std::min(start + per_seg, total);
        float lo = 0, hi = 0;
        for (int j = start; j < end; ++j) {
            lo = std::min(lo, all_samples[j]);
            hi = std::max(hi, all_samples[j]);
        }
        waveform_.peaks[i * 2]     = lo;
        waveform_.peaks[i * 2 + 1] = hi;
    }

    waveform_ready_ = true;
}

// \u2500\u2500 Raw-frame decode loop (runs as a fibre) \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
//
// Opens a SEPARATE AVFormatContext for this file so seek/decode state is
// independent from analysis.  When the decoded pixel format is directly
// supported by the WebGL renderer, frames are copied into shared memory
// without conversion.  Otherwise falls back to swscale → RGBA.
// The frontend fetches frames via anyar-shm:// (zero-copy on Linux).

void VideoPlugin::run_decode_loop() {
    // \u2500\u2500 Open a private format context \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, file_path_.c_str(), nullptr, nullptr) < 0) {
        if (events_) events_->emit("video:error", {{"message", "Cannot open file for decoding"}});
        return;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        if (events_) events_->emit("video:error", {{"message", "Cannot find stream info"}});
        return;
    }

    int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0) {
        avformat_close_input(&fmt);
        if (events_) events_->emit("video:error", {{"message", "No video stream found"}});
        return;
    }

    // \u2500\u2500 Open video decoder \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    auto* par = fmt->streams[vidx]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (!dec) { avformat_close_input(&fmt); return; }

    AVCodecContext* dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dec_ctx, par);
    if (avcodec_open2(dec_ctx, dec, nullptr) < 0) {
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt);
        return;
    }

    // \u2500\u2500 Allocate work frames \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt   = av_packet_alloc();

    SwsContext* sws = nullptr;
    AVFrame*    conv_frame = nullptr;    // only used when sws conversion needed
    uint32_t w = 0, h = 0;
    size_t frame_bytes = 0;              // per-frame buffer size
    std::string webgl_format;            // "yuv420", "nv12", "nv21", "rgba", "rgb", "grayscale"
    bool use_sws = false;                // true if we need swscale conversion

    double fps = probe_.fps > 0 ? probe_.fps : 25.0;
    uint32_t fnum = 0;

    // ── Format mapping: FFmpeg pixel format → WebGL-supported format ────
    //    Returns "" if no direct mapping (needs sws fallback to RGBA).
    auto map_format = [](AVPixelFormat pf) -> std::string {
        switch (pf) {
            case AV_PIX_FMT_YUV420P: return "yuv420";
            case AV_PIX_FMT_NV12:    return "nv12";
            case AV_PIX_FMT_NV21:    return "nv21";
            case AV_PIX_FMT_RGBA:    return "rgba";
            case AV_PIX_FMT_RGB24:   return "rgb";
            case AV_PIX_FMT_GRAY8:   return "grayscale";
            default:                  return "";
        }
    };

    // Calculate buffer size for a given format + dimensions
    auto calc_frame_bytes = [](const std::string& fmt, uint32_t fw, uint32_t fh) -> size_t {
        if (fmt == "yuv420" || fmt == "nv12" || fmt == "nv21")
            return static_cast<size_t>(fw) * fh * 3 / 2;  // 1.5 bytes/pixel
        if (fmt == "rgb")
            return static_cast<size_t>(fw) * fh * 3;
        if (fmt == "grayscale")
            return static_cast<size_t>(fw) * fh;
        // rgba (and sws fallback)
        return static_cast<size_t>(fw) * fh * 4;
    };

    // Helper: detect format from first decoded frame and set up state
    auto init_format = [&](const AVFrame* decoded) -> bool {
        w = static_cast<uint32_t>(decoded->width);
        h = static_cast<uint32_t>(decoded->height);

        webgl_format = map_format(static_cast<AVPixelFormat>(decoded->format));
        if (!webgl_format.empty()) {
            // Direct path — no sws needed
            use_sws = false;
            frame_bytes = calc_frame_bytes(webgl_format, w, h);
            // Clean up any previous sws state
            if (sws) { sws_freeContext(sws); sws = nullptr; }
            if (conv_frame) { av_frame_free(&conv_frame); conv_frame = nullptr; }
            return true;
        }

        // Fallback: convert to RGBA via swscale
        use_sws = true;
        webgl_format = "rgba";
        frame_bytes = static_cast<size_t>(w) * h * 4;

        if (sws) sws_freeContext(sws);
        if (conv_frame) av_frame_free(&conv_frame);

        sws = sws_getContext(
            decoded->width, decoded->height,
            static_cast<AVPixelFormat>(decoded->format),
            decoded->width, decoded->height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return false;

        conv_frame = av_frame_alloc();
        conv_frame->format = AV_PIX_FMT_RGBA;
        conv_frame->width  = decoded->width;
        conv_frame->height = decoded->height;
        if (av_frame_get_buffer(conv_frame, 32) < 0) {
            sws_freeContext(sws); sws = nullptr;
            av_frame_free(&conv_frame);
            return false;
        }
        return true;
    };

    // \u2500\u2500 Helper: Decode the next video frame into `frame` \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    auto decode_next = [&]() -> bool {
        if (avcodec_receive_frame(dec_ctx, frame) == 0)
            return true;

        int read_errors = 0;
        while (true) {
            int ret = av_read_frame(fmt, pkt);
            if (ret == AVERROR_EOF) return false;
            if (ret < 0) {
                if (++read_errors >= 8) return false;
                continue;
            }
            read_errors = 0;

            if (pkt->stream_index != vidx) {
                av_packet_unref(pkt);
                continue;
            }

            ret = avcodec_send_packet(dec_ctx, pkt);
            av_packet_unref(pkt);

            if (ret == AVERROR(EAGAIN)) {
                if (avcodec_receive_frame(dec_ctx, frame) == 0)
                    return true;
            }

            if (avcodec_receive_frame(dec_ctx, frame) == 0)
                return true;
        }
    };

    // \u2500\u2500 Helper: Copy current `frame` into a SharedBuffer slot \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    //    For directly-supported formats, copies raw planes without sws.
    //    For unsupported formats, runs swscale \u2192 RGBA first.
    //    Returns PTS on success, or \u20131.0 on fatal error.
    auto copy_frame_to_shm = [&](anyar::SharedBuffer& dst,
                                 double fallback_pts) -> double {
        // Resolution or format change guard
        if (static_cast<uint32_t>(frame->width)  != w ||
            static_cast<uint32_t>(frame->height) != h) {
            if (!init_format(frame)) return -1.0;
            // Recreate the pool with new buffer size
            frame_pool_ = std::make_unique<anyar::SharedBufferPool>(
                "video-frames", frame_bytes, 5);
            if (events_) events_->emit("video:ready", {
                {"width", w}, {"height", h}, {"fps", fps},
                {"format", webgl_format}
            });
        }

        double pts = (frame->pts != AV_NOPTS_VALUE)
            ? frame->pts * av_q2d(fmt->streams[vidx]->time_base)
            : fallback_pts;

        uint8_t* out = reinterpret_cast<uint8_t*>(dst.data());

        if (!use_sws) {
            // Direct copy \u2014 format already matches a WebGL renderer
            if (webgl_format == "yuv420") {
                // 3 separate planes: Y (w*h), U (w/2*h/2), V (w/2*h/2)
                size_t y_size  = static_cast<size_t>(w) * h;
                size_t uv_size = static_cast<size_t>(w / 2) * (h / 2);

                // Y plane
                if (frame->linesize[0] == static_cast<int>(w)) {
                    std::memcpy(out, frame->data[0], y_size);
                } else {
                    for (uint32_t r = 0; r < h; ++r)
                        std::memcpy(out + r * w, frame->data[0] + r * frame->linesize[0], w);
                }
                out += y_size;

                // U plane
                uint32_t hw = w / 2, hh = h / 2;
                if (frame->linesize[1] == static_cast<int>(hw)) {
                    std::memcpy(out, frame->data[1], uv_size);
                } else {
                    for (uint32_t r = 0; r < hh; ++r)
                        std::memcpy(out + r * hw, frame->data[1] + r * frame->linesize[1], hw);
                }
                out += uv_size;

                // V plane
                if (frame->linesize[2] == static_cast<int>(hw)) {
                    std::memcpy(out, frame->data[2], uv_size);
                } else {
                    for (uint32_t r = 0; r < hh; ++r)
                        std::memcpy(out + r * hw, frame->data[2] + r * frame->linesize[2], hw);
                }
            } else if (webgl_format == "nv12" || webgl_format == "nv21") {
                // 2 planes: Y (w*h), UV interleaved (w*h/2)
                size_t y_size  = static_cast<size_t>(w) * h;
                size_t uv_size = static_cast<size_t>(w) * (h / 2);

                // Y plane
                if (frame->linesize[0] == static_cast<int>(w)) {
                    std::memcpy(out, frame->data[0], y_size);
                } else {
                    for (uint32_t r = 0; r < h; ++r)
                        std::memcpy(out + r * w, frame->data[0] + r * frame->linesize[0], w);
                }
                out += y_size;

                // UV interleaved plane
                if (frame->linesize[1] == static_cast<int>(w)) {
                    std::memcpy(out, frame->data[1], uv_size);
                } else {
                    uint32_t hh = h / 2;
                    for (uint32_t r = 0; r < hh; ++r)
                        std::memcpy(out + r * w, frame->data[1] + r * frame->linesize[1], w);
                }
            } else if (webgl_format == "rgba") {
                // Single plane, 4 bytes/pixel
                if (frame->linesize[0] == static_cast<int>(w * 4)) {
                    std::memcpy(out, frame->data[0], frame_bytes);
                } else {
                    for (uint32_t r = 0; r < h; ++r)
                        std::memcpy(out + r * w * 4,
                                    frame->data[0] + r * frame->linesize[0], w * 4);
                }
            } else if (webgl_format == "rgb") {
                // Single plane, 3 bytes/pixel
                if (frame->linesize[0] == static_cast<int>(w * 3)) {
                    std::memcpy(out, frame->data[0], frame_bytes);
                } else {
                    for (uint32_t r = 0; r < h; ++r)
                        std::memcpy(out + r * w * 3,
                                    frame->data[0] + r * frame->linesize[0], w * 3);
                }
            } else {
                // grayscale \u2014 single plane, 1 byte/pixel
                if (frame->linesize[0] == static_cast<int>(w)) {
                    std::memcpy(out, frame->data[0], frame_bytes);
                } else {
                    for (uint32_t r = 0; r < h; ++r)
                        std::memcpy(out + r * w,
                                    frame->data[0] + r * frame->linesize[0], w);
                }
            }
        } else {
            // sws fallback \u2192 RGBA
            sws_scale(sws, frame->data, frame->linesize, 0,
                      static_cast<int>(h), conv_frame->data, conv_frame->linesize);

            if (conv_frame->linesize[0] == static_cast<int>(w * 4)) {
                std::memcpy(out, conv_frame->data[0], frame_bytes);
            } else {
                for (uint32_t r = 0; r < h; ++r)
                    std::memcpy(out + r * w * 4,
                                conv_frame->data[0] + r * conv_frame->linesize[0], w * 4);
            }
        }

        ++fnum;
        return pts;
    };

    // Helper: publish a completed frame.
    //
    // WebGL mode: transition WRITING→READY and emit `buffer:ready`; the JS
    // frontend will fetch + render + call `video:pool-release` to recycle.
    //
    // Pinhole mode: keep the slot in WRITING state (we own it), swap it
    // into `latest_buf_` under `latest_mu_`, recycle the previous holder,
    // and request a redraw.  No JS round-trip; on_render reads the bytes
    // directly via SharedBuffer::data().
    auto emit_frame = [&](anyar::SharedBuffer& buf, double pts) {
        if (mode_ == RenderMode::Pinhole) {
            anyar::SharedBuffer* prev = nullptr;
            {
                std::lock_guard<std::mutex> lk(latest_mu_);
                prev        = latest_buf_;
                latest_buf_ = &buf;
                latest_w_   = static_cast<int>(w);
                latest_h_   = static_cast<int>(h);
                latest_fmt_ = webgl_format;
            }
            if (prev && frame_pool_) {
                frame_pool_->release_write(*prev, "{}");
                frame_pool_->release_read(prev->name());
            }
            if (pinhole_) pinhole_->request_redraw();
            // Lightweight progress event so the timeline still advances.
            if (events_) {
                events_->emit("video:frame-pts", {
                    {"pts", pts},
                    {"frame", fnum},
                });
            }
            return;
        }

        // WebGL mode (legacy path)
        if (!events_) return;
        frame_pool_->release_write(buf, "{}");
        events_->emit("buffer:ready", {
            {"name", buf.name()},
            {"pool", "video-frames"},
            {"url", "anyar-shm://" + buf.name()},
            {"size", buf.size()},
            {"metadata", {
                {"width", w},
                {"height", h},
                {"pts", pts},
                {"frame", fnum},
                {"format", webgl_format}
            }}
        });
    };

    // \u2500\u2500 Decode first frame for pixel-format detection \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    {
        bool got_first = false;
        while (!got_first) {
            int ret = av_read_frame(fmt, pkt);
            if (ret < 0) break;
            if (pkt->stream_index == vidx) {
                avcodec_send_packet(dec_ctx, pkt);
                if (avcodec_receive_frame(dec_ctx, frame) == 0)
                    got_first = true;
            }
            av_packet_unref(pkt);
        }

        if (!got_first || !init_format(frame)) {
            av_packet_free(&pkt);
            av_frame_free(&frame);
            if (sws) sws_freeContext(sws);
            if (conv_frame) av_frame_free(&conv_frame);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&fmt);
            if (events_) events_->emit("video:error", {{"message", "Failed to decode first frame"}});
            return;
        }
    }

    // \u2500\u2500 Create SharedBufferPool \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    // ── Create SharedBufferPool ──────────────────────────────────────────
    // Pinhole mode permanently holds one slot in `latest_buf_` (WRITING)
    // until the next emit_frame replaces it, so it needs one extra slot
    // beyond the working capacity to avoid deadlocking acquire_write().
    const int pool_slots = (mode_ == RenderMode::Pinhole) ? 6 : 5;
    frame_pool_ = std::make_unique<anyar::SharedBufferPool>(
        "video-frames", frame_bytes, pool_slots);

    // \u2500\u2500 Send ready event + poster frame \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    if (events_) {
        events_->emit("video:ready", {
            {"width", w}, {"height", h}, {"fps", fps},
            {"format", webgl_format}
        });
    }

    {
        auto& buf = frame_pool_->acquire_write();
        double pts = copy_frame_to_shm(buf, 0.0);
        if (pts < 0) goto cleanup;
        emit_frame(buf, pts);
    }

    av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec_ctx);

    // \u2500\u2500 Main audio-driven decode / send loop \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    //
    //  Architecture:
    //    1. Pre-decode video frames into SharedBuffer pool slots.
    //    2. Frontend audio element is the master clock; it sends
    //       video:sync { time } via IPC.
    //    3. The command handler stores that in audio_time_.
    //    4. This loop emits buffer:ready for the frame whose PTS
    //       best matches audio_time_.
    //    For video-only files (no audio stream), a local wall-clock
    //    is used instead so playback is self-driven.
    {
        bool eos = false;
        const double half_frame = 0.5 / fps;
        bool was_playing = false;

        // Detect video-only (no audio stream in the file)
        const bool has_audio = (audio_stream_ >= 0);

        // Wall-clock timing for video-only playback
        using wall_clock = std::chrono::steady_clock;
        wall_clock::time_point wall_play_start = wall_clock::now();
        double wall_time_offset = 0.0;  // accumulated time base (handles pause/seek)

        constexpr int POOL_CAP = 5;
        std::array<double, POOL_CAP> pool_pts{};
        std::array<anyar::SharedBuffer*, POOL_CAP> pool_bufs{};
        int pool_head = 0, pool_tail = 0, pool_count = 0;

        while (streaming_) {

            // \u2500\u2500 Handle pending seek \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
            if (pending_seek_ >= 0) {
                double t = pending_seek_;
                pending_seek_ = -1.0;
                audio_time_   = -1.0;

                // Reset wall-clock base to seek target
                if (!has_audio) {
                    wall_time_offset = t;
                    wall_play_start = wall_clock::now();
                }

                // Release any held pool buffers (queued via acquire_write
                // but never emitted — must transition WRITING→READY→FREE,
                // not just READING→FREE, otherwise slots leak).
                for (int i = 0; i < pool_count; ++i) {
                    int idx = (pool_head + i) % POOL_CAP;
                    if (pool_bufs[idx]) {
                        frame_pool_->release_write(*pool_bufs[idx], "{}");
                        frame_pool_->release_read(pool_bufs[idx]->name());
                        pool_bufs[idx] = nullptr;
                    }
                }
                pool_head = pool_tail = pool_count = 0;
                eos = false;

                int64_t ts = static_cast<int64_t>(t * AV_TIME_BASE);
                av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(dec_ctx);

                // Fast-forward to target
                {
                    AVRational vtb = fmt->streams[vidx]->time_base;
                    int skipped = 0;
                    bool ff_ok = true;
                    while (streaming_ && pending_seek_ < 0) {
                        if (!decode_next()) { ff_ok = false; break; }
                        double fpts = (frame->pts != AV_NOPTS_VALUE)
                            ? frame->pts * av_q2d(vtb) : t;
                        if (fpts >= t - half_frame) break;
                        ++skipped;
                        if ((skipped & 0xF) == 0)
                            boost::this_fiber::sleep_for(
                                std::chrono::milliseconds(0));
                    }

                    if (!ff_ok && pending_seek_ < 0) {
                        av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
                        avcodec_flush_buffers(dec_ctx);
                    }
                }

                // Send preview frame
                if (pending_seek_ < 0 && frame->data[0]) {
                    auto& buf = frame_pool_->acquire_write();
                    double pts = copy_frame_to_shm(buf, t);
                    if (pts >= 0) {
                        emit_frame(buf, pts);
                    }
                }

                continue;
            }

            // \u2500\u2500 Paused \u2014 idle wait \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
            if (!playing_) {
                // On pause transition: save accumulated wall-clock time
                if (was_playing && !has_audio) {
                    auto now = wall_clock::now();
                    wall_time_offset += std::chrono::duration<double>(now - wall_play_start).count();
                }
                was_playing = false;
                boost::this_fiber::sleep_for(std::chrono::milliseconds(30));
                continue;
            }

            // \u2500\u2500 Just resumed playing \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
            if (!was_playing) {
                was_playing = true;
                // Reset wall-clock start for video-only resume
                if (!has_audio) {
                    wall_play_start = wall_clock::now();
                }
                if (eos) {
                    eos = false;
                    double atime = has_audio ? audio_time_ : wall_time_offset;
                    if (atime >= 0) {
                        int64_t ts = static_cast<int64_t>(atime * AV_TIME_BASE);
                        av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
                        avcodec_flush_buffers(dec_ctx);
                        // Release held buffers (queued via acquire_write
                        // but never emitted — must transition WRITING→FREE).
                        for (int i = 0; i < pool_count; ++i) {
                            int idx2 = (pool_head + i) % POOL_CAP;
                            if (pool_bufs[idx2]) {
                                frame_pool_->release_write(*pool_bufs[idx2], "{}");
                                frame_pool_->release_read(pool_bufs[idx2]->name());
                                pool_bufs[idx2] = nullptr;
                            }
                        }
                        pool_head = pool_tail = pool_count = 0;
                    }
                }
            }

            // \u2500\u2500 Pre-decode to fill pool \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
            while (pool_count < POOL_CAP && !eos && streaming_
                   && pending_seek_ < 0) {
                if (!decode_next()) { eos = true; break; }
                auto& buf = frame_pool_->acquire_write();
                double pts = copy_frame_to_shm(buf, 0.0);
                if (pts < 0) goto cleanup;
                pool_bufs[pool_tail] = &buf;
                pool_pts[pool_tail] = pts;
                pool_tail = (pool_tail + 1) % POOL_CAP;
                ++pool_count;
                boost::this_fiber::sleep_for(std::chrono::milliseconds(1));
            }

            // \u2500\u2500 Audio-driven frame dispatch \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
            {
                double atime;
                if (has_audio) {
                    atime = audio_time_;
                    if (atime < 0) {
                        boost::this_fiber::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }
                } else {
                    // Video-only: compute time from local wall clock
                    auto now = wall_clock::now();
                    atime = wall_time_offset +
                        std::chrono::duration<double>(now - wall_play_start).count();
                }

                // Drop frames the clock has already passed
                while (pool_count > 1) {
                    int next_idx = (pool_head + 1) % POOL_CAP;
                    if (pool_pts[pool_head] < atime - half_frame &&
                        pool_pts[next_idx]  <= atime + half_frame) {
                        // Release dropped frame back to pool
                        if (pool_bufs[pool_head]) {
                            frame_pool_->release_write(*pool_bufs[pool_head], "{}");
                            frame_pool_->release_read(pool_bufs[pool_head]->name());
                            pool_bufs[pool_head] = nullptr;
                        }
                        pool_head = next_idx;
                        --pool_count;
                    } else {
                        break;
                    }
                }

                // Send head frame if its PTS is due
                if (pool_count > 0 &&
                    pool_pts[pool_head] <= atime + half_frame) {
                    emit_frame(*pool_bufs[pool_head], pool_pts[pool_head]);
                    pool_bufs[pool_head] = nullptr;
                    pool_head = (pool_head + 1) % POOL_CAP;
                    --pool_count;
                }
            }

            // \u2500\u2500 EOS: pool fully drained \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
            if (eos && pool_count == 0) {
                if (has_audio) {
                    // Audio still playing — re-seek video to catch up
                    double atime = audio_time_;
                    if (atime >= 0 && atime < probe_.duration - 1.0) {
                        int64_t ts2 = static_cast<int64_t>(atime * AV_TIME_BASE);
                        av_seek_frame(fmt, -1, ts2, AVSEEK_FLAG_BACKWARD);
                        avcodec_flush_buffers(dec_ctx);
                        eos = false;
                        continue;
                    }
                }
                if (events_) events_->emit("video:ended", {});
                playing_ = false;
                continue;
            }

            boost::this_fiber::sleep_for(std::chrono::milliseconds(2));
        }
        // Log exit from main decode loop
        std::cout << "[VideoPlugin] Exiting main decode loop" << std::endl;
    }

cleanup:
    av_packet_free(&pkt);
    av_frame_free(&frame);
    if (conv_frame) av_frame_free(&conv_frame);
    if (sws) sws_freeContext(sws);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt);
    frame_pool_.reset();
}

// ── HTTP streaming route ────────────────────────────────────────────────────
//
// Serve the raw file with Range-request support so <video> can seek.

void VideoPlugin::register_stream_route() {
    // Intentional no-op: we use serve_static or a custom route below
    // during video:open, we return the URL pointing to the file served
    // via a dedicated route.
}

// ── Plugin initialization ───────────────────────────────────────────────────

void VideoPlugin::initialize(anyar::PluginContext& ctx) {
    service_ = ctx.service;
    events_ = &ctx.events;
    auto& cmds = ctx.commands;

    // ── video:play — Start / resume frame streaming ─────────────────────
    cmds.add("video:play", [this](const json& /*args*/) -> json {
        if (!streaming_) {
            // First play — launch the decode loop fibre
            streaming_ = true;
            playing_   = true;
            pending_seek_ = -1.0;
            audio_time_   = -1.0;
            service_->execute([this]() {
                try {
                    run_decode_loop();
                } catch (const std::exception& ex) {
                    std::cerr << "[VideoPlugin] Exception in decode loop: " << ex.what() << std::endl;
                } catch (...) {
                    std::cerr << "[VideoPlugin] Unknown exception in decode loop." << std::endl;
                }
                streaming_ = false;
                playing_ = false;
                frame_pool_.reset();
            });
        } else {
            playing_ = true;
        }
        return {{"ok", true}};
    });

    // ── video:pause — Pause frame streaming ─────────────────────────────
    cmds.add("video:pause", [this](const json& /*args*/) -> json {
        playing_ = false;
        return {{"ok", true}};
    });

    // ── video:seek — Seek to a timestamp ────────────────────────────────
    cmds.add("video:seek", [this](const json& args) -> json {
        pending_seek_ = args.at("time").get<double>();
        return {{"ok", true}};
    });

    // ── video:sync — Audio clock update from frontend ───────────────────
    cmds.add("video:sync", [this](const json& args) -> json {
        audio_time_ = args.at("time").get<double>();
        return {{"ok", true}};
    });

    // ── video:pool-release — Consumer releases a buffer back ────────────
    cmds.add("video:pool-release", [this](const json& args) -> json {
        if (frame_pool_) {
            std::string buf_name = args.at("name").get<std::string>();
            frame_pool_->release_read(buf_name);
        }
        return {{"ok", true}};
    });

    // ── Register the video file streaming route ─────────────────────────────
    // Serves the currently opened file with Range-request support so <video>
    // can seek. We register a custom GET handler on the server.
    if (ctx.server) {
        ctx.server->on_http_request(
            "/video/stream", "GET",
            [this](asyik::http_request_ptr req, asyik::http_route_args args) {
                std::lock_guard<boost::fibers::mutex> lock(mtx_);

                if (file_path_.empty()) {
                    req->response.result(404);
                    req->response.body = "No video file open";
                    return;
                }

                // Read the entire file and serve (for simplicity in this demo)
                // Range-request support: parse Range header
                std::ifstream ifs(file_path_, std::ios::binary | std::ios::ate);
                if (!ifs) {
                    req->response.result(404);
                    req->response.body = "Cannot read file";
                    return;
                }

                int64_t file_size = ifs.tellg();

                // Determine MIME type
                std::string mime = "application/octet-stream";
                auto ext = std::filesystem::path(file_path_).extension().string();
                if (ext == ".mp4") mime = "video/mp4";
                else if (ext == ".webm") mime = "video/webm";
                else if (ext == ".mkv") mime = "video/x-matroska";
                else if (ext == ".avi") mime = "video/x-msvideo";
                else if (ext == ".mov") mime = "video/quicktime";
                else if (ext == ".ogg") mime = "video/ogg";

                // Check for Range header
                std::string range_hdr;
                auto it = req->headers.find("Range");
                if (it != req->headers.end()) {
                    range_hdr = std::string(it->value());
                }

                if (!range_hdr.empty() && range_hdr.substr(0, 6) == "bytes=") {
                    // Parse "bytes=START-END"
                    std::string range_val = range_hdr.substr(6);
                    int64_t start = 0, end = file_size - 1;
                    auto dash = range_val.find('-');
                    if (dash != std::string::npos) {
                        std::string s_start = range_val.substr(0, dash);
                        std::string s_end   = range_val.substr(dash + 1);
                        if (!s_start.empty()) start = std::stoll(s_start);
                        if (!s_end.empty())   end   = std::stoll(s_end);
                    }
                    if (start < 0) start = 0;
                    if (end >= file_size) end = file_size - 1;
                    int64_t length = end - start + 1;

                    ifs.seekg(start, std::ios::beg);
                    std::string data(length, '\0');
                    ifs.read(&data[0], length);

                    req->response.result(206);
                    req->response.headers.set("Content-Type", mime);
                    req->response.headers.set("Content-Length", std::to_string(length));
                    req->response.headers.set("Content-Range",
                        "bytes " + std::to_string(start) + "-" + std::to_string(end) +
                        "/" + std::to_string(file_size));
                    req->response.headers.set("Accept-Ranges", "bytes");
                    req->response.headers.set("Access-Control-Allow-Origin", "*");
                    req->response.body = std::move(data);
                } else {
                    // Serve full file
                    ifs.seekg(0, std::ios::beg);
                    std::string data(file_size, '\0');
                    ifs.read(&data[0], file_size);

                    req->response.result(200);
                    req->response.headers.set("Content-Type", mime);
                    req->response.headers.set("Content-Length", std::to_string(file_size));
                    req->response.headers.set("Accept-Ranges", "bytes");
                    req->response.headers.set("Access-Control-Allow-Origin", "*");
                    req->response.body = std::move(data);
                }
            }
        );
    }

    // ── video:open ──────────────────────────────────────────────────────────
    cmds.add("video:open", [this](const json& args) -> json {
        std::lock_guard<boost::fibers::mutex> lock(mtx_);

        std::string path = args.at("path").get<std::string>();
        if (!std::filesystem::exists(path)) {
            throw std::runtime_error("File not found: " + path);
        }
        path = std::filesystem::canonical(path).string();

        open_file(path);

        // Return info; the frontend will use /video/stream to play the file
        return {
            {"url",        "/video/stream"},
            {"duration",   probe_.duration},
            {"width",      probe_.width},
            {"height",     probe_.height},
            {"videoCodec", probe_.videoCodec},
            {"audioCodec", probe_.audioCodec},
            {"fps",        probe_.fps},
            {"sampleRate", probe_.sampleRate},
            {"channels",   probe_.channels},
            {"fileSize",   probe_.fileSizeBytes}
        };
    });

    // ── video:bitrate ───────────────────────────────────────────────────────
    cmds.add("video:bitrate", [this](const json& args) -> json {
        std::lock_guard<boost::fibers::mutex> lock(mtx_);
        if (!fmt_ctx_) throw std::runtime_error("No file open");

        double step = args.value("step", 0.5);
        if (!bitrate_ready_) {
            compute_bitrate(step);
        }

        return {
            {"timestamps", bitrate_.timestamps},
            {"videoBps",   bitrate_.videoBps},
            {"audioBps",   bitrate_.audioBps}
        };
    });

    // ── video:waveform ──────────────────────────────────────────────────────
    cmds.add("video:waveform", [this](const json& args) -> json {
        std::lock_guard<boost::fibers::mutex> lock(mtx_);
        if (!fmt_ctx_) throw std::runtime_error("No file open");

        int samples = args.value("samples", 2000);
        if (!waveform_ready_) {
            compute_waveform(samples);
        }

        return {
            {"peaks", waveform_.peaks}
        };
    });

    // ── video:close ─────────────────────────────────────────────────────────
    cmds.add("video:close", [this](const json& /*args*/) -> json {
        std::lock_guard<boost::fibers::mutex> lock(mtx_);
        stop_streaming();
        close_file();
        return {{"closed", true}};
    });

    // ── video:get-mode — frontend asks which renderer to use ───────────
    cmds.add("video:get-mode", [this](const json& /*args*/) -> json {
        return {
            {"mode", mode_ == RenderMode::Pinhole ? "pinhole" : "webgl"}
        };
    });

    std::cout << "[VideoPlugin] Initialized — video:open/close/bitrate/waveform/play/pause/seek/sync"
              << " + SharedBuffer pool + /video/stream HTTP"
              << " + render mode=" << (mode_ == RenderMode::Pinhole ? "pinhole" : "webgl")
              << std::endl;
}

void VideoPlugin::shutdown() {

    std::lock_guard<boost::fibers::mutex> lock(mtx_);
    close_file();
    pinhole_.reset();
    std::cout << "[VideoPlugin] Shutdown" << std::endl;
}

} // namespace videoplayer
