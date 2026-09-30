// VideoPlugin — FFmpeg-based multimedia analysis + raw-frame decode pipeline
//
// Uses libavformat for container probing + packet iteration (bitrate),
// libavcodec + libswresample for audio waveform extraction, and
// libavcodec + optional libswscale for frame decode.
//
// Frames the renderers support natively (YUV420P, NV12, NV21, RGBA, BGRA,
// RGB24, GRAY8) are copied without conversion; anything else (10-bit, 4:2:2,
// palettised …) is converted to YUV420P, which is 1.5 bytes/pixel.
//
// All FFmpeg calls run on LibAsyik's worker pool through anyar::run_blocking();
// the service thread (IPC, events, HTTP) only schedules work and moves
// shared_ptrs around.

#include "video_plugin.h"

#include <anyar/event_bus.h>
#include <anyar/http_file.h>
#include <anyar/path.h>
#include <anyar/pinhole.h>
#include <anyar/shared_buffer.h>
#include <anyar/types.h>

#include <libasyik/http.hpp>
#include <libasyik/service.hpp>

#include <nlohmann/json.hpp>

#include <boost/fiber/operations.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

// ── FFmpeg C headers ────────────────────────────────────────────────────────
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

// FFmpeg 5.1 (libavutil 57.28) replaced channels/channel_layout with
// AVChannelLayout; FFmpeg 7 removed the old fields.  Support both: Ubuntu
// 22.04 ships 4.4, vcpkg ships 7/8.
#define VP_HAS_CH_LAYOUT (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100))

#if VP_HAS_CH_LAYOUT
namespace {
/// RAII AVChannelLayout (custom orders own heap memory).
struct ChannelLayout {
    AVChannelLayout l{};
    ChannelLayout() = default;
    ChannelLayout(const ChannelLayout&) = delete;
    ChannelLayout& operator=(const ChannelLayout&) = delete;
    ~ChannelLayout() { av_channel_layout_uninit(&l); }
};
} // namespace
#endif

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace videoplayer {

PlaybackControl::~PlaybackControl() = default;

AudioStream::~AudioStream() {
    if (!temp_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(anyar::path_from_utf8(temp_path), ec);
    }
}

namespace {

/// VIDEO_PLAYER_DEBUG=1 → timestamped trace of seeks, presentation and
/// stalls on stderr.  Cheap when off.
bool debug_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("VIDEO_PLAYER_DEBUG");
        return v && *v && std::string(v) != "0";
    }();
    return on;
}

double debug_now() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

#define VP_DEBUG(expr)                                                        \
    do {                                                                      \
        if (debug_enabled()) {                                                \
            std::ostringstream vp_os_;                                        \
            vp_os_ << std::fixed << std::setprecision(3) << "[vp " << debug_now() \
                   << "] " << expr << "\n";                                   \
            std::cerr << vp_os_.str() << std::flush;                          \
        }                                                                     \
    } while (0)

// ── RAII wrappers for FFmpeg objects ────────────────────────────────────────

struct FormatCloser  { void operator()(AVFormatContext* p) const { avformat_close_input(&p); } };
struct CodecFreer    { void operator()(AVCodecContext* p)  const { avcodec_free_context(&p); } };
struct FrameFreer    { void operator()(AVFrame* p)         const { av_frame_free(&p); } };
struct PacketFreer   { void operator()(AVPacket* p)        const { av_packet_free(&p); } };
struct SwrFreer      { void operator()(SwrContext* p)      const { swr_free(&p); } };
struct SwsFreer      { void operator()(SwsContext* p)      const { sws_freeContext(p); } };

using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecPtr  = std::unique_ptr<AVCodecContext, CodecFreer>;
using FramePtr  = std::unique_ptr<AVFrame, FrameFreer>;
using PacketPtr = std::unique_ptr<AVPacket, PacketFreer>;
using SwrPtr    = std::unique_ptr<SwrContext, SwrFreer>;
using SwsPtr    = std::unique_ptr<SwsContext, SwsFreer>;

std::string av_err(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

FormatPtr open_input(const std::string& path) {
    AVFormatContext* fc = nullptr;
    int ret = avformat_open_input(&fc, path.c_str(), nullptr, nullptr);
    if (ret < 0) throw std::runtime_error("Cannot open media file: " + av_err(ret));
    FormatPtr owned(fc);
    ret = avformat_find_stream_info(fc, nullptr);
    if (ret < 0) throw std::runtime_error("Cannot find stream info: " + av_err(ret));
    return owned;
}

CodecPtr open_decoder(AVStream* st) {
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) throw std::runtime_error("No decoder for codec");
    CodecPtr ctx(avcodec_alloc_context3(dec));
    if (!ctx) throw std::runtime_error("Out of memory");
    avcodec_parameters_to_context(ctx.get(), st->codecpar);
    ctx->pkt_timebase = st->time_base;
    ctx->thread_count = 0;   // let FFmpeg pick (frame/slice threads)
    if (avcodec_open2(ctx.get(), dec, nullptr) < 0) throw std::runtime_error("Cannot open decoder");
    return ctx;
}

/// Media timeline origin (seconds): browsers report <audio>.currentTime
/// relative to the container start, so all our timestamps are too.
double container_start(const AVFormatContext* fc) {
    return fc->start_time != AV_NOPTS_VALUE ? static_cast<double>(fc->start_time) / AV_TIME_BASE : 0.0;
}

