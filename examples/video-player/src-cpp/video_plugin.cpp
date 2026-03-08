// VideoPlugin — FFmpeg-based multimedia analysis + raw-frame decode pipeline
//
// Uses libavformat for container probing + packet iteration (bitrate),
// libavcodec + libswresample for audio waveform extraction, and
// libavcodec + libswscale for frame-by-frame RGBA decode → WebSocket binary push.

#include "video_plugin.h"

#include <anyar/types.h>

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

// ── Raw-frame decode loop (runs as a fibre) ─────────────────────────────────
//
// Opens a SEPARATE AVFormatContext for this file so seek/decode state is
// independent from analysis.  Converts every video frame to RGBA via
// libswscale and pushes it as a binary WebSocket message:
//
//   bytes  0–3   uint32_le  width
//   bytes  4–7   uint32_le  height
//   bytes  8–15  float64_le pts (seconds)
//   bytes 16–19  uint32_le  frame_number
//   bytes 20+    uint8[w*h*4]  RGBA pixel data

void VideoPlugin::run_decode_loop(std::shared_ptr<asyik::websocket> ws) {
    // ── Open a private format context ────────────────────────────────────
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, file_path_.c_str(), nullptr, nullptr) < 0) {
        try { ws->send_string(R"({"event":"error","message":"Cannot open file for decoding"})"); }
        catch (...) {}
        return;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        try { ws->send_string(R"({"event":"error","message":"Cannot find stream info"})"); }
        catch (...) {}
        return;
    }

    int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0) {
        avformat_close_input(&fmt);
        try { ws->send_string(R"({"event":"error","message":"No video stream found"})"); }
        catch (...) {}
        return;
    }

    // ── Open video decoder ───────────────────────────────────────────────
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

    // ── Allocate work frames ─────────────────────────────────────────────
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt   = av_packet_alloc();

    // swscale context is created lazily after the first frame is decoded
    // so we use the actual decoded pixel format (not codecpar->format which
    // can differ or be AV_PIX_FMT_NONE for some containers/codecs).
    SwsContext* sws = nullptr;
    AVFrame*    rgba = nullptr;
    uint32_t w = 0, h = 0;
    size_t pixel_bytes = 0;

    double fps = probe_.fps > 0 ? probe_.fps : 25.0;
    uint32_t fnum = 0;

    // Helper: (re-)initialise swscale + RGBA frame when decoded format is known
    auto init_sws = [&](const AVFrame* decoded) -> bool {
        if (sws) sws_freeContext(sws);
        if (rgba) av_frame_free(&rgba);

        sws = sws_getContext(
            decoded->width, decoded->height,
            static_cast<AVPixelFormat>(decoded->format),
            decoded->width, decoded->height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return false;

        rgba = av_frame_alloc();
        rgba->format = AV_PIX_FMT_RGBA;
        rgba->width  = decoded->width;
        rgba->height = decoded->height;
        if (av_frame_get_buffer(rgba, 32) < 0) {
            sws_freeContext(sws); sws = nullptr;
            av_frame_free(&rgba);
            return false;
        }

        w = static_cast<uint32_t>(decoded->width);
        h = static_cast<uint32_t>(decoded->height);
        pixel_bytes = static_cast<size_t>(w) * h * 4;
        return true;
    };

    // ── Helper: Decode the next video frame into `frame` ─────────────────
    //    Returns true on success, false only on genuine EOF.
    //    Handles EAGAIN from avcodec_send_packet, retries transient
    //    av_read_frame errors, and drains buffered frames first.
    auto decode_next = [&]() -> bool {
        // Try to receive a frame already queued in the decoder
        // (e.g. reordered B-frames from a previous send batch).
        if (avcodec_receive_frame(dec_ctx, frame) == 0)
            return true;

        int read_errors = 0;
        while (true) {
            int ret = av_read_frame(fmt, pkt);
            if (ret == AVERROR_EOF) return false;     // genuine end
            if (ret < 0) {
                // Transient demuxer error (common after rapid seeks).
                // Retry a few times before giving up.
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
                // Decoder input buffer full — drain a frame first.
                if (avcodec_receive_frame(dec_ctx, frame) == 0)
                    return true;
                // No frame ready yet, keep feeding packets.
            }
            // AVERROR_INVALIDDATA / other send errors: skip bad packet,
            // the decoder can usually recover after the next keyframe.

            if (avcodec_receive_frame(dec_ctx, frame) == 0)
                return true;
        }
    };

    // ── Helper: Convert current `frame` → RGBA into `dst` buffer ────────
    //    Writes 20-byte header + RGBA pixels.  Returns PTS on success,
    //    or –1.0 on fatal error.
    auto convert_to = [&](std::vector<uint8_t>& dst,
                          double fallback_pts) -> double {
        // Resolution-change guard
        if (static_cast<uint32_t>(frame->width)  != w ||
            static_cast<uint32_t>(frame->height) != h) {
            if (!init_sws(frame)) return -1.0;
            try {
                json dim = {{"event", "ready"}, {"width", w}, {"height", h}, {"fps", fps}};
                ws->send_string(dim.dump());
            } catch (...) { return -1.0; }
        }

        sws_scale(sws, frame->data, frame->linesize, 0,
                  static_cast<int>(h), rgba->data, rgba->linesize);

        double pts = (frame->pts != AV_NOPTS_VALUE)
            ? frame->pts * av_q2d(fmt->streams[vidx]->time_base)
            : fallback_pts;

        // Ensure buffer matches current dimensions
        if (dst.size() != 20 + pixel_bytes) {
            dst.resize(20 + pixel_bytes);
            std::memcpy(dst.data(),     &w, 4);
            std::memcpy(dst.data() + 4, &h, 4);
        }

        std::memcpy(dst.data() + 8,  &pts, 8);
        std::memcpy(dst.data() + 16, &fnum, 4);
        ++fnum;

        if (rgba->linesize[0] == static_cast<int>(w * 4)) {
            std::memcpy(dst.data() + 20, rgba->data[0], pixel_bytes);
        } else {
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(dst.data() + 20 + y * w * 4,
                            rgba->data[0] + y * rgba->linesize[0], w * 4);
        }
        return pts;
    };

    // ── Decode first frame for pixel-format detection ────────────────────
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

        if (!got_first || !init_sws(frame)) {
            av_packet_free(&pkt);
            av_frame_free(&frame);
            if (sws) sws_freeContext(sws);
            if (rgba) av_frame_free(&rgba);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&fmt);
            try { ws->send_string(R"({"event":"error","message":"Failed to decode first frame"})"); }
            catch (...) {}
            return;
        }
    }

    // ── Pre-allocate frame pool ──────────────────────────────────────────
    //
    //    Fixed-size circular buffer of ready-to-send binary messages.
    //    Allocated once, reused for the whole session → no malloc churn
    //    during playback, which eliminates the heap corruption.
    constexpr int POOL_CAP = 5;
    std::array<std::vector<uint8_t>, POOL_CAP> pool;
    std::array<double, POOL_CAP> pool_pts{};
    int pool_head = 0, pool_tail = 0, pool_count = 0;

    for (auto& b : pool) {
        b.resize(20 + pixel_bytes);
        std::memcpy(b.data(),     &w, 4);
        std::memcpy(b.data() + 4, &h, 4);
    }

    // One extra buffer for immediate sends (poster & seek previews)
    std::vector<uint8_t> imm(20 + pixel_bytes);
    std::memcpy(imm.data(),     &w, 4);
    std::memcpy(imm.data() + 4, &h, 4);

    // ── Send ready event + poster frame ──────────────────────────────────
    try {
        json ready = {{"event", "ready"}, {"width", w}, {"height", h}, {"fps", fps}};
        ws->send_string(ready.dump());
    } catch (...) { goto cleanup; }

    {
        double pts = convert_to(imm, 0.0);
        if (pts < 0) goto cleanup;
        try { ws->write_basic_buffer(imm); } catch (...) { goto cleanup; }
    }

    av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec_ctx);

    // ── Main audio-driven decode / send loop ─────────────────────────────
    //
    //  Architecture:
    //    1. Pre-decode video frames into `pool` (up to POOL_CAP ahead).
    //    2. Frontend audio element is the master clock; it periodically
    //       sends { cmd:"sync", time:<audioEl.currentTime> }.
    //    3. The reader fibre stores that in audio_time_.
    //    4. This loop sends the pool frame whose PTS best matches
    //       audio_time_, dropping frames the audio has already passed.
    {
        bool eos = false;
        const double half_frame = 0.5 / fps;
        bool was_playing = false;

        while (streaming_) {

            // ── Handle pending seek ──────────────────────────────────
            if (pending_seek_ >= 0) {
                double t = pending_seek_;
                pending_seek_ = -1.0;
                audio_time_   = -1.0;       // wait for fresh sync

                // Flush pool
                pool_head = pool_tail = pool_count = 0;
                eos = false;

                int64_t ts = static_cast<int64_t>(t * AV_TIME_BASE);
                av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(dec_ctx);

                // Fast-forward: decode WITHOUT sws_scale until we
                // reach a frame near the target.  This avoids the
                // very slow pool-churn catch-up when the keyframe is
                // far before the requested time.
                //
                // IMPORTANT: do NOT set eos here.  A decode failure
                // during fast-forward is usually transient (mmco /
                // reference-frame confusion after flush).  The normal
                // pool-fill path will retry and correctly determine
                // real EOF vs. transient decoder hiccup.
                {
                    AVRational vtb = fmt->streams[vidx]->time_base;
                    int skipped = 0;
                    bool ff_ok = true;
                    while (streaming_ && pending_seek_ < 0) {
                        if (!decode_next()) { ff_ok = false; break; }
                        double fpts = (frame->pts != AV_NOPTS_VALUE)
                            ? frame->pts * av_q2d(vtb) : t;
                        if (fpts >= t - half_frame) break; // close enough
                        ++skipped;
                        // Yield occasionally so reader fibre can process
                        // a NEW seek that may supersede this one.
                        if ((skipped & 0xF) == 0)
                            boost::this_fiber::sleep_for(
                                std::chrono::milliseconds(0));
                    }

                    // If fast-forward hit a decode error, re-seek and
                    // flush the decoder to give it a clean slate.  The
                    // pool-fill loop will attempt decoding from that
                    // keyframe position.
                    if (!ff_ok && pending_seek_ < 0) {
                        av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
                        avcodec_flush_buffers(dec_ctx);
                    }
                }

                // Send preview only when no newer seek has already
                // arrived and fast-forward left a valid frame.
                if (pending_seek_ < 0 && frame->data[0]) {
                    double pts = convert_to(imm, t);
                    if (pts >= 0) {
                        try { ws->write_basic_buffer(imm); } catch (...) { goto cleanup; }
                    }
                }

                // Always restart the loop after a seek so we re-check
                // pending_seek_ before falling through to pool-fill /
                // EOS — prevents false "ended" events and wasted
                // decode work at the wrong position.
                continue;
            }

            // ── Paused — idle wait ───────────────────────────────────
            if (!playing_) {
                was_playing = false;
                boost::this_fiber::sleep_for(std::chrono::milliseconds(30));
                continue;
            }

            // ── Just resumed playing (pause→play or ended→play) ─────
            //    Reset eos so pool-fill retries decoding.  Without this,
            //    a stale eos=true from a previous EOS or transient error
            //    causes an instant false "ended" event.
            if (!was_playing) {
                was_playing = true;
                if (eos) {
                    eos = false;
                    // Re-seek to the last known audio position so the
                    // demuxer is in a clean state.
                    double atime = audio_time_;
                    if (atime >= 0) {
                        int64_t ts = static_cast<int64_t>(atime * AV_TIME_BASE);
                        av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
                        avcodec_flush_buffers(dec_ctx);
                        pool_head = pool_tail = pool_count = 0;
                    }
                }
            }

            // ── Pre-decode to fill pool ──────────────────────────────
            while (pool_count < POOL_CAP && !eos && streaming_
                   && pending_seek_ < 0) {
                if (!decode_next()) { eos = true; break; }
                double pts = convert_to(pool[pool_tail], 0.0);
                if (pts < 0) goto cleanup;
                pool_pts[pool_tail] = pts;
                pool_tail = (pool_tail + 1) % POOL_CAP;
                ++pool_count;
                // Yield so reader fibre can deliver sync / control msgs
                boost::this_fiber::sleep_for(std::chrono::milliseconds(1));
            }

            // ── Audio-driven frame dispatch ──────────────────────────
            {
                double atime = audio_time_;
                if (atime < 0) {
                    // No audio sync yet — keep buffering but don't send
                    boost::this_fiber::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }

                // Drop frames the audio clock has already passed
                while (pool_count > 1) {
                    int next_idx = (pool_head + 1) % POOL_CAP;
                    if (pool_pts[pool_head] < atime - half_frame &&
                        pool_pts[next_idx]  <= atime + half_frame) {
                        pool_head = next_idx;
                        --pool_count;
                    } else {
                        break;
                    }
                }

                // Send head frame if its PTS is due
                if (pool_count > 0 &&
                    pool_pts[pool_head] <= atime + half_frame) {
                    try { ws->write_basic_buffer(pool[pool_head]); }
                    catch (...) { goto cleanup; }
                    pool_head = (pool_head + 1) % POOL_CAP;
                    --pool_count;
                }
            }

            // ── EOS: pool fully drained ──────────────────────────────
            if (eos && pool_count == 0) {
                // Before declaring the video ended, try one re-seek.
                // A transient demuxer/decoder error can set eos
                // incorrectly (especially after rapid seeks).  A
                // fresh seek + flush usually recovers.
                double atime = audio_time_;
                if (atime >= 0 && atime < probe_.duration - 1.0) {
                    int64_t ts2 = static_cast<int64_t>(atime * AV_TIME_BASE);
                    av_seek_frame(fmt, -1, ts2, AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(dec_ctx);
                    eos = false;
                    continue;   // retry pool-fill
                }
                try { ws->send_string(R"({"event":"ended"})"); } catch (...) {}
                playing_ = false;
                continue;
            }

            // Yield to let reader fibre process incoming messages
            boost::this_fiber::sleep_for(std::chrono::milliseconds(2));
        }
    }

cleanup:
    av_packet_free(&pkt);
    av_frame_free(&frame);
    if (rgba) av_frame_free(&rgba);
    if (sws) sws_freeContext(sws);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt);
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
    auto& cmds = ctx.commands;

    // ── Register the raw-frame WebSocket endpoint ─────────────────────────
    // Binary push: C++ decode-loop → RGBA frames → browser canvas
    // Text control: browser → { cmd: play|pause|seek|stop, time?: N }
    if (ctx.server) {
        ctx.server->on_websocket(
            "/video/frames",
            [this](auto ws, auto /*args*/) {
                // Stop any previous stream (only one client at a time)
                stop_streaming();
                streaming_ = true;
                playing_   = false;
                pending_seek_ = -1.0;
                audio_time_   = -1.0;

                // Launch a helper fibre that reads control messages from
                // the client and updates the shared flags.  This fibre is
                // detached — it exits when streaming_ becomes false or the
                // WebSocket closes.
                service_->execute([this, ws]() {
                    try {
                        while (streaming_) {
                            auto msg = ws->get_string();
                            auto j = json::parse(msg);
                            std::string cmd = j.at("cmd").template get<std::string>();

                            if (cmd == "play") {
                                playing_ = true;
                            } else if (cmd == "pause") {
                                playing_ = false;
                            } else if (cmd == "seek") {
                                pending_seek_ = j.at("time").template get<double>();
                            } else if (cmd == "sync") {
                                audio_time_ = j.at("time").template get<double>();
                            } else if (cmd == "stop") {
                                streaming_ = false;
                            }
                        }
                    } catch (...) {
                        // Connection closed or parse error
                    }
                    streaming_ = false;
                });

                // Run the decode loop IN THIS (handler) fibre.
                // The handler keeps the WebSocket alive for its entire
                // lifetime — LibAsyik only tears down the connection
                // after the handler returns, so the decode loop can
                // safely write binary frames until it's done.
                try { run_decode_loop(ws); } catch (...) {}
                streaming_ = false;
            }
        );
    }

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
        close_file();
        return {{"closed", true}};
    });

    std::cout << "[VideoPlugin] Initialized — video:open, video:bitrate, video:waveform, video:close"
              << " + /video/frames WS + /video/stream HTTP"
              << std::endl;
}

void VideoPlugin::shutdown() {
    stop_streaming();
    std::lock_guard<boost::fibers::mutex> lock(mtx_);
    close_file();
    std::cout << "[VideoPlugin] Shutdown" << std::endl;
}

} // namespace videoplayer