std::optional<anyar::pixel_format> map_pix_fmt(int f) {
    switch (f) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P: return anyar::pixel_format::yuv420;
        case AV_PIX_FMT_NV12:     return anyar::pixel_format::nv12;
        case AV_PIX_FMT_NV21:     return anyar::pixel_format::nv21;
        case AV_PIX_FMT_RGBA:     return anyar::pixel_format::rgba;
        case AV_PIX_FMT_BGRA:     return anyar::pixel_format::bgra;
        case AV_PIX_FMT_RGB24:    return anyar::pixel_format::rgb;
        case AV_PIX_FMT_GRAY8:    return anyar::pixel_format::grayscale;
        default:                  return std::nullopt;
    }
}

// ── Probe ───────────────────────────────────────────────────────────────────

std::shared_ptr<MediaInfo> probe_file(const std::string& path) {
    auto fc = open_input(path);
    auto info = std::make_shared<MediaInfo>();
    info->path = path;
    auto& p = info->probe;
    p.duration = fc->duration != AV_NOPTS_VALUE ? static_cast<double>(fc->duration) / AV_TIME_BASE : 0.0;
    std::error_code ec;
    p.fileSizeBytes = static_cast<int64_t>(std::filesystem::file_size(anyar::path_from_utf8(path), ec));

    int v = av_find_best_stream(fc.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int a = av_find_best_stream(fc.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (v >= 0) {
        auto* st  = fc->streams[v];
        auto* par = st->codecpar;
        p.width  = par->width;
        p.height = par->height;
        const AVCodecDescriptor* d = avcodec_descriptor_get(par->codec_id);
        p.videoCodec = d ? d->name : "unknown";
        AVRational fr = st->avg_frame_rate.den > 0 ? st->avg_frame_rate : st->r_frame_rate;
        if (fr.den > 0 && fr.num > 0) p.fps = av_q2d(fr);
    }
    if (a >= 0) {
        auto* par = fc->streams[a]->codecpar;
        const AVCodecDescriptor* d = avcodec_descriptor_get(par->codec_id);
        p.audioCodec = d ? d->name : "unknown";
        p.sampleRate = par->sample_rate;
#if VP_HAS_CH_LAYOUT
        p.channels   = par->ch_layout.nb_channels;
#else
        p.channels   = par->channels;
#endif
    }
    return info;
}

// ── Bitrate analysis (worker thread) ────────────────────────────────────────
//
// Iterate all packets, bucket their byte sizes by time window.

BitrateData compute_bitrate(const std::string& path, double duration, double step) {
    if (step <= 0) step = 0.5;
    auto fc = open_input(path);
    const int vidx = av_find_best_stream(fc.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int aidx = av_find_best_stream(fc.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    const double origin = container_start(fc.get());

    const double dur = duration > 0 ? duration : 1.0;
    const int n = std::max(1, static_cast<int>(std::ceil(dur / step)));
    std::vector<int64_t> vb(n, 0), ab(n, 0);

    PacketPtr pkt(av_packet_alloc());
    while (av_read_frame(fc.get(), pkt.get()) >= 0) {
        const int idx = pkt->stream_index;
        if (idx == vidx || idx == aidx) {
            int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            double t = ts != AV_NOPTS_VALUE ? ts * av_q2d(fc->streams[idx]->time_base) - origin : 0.0;
            int bucket = std::clamp(static_cast<int>(t / step), 0, n - 1);
            (idx == vidx ? vb : ab)[bucket] += pkt->size;
        }
        av_packet_unref(pkt.get());
    }

    BitrateData out;
    out.timestamps.resize(n);
    out.videoBps.resize(n);
    out.audioBps.resize(n);
    for (int i = 0; i < n; ++i) {
        out.timestamps[i] = (i + 0.5) * step;          // bucket centre
        out.videoBps[i]   = vb[i] * 8.0 / step;
        out.audioBps[i]   = ab[i] * 8.0 / step;
    }
    return out;
}

// ── Waveform extraction (worker thread) ─────────────────────────────────────
//
// Decode audio → resample to mono 8 kHz float → reduce to N (min,max) pairs.
// The resampler is configured from the first DECODED frame (the decoder's
// real output format/rate/layout), not from codecpar, which may differ
// (e.g. HE-AAC SBR doubles the rate; some decoders output planar formats).

WaveformData compute_waveform(const std::string& path, int num_samples) {
    constexpr int kRate = 8000;
    if (num_samples <= 0) num_samples = 2000;
    WaveformData out;

    auto fc = open_input(path);
    const int aidx = av_find_best_stream(fc.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (aidx < 0) return out;
    auto dec = open_decoder(fc->streams[aidx]);

    SwrPtr swr;
    int swr_fmt = -1, swr_rate = 0;
#if VP_HAS_CH_LAYOUT
    ChannelLayout swr_layout;
#else
    uint64_t swr_layout = 0;
#endif
    std::vector<float> samples;
    std::vector<float> chunk;

    auto resample = [&](const AVFrame* f) {
#if VP_HAS_CH_LAYOUT
        ChannelLayout layout;
        if (f->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_copy(&layout.l, &f->ch_layout);
        } else {
            av_channel_layout_default(&layout.l, f->ch_layout.nb_channels);
        }
        if (!swr || f->format != swr_fmt || f->sample_rate != swr_rate ||
            av_channel_layout_compare(&layout.l, &swr_layout.l) != 0) {
            ChannelLayout mono;
            av_channel_layout_default(&mono.l, 1);
            SwrContext* s = nullptr;
            if (swr_alloc_set_opts2(&s, &mono.l, AV_SAMPLE_FMT_FLT, kRate,
                                    &layout.l, static_cast<AVSampleFormat>(f->format),
                                    f->sample_rate, 0, nullptr) < 0) {
                s = nullptr;
            }
            swr.reset(s);
            if (!swr || swr_init(swr.get()) < 0) throw std::runtime_error("Cannot initialize resampler");
            swr_fmt = f->format; swr_rate = f->sample_rate;
            av_channel_layout_uninit(&swr_layout.l);
            av_channel_layout_copy(&swr_layout.l, &layout.l);
        }
#else
        const uint64_t layout = f->channel_layout ? f->channel_layout
                                                  : av_get_default_channel_layout(f->channels);
        if (!swr || f->format != swr_fmt || f->sample_rate != swr_rate || layout != swr_layout) {
            swr.reset(swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_MONO, AV_SAMPLE_FMT_FLT, kRate,
                                         layout, static_cast<AVSampleFormat>(f->format),
                                         f->sample_rate, 0, nullptr));
            if (!swr || swr_init(swr.get()) < 0) throw std::runtime_error("Cannot initialize resampler");
            swr_fmt = f->format; swr_rate = f->sample_rate; swr_layout = layout;
        }
#endif
        const int cap = swr_get_out_samples(swr.get(), f ? f->nb_samples : 0) + 32;
        chunk.resize(static_cast<size_t>(std::max(cap, 32)));
        uint8_t* outp = reinterpret_cast<uint8_t*>(chunk.data());
        int got = swr_convert(swr.get(), &outp, static_cast<int>(chunk.size()),
                              const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
        if (got > 0) samples.insert(samples.end(), chunk.begin(), chunk.begin() + got);
    };

    PacketPtr pkt(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    auto drain = [&] {
        while (avcodec_receive_frame(dec.get(), frame.get()) == 0) {
            resample(frame.get());
            av_frame_unref(frame.get());
        }
    };
    while (av_read_frame(fc.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == aidx && avcodec_send_packet(dec.get(), pkt.get()) >= 0) drain();
        av_packet_unref(pkt.get());
    }
    avcodec_send_packet(dec.get(), nullptr);
    drain();
    if (swr) {   // flush buffered resampler output
        chunk.resize(static_cast<size_t>(swr_get_delay(swr.get(), kRate) + 64));
        uint8_t* outp = reinterpret_cast<uint8_t*>(chunk.data());
        int got = swr_convert(swr.get(), &outp, static_cast<int>(chunk.size()), nullptr, 0);
        if (got > 0) samples.insert(samples.end(), chunk.begin(), chunk.begin() + got);
    }

    const int total = static_cast<int>(samples.size());
    if (total == 0) return out;
    const int n = std::max(1, std::min(num_samples, total / 2));
    out.peaks.resize(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        // Proportional segment bounds so the whole signal is covered.
        const int start = static_cast<int>(static_cast<int64_t>(i) * total / n);
        const int end   = static_cast<int>(static_cast<int64_t>(i + 1) * total / n);
        float lo = 0, hi = 0;
        for (int j = start; j < end; ++j) {
            lo = std::min(lo, samples[j]);
            hi = std::max(hi, samples[j]);
        }
        out.peaks[i * 2]     = lo;
        out.peaks[i * 2 + 1] = hi;
    }
    return out;
}

// ── Audio-only remux (worker thread) ────────────────────────────────────────
//
// The <audio> element only needs the soundtrack; C++ decodes the video.
// Handing WebKit the full file makes its media pipeline demux (and buffer)
// the video track too — with high-bitrate video on a high-latency audio
// device, flushing seeks can then stall the pipeline for good.  Stream-copy
// the audio into Matroska (accepts any codec; no re-encode), shifted to the
// same timeline origin the decoder uses (container start time).

void remux_audio_only(const std::string& in_path, const std::string& out_path) {
    auto in = open_input(in_path);
    const int aidx = av_find_best_stream(in.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (aidx < 0) throw std::runtime_error("no audio stream");
    AVStream* ist = in->streams[aidx];

    AVFormatContext* oc_raw = nullptr;
    if (avformat_alloc_output_context2(&oc_raw, nullptr, "matroska", out_path.c_str()) < 0 || !oc_raw)
        throw std::runtime_error("cannot create audio remux output");
    struct OutCloser {
        void operator()(AVFormatContext* c) const {
            if (c->pb) avio_closep(&c->pb);
            avformat_free_context(c);
        }
    };
    std::unique_ptr<AVFormatContext, OutCloser> oc(oc_raw);

    AVStream* ost = avformat_new_stream(oc.get(), nullptr);
    if (!ost || avcodec_parameters_copy(ost->codecpar, ist->codecpar) < 0)
        throw std::runtime_error("cannot copy audio stream parameters");
    ost->codecpar->codec_tag = 0;
    ost->time_base = ist->time_base;
    if (avio_open(&oc->pb, out_path.c_str(), AVIO_FLAG_WRITE) < 0)
        throw std::runtime_error("cannot open " + out_path);
    if (avformat_write_header(oc.get(), nullptr) < 0) throw std::runtime_error("cannot write audio header");

    const int64_t origin = av_rescale_q(in->start_time != AV_NOPTS_VALUE ? in->start_time : 0,
                                        AV_TIME_BASE_Q, ist->time_base);
    PacketPtr pkt(av_packet_alloc());
    while (av_read_frame(in.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == aidx) {
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= origin;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= origin;
            if ((pkt->dts != AV_NOPTS_VALUE && pkt->dts < 0) || (pkt->pts != AV_NOPTS_VALUE && pkt->pts < 0)) {
                av_packet_unref(pkt.get());   // encoder priming before the origin
                continue;
            }
            pkt->stream_index = 0;
            av_packet_rescale_ts(pkt.get(), ist->time_base, ost->time_base);
            pkt->pos = -1;
            av_interleaved_write_frame(oc.get(), pkt.get());   // takes the reference
        }
        av_packet_unref(pkt.get());
    }
    av_write_trailer(oc.get());
}

// ── VideoDecoder — used from one worker job at a time ───────────────────────

class VideoDecoder {
public:
    explicit VideoDecoder(const std::string& path) : fmt_(open_input(path)) {
        vidx_ = av_find_best_stream(fmt_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vidx_ < 0) throw std::runtime_error("No video stream found");
        st_    = fmt_->streams[vidx_];
        ctx_   = open_decoder(st_);
        frame_.reset(av_frame_alloc());
        held_.reset(av_frame_alloc());
        conv_.reset(av_frame_alloc());
        pkt_.reset(av_packet_alloc());
        origin_ = container_start(fmt_.get());
        AVRational fr = st_->avg_frame_rate.den > 0 ? st_->avg_frame_rate : st_->r_frame_rate;
        fps_ = (fr.den > 0 && fr.num > 0) ? av_q2d(fr) : 25.0;
    }

    double fps() const { return fps_; }

    /// Decode the next video frame into frame_.  Drains the decoder at EOF
    /// so the last frames are not lost.  false = end of stream.
    bool next() {
        while (true) {
            int r = avcodec_receive_frame(ctx_.get(), frame_.get());
            if (r == 0) {
                last_pts_ = pts_of(frame_.get());
                return true;
            }
            if (r == AVERROR_EOF || draining_) return false;
            // r == EAGAIN (or a recoverable decode error): feed more input.
            r = av_read_frame(fmt_.get(), pkt_.get());
            if (r < 0) {
                avcodec_send_packet(ctx_.get(), nullptr);   // enter draining mode
                draining_ = true;
                if (avcodec_receive_frame(ctx_.get(), frame_.get()) == 0) {
                    last_pts_ = pts_of(frame_.get());
                    return true;
                }
                return false;
            }
            if (pkt_->stream_index == vidx_) avcodec_send_packet(ctx_.get(), pkt_.get());
            av_packet_unref(pkt_.get());
        }
    }

    /// Seek so that frame_ holds the first frame with pts ≥ t − tolerance
    /// (or the last frame if the stream ends first).  false = nothing decodable.
    bool seek_to(double t, double tolerance) {
        const double target = std::max(0.0, t);

        // Land on a keyframe at or before the target.  Some demuxers (MPEG-TS,
        // streams without an index) may land AFTER it even with
        // AVSEEK_FLAG_BACKWARD — the decoder then skips to the next IDR — so
        // back off progressively, and finally restart from the first byte.
        bool got = false;
        for (double back : {0.0, 1.0, 3.0, 10.0, 30.0, -1.0}) {
            if (back < 0) seek_to_file_start();
            else seek_raw(std::max(0.0, target - back));
            got = next();
            if (got && pts_of(frame_.get()) <= target + tolerance) break;
        }
        if (!got) return false;

        // Decode forward to the target; keep the most recent frame in held_
        // so hitting EOF still leaves us something to show.
        av_frame_unref(held_.get());
        do {
            av_frame_unref(held_.get());
            av_frame_move_ref(held_.get(), frame_.get());
            if (pts_of(held_.get()) >= target - tolerance) break;
        } while (next());
        av_frame_unref(frame_.get());
        av_frame_move_ref(frame_.get(), held_.get());
        last_pts_ = pts_of(frame_.get());
        return true;
    }

    /// Copy the current frame into a mailbox frame (converting if needed).
    std::shared_ptr<anyar::Frame> to_frame(anyar::FrameMailbox& mailbox) {
        const AVFrame* src = frame_.get();
        auto fmt = map_pix_fmt(src->format);
        if (!fmt) {
            // Unsupported layout → YUV420P via swscale.
            sws_.reset(sws_getCachedContext(sws_.release(),
                src->width, src->height, static_cast<AVPixelFormat>(src->format),
                src->width, src->height, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr));
            if (!sws_) throw std::runtime_error("Unsupported pixel format");
            if (conv_->width != src->width || conv_->height != src->height) {
                av_frame_unref(conv_.get());
                conv_->format = AV_PIX_FMT_YUV420P;
                conv_->width  = src->width;
                conv_->height = src->height;
                if (av_frame_get_buffer(conv_.get(), 0) < 0) throw std::runtime_error("Out of memory");
            }
            sws_scale(sws_.get(), src->data, src->linesize, 0, src->height,
                      conv_->data, conv_->linesize);
            src = conv_.get();
            fmt = anyar::pixel_format::yuv420;
        }

        const size_t bytes = anyar::pixel_format_byte_size(*fmt, src->width, src->height);
        const int need = av_image_get_buffer_size(static_cast<AVPixelFormat>(src->format),
                                                  src->width, src->height, 1);
        if (need < 0 || static_cast<size_t>(need) != bytes) {
            throw std::runtime_error("Unexpected frame layout");
        }
        auto out = mailbox.acquire(bytes);
        av_image_copy_to_buffer(out->data.data(), static_cast<int>(bytes),
                                src->data, src->linesize,
                                static_cast<AVPixelFormat>(src->format),
                                src->width, src->height, 1);
        out->width  = src->width;
        out->height = src->height;
        out->format = *fmt;
        out->pts    = last_pts_;
        return out;
    }

private:
    void seek_raw(double media_time) {
        const double abs_t = media_time + origin_;
        const int64_t ts = av_rescale_q(static_cast<int64_t>(abs_t * AV_TIME_BASE),
                                        AV_TIME_BASE_Q, st_->time_base);
        if (av_seek_frame(fmt_.get(), vidx_, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            av_seek_frame(fmt_.get(), -1, static_cast<int64_t>(abs_t * AV_TIME_BASE),
                          AVSEEK_FLAG_BACKWARD);
        }
        avcodec_flush_buffers(ctx_.get());
        draining_ = false;
    }

    void seek_to_file_start() {
        const bool byte_ok = !(fmt_->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                             av_seek_frame(fmt_.get(), -1, 0, AVSEEK_FLAG_BYTE) >= 0;
        if (!byte_ok) {
            av_seek_frame(fmt_.get(), -1, fmt_->start_time != AV_NOPTS_VALUE ? fmt_->start_time : 0,
                          AVSEEK_FLAG_BACKWARD);
        }
        avcodec_flush_buffers(ctx_.get());
        draining_ = false;
    }

    double pts_of(const AVFrame* f) const {
        int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        if (ts == AV_NOPTS_VALUE) return last_pts_ + 1.0 / fps_;
        return ts * av_q2d(st_->time_base) - origin_;
    }

    FormatPtr fmt_;
    AVStream* st_ = nullptr;
    int       vidx_ = -1;
    CodecPtr  ctx_;
    FramePtr  frame_, held_, conv_;
    PacketPtr pkt_;
    SwsPtr    sws_;
    double    origin_   = 0.0;
    double    fps_      = 25.0;
    double    last_pts_ = 0.0;
    bool      draining_ = false;
};

} // namespace

// ── Events ──────────────────────────────────────────────────────────────────

void VideoPlugin::emit(const std::string& event, const json& payload) {
    if (events_) events_->emit(event, payload);
}

// ── Pinhole binding ─────────────────────────────────────────────────────────
//
// The render callback runs on the GTK main thread.  It only touches the
// mailbox (captured by shared_ptr, so it cannot dangle) and holds the frame
// it draws for the whole draw: the decoder can never recycle or free it
// mid-upload.

void VideoPlugin::set_pinhole(std::shared_ptr<anyar::Pinhole> pin) {
    pinhole_ = std::move(pin);
    if (!pinhole_) return;
    pinhole_->on_render([mailbox = mailbox_](anyar::PinholeRenderContext& ctx) {
        ctx.clear(0.0f, 0.0f, 0.0f, 1.0f);   // opaque letterbox bars
        if (auto frame = mailbox->latest()) ctx.draw_frame(*frame);
    });
}

// ── Session lifecycle ───────────────────────────────────────────────────────

void VideoPlugin::start_playback(std::shared_ptr<const MediaInfo> media) {
    auto pb = std::make_shared<PlaybackControl>();
    pb->session = ++session_counter_;
    playback_ = pb;
    decode_task_.start(service_, [this, media, pb](anyar::StopToken st) {
        run_decode_loop(st, media, pb);
    });
}

void VideoPlugin::stop_playback() {
    if (!decode_task_.stop(5s)) {
        std::cerr << "[VideoPlugin] decode loop did not stop within 5s" << std::endl;
    }
    playback_.reset();
    mailbox_->clear();
    if (pinhole_) pinhole_->request_redraw();   // show black instead of a stale frame
}

// ── Audio stream for the <audio> element ────────────────────────────────────

std::shared_ptr<AudioStream> VideoPlugin::prepare_audio_stream(std::shared_ptr<const MediaInfo> media) {
    auto a = std::make_shared<AudioStream>();
    if (!media->has_audio() || !media->has_video()) {   // already audio-only (or silent)
        boost::fibers::promise<std::string> p;
        p.set_value(media->path);
        a->ready = p.get_future().share();
        return a;
    }
    // Per-process random tag (not getpid(): POSIX-only) + per-open counter.
    static const std::string process_tag = std::to_string(std::random_device{}());
    static std::atomic<uint64_t> counter{0};
    a->temp_path = anyar::path_to_utf8(
        std::filesystem::temp_directory_path() /
        ("anyar-video-player-" + process_tag + "-" + std::to_string(++counter) + ".mka"));
    // Remux on the worker pool; /video/stream waits on the future.
    a->ready = service_->async([in = media->path, out = a->temp_path]() -> std::string {
        const double t0 = debug_now();
        try {
            remux_audio_only(in, out);
            VP_DEBUG("audio-only remux ready in " << (debug_now() - t0) * 1000 << " ms: " << out);
            return out;
        } catch (const std::exception& e) {
            std::cerr << "[VideoPlugin] audio remux failed (" << e.what()
                      << "); serving the original file to <audio>" << std::endl;
            return in;
        }
    }).share();
    return a;
}

// ── Frame presentation ──────────────────────────────────────────────────────

void VideoPlugin::present_frame(PlaybackControl& pb, const std::shared_ptr<anyar::Frame>& frame,
                                bool preview) {
    if (mode_ == RenderMode::Pinhole) {
        mailbox_->publish(frame);
        if (pinhole_) pinhole_->request_redraw();
        if (preview) emit("video:frame-pts", {{"pts", frame->pts}});
        return;
    }

    // WebGL: copy into a shared-memory slot and notify JS.  Each session
    // (and each resolution) gets its own pool name, so a late
    // video:pool-release for an old buffer can never touch a new one.
    const size_t bytes = frame->data.size();
    if (!pb.pool || pb.pool->buffer_size() != bytes) {
        pb.pool.reset();
        static uint64_t pool_gen = 0;
        pb.pool = std::make_unique<anyar::SharedBufferPool>(
            "video-frames-" + std::to_string(pb.session) + "-" + std::to_string(++pool_gen),
            bytes, 4);
    }
    anyar::SharedBuffer* buf = pb.pool->try_acquire_write();
    if (!buf) return;   // renderer behind → drop this frame rather than stall
    std::memcpy(buf->data(), frame->data.data(), bytes);
    pb.pool->release_write(*buf, "{}");
    emit("buffer:ready", {
        {"name", buf->name()},
        {"pool", pb.pool->base_name()},
        {"url", "anyar-shm://" + buf->name()},
        {"id", buf->id()},
        {"size", bytes},
        {"metadata", {
            {"width", frame->width},
            {"height", frame->height},
            {"pts", frame->pts},
            {"format", anyar::pixel_format_name(frame->format)},
        }},
    });
}

// ── Decode loop (BackgroundTask fiber) ──────────────────────────────────────
//
// Clock: the frontend <audio> element is the master clock and reports it via
// video:sync.  Video-only files use a local wall clock instead.  Frames are
// pre-decoded into a small queue and presented when their PTS is due; late
// frames are dropped.  Seeks decode from the preceding keyframe to the exact
// target, show that frame, then acknowledge with video:seeked so the
// frontend can start audio from the same point.

void VideoPlugin::run_decode_loop(anyar::StopToken stop,
                                  std::shared_ptr<const MediaInfo> media,
                                  std::shared_ptr<PlaybackControl> pb) {
    std::unique_ptr<VideoDecoder> dec;
    std::shared_ptr<anyar::Frame> first;
    try {
        dec = anyar::run_blocking(service_, [&] {
            auto d = std::make_unique<VideoDecoder>(media->path);
            if (!d->next()) throw std::runtime_error("Failed to decode first frame");
            first = d->to_frame(*mailbox_);
            return d;
        });
    } catch (const std::exception& e) {
        emit("video:error", {{"message", e.what()}});
        return;
    }
    if (stop.stop_requested()) return;

    const double fps        = dec->fps();
    const double half_frame = 0.5 / fps;
    const bool   has_audio  = media->has_audio();

    emit("video:ready", {{"width", first->width}, {"height", first->height},
                         {"fps", fps}, {"format", anyar::pixel_format_name(first->format)}});
    present_frame(*pb, first, /*preview=*/true);
    first.reset();

    // Decode + convert one frame on the worker pool; errors end the stream.
    auto decode_one = [&]() -> std::shared_ptr<anyar::Frame> {
        try {
            const double d0 = debug_now();
            auto f = anyar::run_blocking(service_, [&]() -> std::shared_ptr<anyar::Frame> {
                if (!dec->next()) return nullptr;
                return dec->to_frame(*mailbox_);
            });
            const double dt = debug_now() - d0;
            if (dt > 0.1) VP_DEBUG("slow decode: " << dt * 1000 << " ms");
            if (!f) VP_DEBUG("decoder reached end of stream");
            return f;
        } catch (const std::exception& e) {
            emit("video:error", {{"message", std::string("Decode failed: ") + e.what()}});
            return nullptr;
        }
    };

    // Wall clock (video-only files)
    using clock = std::chrono::steady_clock;
    double wall_offset  = 0.0;
    bool   wall_running = false;
    clock::time_point wall_start;
    auto wall_now = [&] {
        return wall_offset + (wall_running
            ? std::chrono::duration<double>(clock::now() - wall_start).count() : 0.0);
    };

    constexpr size_t kQueueCap = 5;
    std::deque<std::shared_ptr<anyar::Frame>> queue;
    bool eos = false;
    bool ended_emitted = false;

    // Debug: periodic status + stall detection while playing.
    double last_present_wall = debug_now(), last_status_wall = 0.0, last_presented_pts = -1.0;
    auto debug_tick = [&](double now_t) {
        if (!debug_enabled()) return;
        const double w = debug_now();
        const bool stalled = pb->playing && w - last_present_wall > 1.0;
        if (w - last_status_wall >= (stalled ? 0.5 : 2.0)) {
            last_status_wall = w;
            VP_DEBUG((stalled ? "STALL " : "status ") << "clock=" << now_t
                     << " last_pts=" << last_presented_pts << " queue=" << queue.size()
                     << " [" << (queue.empty() ? -1.0 : queue.front()->pts) << ".."
                     << (queue.empty() ? -1.0 : queue.back()->pts) << "] eos=" << eos
                     << " playing=" << pb->playing);
        }
    };

    while (!stop.stop_requested()) {
        // ── Seek ──────────────────────────────────────────────────────────
        if (pb->pending_seek >= 0) {
            const double   t  = pb->pending_seek;
            const uint64_t id = pb->seek_id;
            pb->pending_seek = -1.0;
            pb->audio_time   = -1.0;   // wait for the frontend's post-seek clock
            queue.clear();
            eos = false;
            ended_emitted = false;

            VP_DEBUG("seek id=" << id << " t=" << t << " start");
            const double seek_t0 = debug_now();
            std::shared_ptr<anyar::Frame> f;
            try {
                f = anyar::run_blocking(service_, [&]() -> std::shared_ptr<anyar::Frame> {
                    if (!dec->seek_to(t, half_frame)) return nullptr;
                    return dec->to_frame(*mailbox_);
                });
            } catch (const std::exception& e) {
                emit("video:error", {{"message", std::string("Seek failed: ") + e.what()}});
            }
            VP_DEBUG("seek id=" << id << " done in " << (debug_now() - seek_t0) * 1000 << " ms, frame pts="
                     << (f ? f->pts : -1.0) << (pb->pending_seek >= 0 ? " (superseded)" : ""));
            if (stop.stop_requested()) break;
            if (pb->pending_seek >= 0) continue;   // superseded — only ack the newest

            const double shown = f ? f->pts : t;
            if (f) present_frame(*pb, f, /*preview=*/true);
            last_present_wall = debug_now();
            wall_offset  = t;
            wall_running = false;
            emit("video:seeked", {{"id", id}, {"time", t}, {"pts", shown}});
            continue;
        }

        // ── Paused ────────────────────────────────────────────────────────
        if (!pb->playing) {
            if (wall_running) { wall_offset = wall_now(); wall_running = false; }
            boost::this_fiber::sleep_for(15ms);
            continue;
        }
        if (!has_audio && !wall_running) { wall_start = clock::now(); wall_running = true; }

        // ── Keep the queue topped up (one decode per iteration so control
        //    changes are seen within one frame time) ──────────────────────
        bool decoded = false;
        if (queue.size() < kQueueCap && !eos) {
            if (auto f = decode_one()) queue.push_back(std::move(f));
            else eos = true;
            decoded = true;
            if (stop.stop_requested() || pb->pending_seek >= 0 || !pb->playing) continue;
        }

        // ── Present whatever is due ───────────────────────────────────────
        const double now_t = has_audio ? pb->audio_time : wall_now();
        if (now_t >= 0) {
            while (queue.size() > 1 && queue[1]->pts <= now_t + half_frame) {
                queue.pop_front();   // late: a newer frame is already due
            }
            if (!queue.empty() && queue.front()->pts <= now_t + half_frame) {
                present_frame(*pb, queue.front(), /*preview=*/false);
                last_presented_pts = queue.front()->pts;
                last_present_wall  = debug_now();
                queue.pop_front();
            }
        }
        debug_tick(now_t);

        // ── End of stream ────────────────────────────────────────────────
        if (eos && queue.empty() && !ended_emitted) {
            ended_emitted = true;
            // With audio, the <audio> element's `ended` drives the UI; we
            // just hold the last frame.  Video-only: we own the clock.
            if (!has_audio) {
                pb->playing = false;
                emit("video:ended", json::object());
            }
        }

        if (!decoded || queue.size() >= kQueueCap || eos) {
            boost::this_fiber::sleep_for(4ms);
        } else {
            boost::this_fiber::yield();
        }
    }
}

// ── Plugin initialization ───────────────────────────────────────────────────

void VideoPlugin::initialize(anyar::PluginContext& ctx) {
    service_ = ctx.service;
    events_  = &ctx.events;
    auto& cmds = ctx.commands;

    // ── /video/stream — Range-aware file streaming for the <audio> element.
    //    The streaming serve_file() answers every range in full (WebKit's
    //    media loader requires that) while writing it in small chunks read
    //    off the service thread.
    if (ctx.server) {
        // weak_ptr: the route is owned by the server — no reference cycle.
        std::weak_ptr<asyik::http_server<asyik::http_stream_type>> weak_server = ctx.server;
        ctx.server->on_http_request(
            "/video/stream", "GET",
            [this, weak_server](asyik::http_request_ptr req, asyik::http_route_args) {
                auto audio = audio_;   // snapshot; open/close may swap it meanwhile
                if (!audio) {
                    req->response.result(404);
                    req->response.body = "No video file open";
                    return;
                }
                const std::string path = audio->ready.get();   // waits for the remux (fiber-suspend)
                auto rng = req->headers.find("Range");
                VP_DEBUG("http /video/stream " << path << " Range: "
                         << (rng != req->headers.end() ? std::string(rng->value()) : "(none)"));
                anyar::serve_file(weak_server.lock(), req, path);
                VP_DEBUG("http /video/stream done");
            });
    }

    // ── video:open ──────────────────────────────────────────────────────
    cmds.add("video:open", [this](const json& args) -> json {
        std::lock_guard<boost::fibers::mutex> lock(mtx_);

        std::string path = args.at("path").get<std::string>();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(anyar::path_from_utf8(path), ec)) {
            throw std::runtime_error("File not found: " + path);
        }
        // Keep UTF-8 throughout: FFmpeg, serve_file and the JSON replies all
        // take UTF-8 paths (path::string() would be ANSI on Windows).
        path = anyar::path_to_utf8(std::filesystem::canonical(anyar::path_from_utf8(path)));

        // Fully stop the previous session BEFORE touching shared state.
        stop_playback();
        media_.reset();
        audio_.reset();
        bitrate_for_.reset();
        waveform_for_.reset();

        std::shared_ptr<const MediaInfo> media =
            anyar::run_blocking(service_, [path] { return probe_file(path); });
        media_ = media;
        audio_ = prepare_audio_stream(media);
        if (media->has_video()) start_playback(media);   // paused; shows first frame

        const auto& p = media->probe;
        return {
            {"url",        "/video/stream"},
            {"duration",   p.duration},
            {"width",      p.width},
            {"height",     p.height},
            {"videoCodec", p.videoCodec},
            {"audioCodec", p.audioCodec},
            {"fps",        p.fps},
            {"sampleRate", p.sampleRate},
            {"channels",   p.channels},
            {"fileSize",   p.fileSizeBytes},
        };
    });

    // ── video:close ─────────────────────────────────────────────────────
    cmds.add("video:close", [this](const json&) -> json {
        std::lock_guard<boost::fibers::mutex> lock(mtx_);
        stop_playback();
        media_.reset();
        audio_.reset();
        bitrate_for_.reset();
        waveform_for_.reset();
        return {{"closed", true}};
    });

    // ── video:bitrate / video:waveform — computed on the worker pool with
    //    their own demuxer, so they run in parallel and never block IPC.
    cmds.add("video:bitrate", [this](const json& args) -> json {
        auto media = media_;
        if (!media) throw std::runtime_error("No file open");
        if (bitrate_for_ != media) {
            const double step = args.value("step", 0.5);
            auto data = anyar::run_blocking(service_, [media, step] {
                return compute_bitrate(media->path, media->probe.duration, step);
            });
            if (media_ == media) { bitrate_ = std::move(data); bitrate_for_ = media; }
            else throw std::runtime_error("File changed during analysis");
        }
        return {{"timestamps", bitrate_.timestamps},
                {"videoBps",   bitrate_.videoBps},
                {"audioBps",   bitrate_.audioBps}};
    });

    cmds.add("video:waveform", [this](const json& args) -> json {
        auto media = media_;
        if (!media) throw std::runtime_error("No file open");
        if (waveform_for_ != media) {
            const int samples = args.value("samples", 2000);
            auto data = anyar::run_blocking(service_, [media, samples] {
                return compute_waveform(media->path, samples);
            });
            if (media_ == media) { waveform_ = std::move(data); waveform_for_ = media; }
            else throw std::runtime_error("File changed during analysis");
        }
        return {{"peaks", waveform_.peaks}};
    });

    // ── Transport ───────────────────────────────────────────────────────
    cmds.add("video:play", [this](const json&) -> json {
        VP_DEBUG("cmd play");
        if (playback_) playback_->playing = true;
        return {{"ok", true}};
    });

    cmds.add("video:pause", [this](const json&) -> json {
        VP_DEBUG("cmd pause");
        if (playback_) playback_->playing = false;
        return {{"ok", true}};
    });

    cmds.add("video:seek", [this](const json& args) -> json {
        if (playback_) {
            playback_->pending_seek = std::max(0.0, args.at("time").get<double>());
            playback_->seek_id      = args.value("id", uint64_t{0});
            VP_DEBUG("cmd seek id=" << playback_->seek_id << " t=" << playback_->pending_seek);
        }
        return {{"ok", true}};
    });

    cmds.add("video:sync", [this](const json& args) -> json {
        const double t = args.at("time").get<double>();
        if (debug_enabled()) {   // log clock gaps/jumps only, not every sync
            static double last_t = -1, last_wall = 0;
            const double w = debug_now();
            if (last_t < 0 || std::abs(t - last_t) > 0.5 || w - last_wall > 0.5) {
                VP_DEBUG("sync t=" << t << " (prev " << last_t << ", " << (w - last_wall) * 1000 << " ms ago)");
            }
            last_t = t; last_wall = w;
        }
        if (playback_) playback_->audio_time = t;
        return {{"ok", true}};
    });

    cmds.add("video:pool-release", [this](const json& args) -> json {
        if (playback_ && playback_->pool) {
            playback_->pool->release_read(args.at("name").get<std::string>());
        }
        return {{"ok", true}};
    });

    cmds.add("video:get-mode", [this](const json&) -> json {
        return {{"mode", mode_ == RenderMode::Pinhole ? "pinhole" : "webgl"}};
    });

    std::cout << "[VideoPlugin] Initialized — render mode="
              << (mode_ == RenderMode::Pinhole ? "pinhole" : "webgl") << std::endl;
}

// Runs on the main thread while the service thread is still alive.
void VideoPlugin::shutdown() {
    std::lock_guard<boost::fibers::mutex> lock(mtx_);
    stop_playback();   // joins the decode fiber (service thread still alive)
    pinhole_.reset();
    std::cout << "[VideoPlugin] Shutdown" << std::endl;
}

} // namespace videoplayer
