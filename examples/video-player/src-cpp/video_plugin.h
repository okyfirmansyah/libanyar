#pragma once

// VideoPlugin — FFmpeg-based video analysis, HTTP streaming, and raw frame decoding
//
// Commands:
//   video:open      { path }         → { url, duration, width, height, videoCodec, audioCodec, ... }
//   video:bitrate   { step? }        → { timestamps[], videoBps[], audioBps[] }
//   video:waveform  { samples? }     → { peaks[] }
//   video:play      {}               → { ok }
//   video:pause     {}               → { ok }
//   video:seek      { time }         → { ok }
//   video:sync      { time }         → { ok }
//   video:close     {}               → {}
//
// Events:
//   buffer:ready    { name, pool, url, size, metadata }  — per-frame notification
//   video:ended     {}                                   — end of stream
//   video:ready     { width, height, fps }               — first frame decoded
//
// Shared Memory:
//   Pool "video-frames" (3 buffers) served via anyar-shm:// URI scheme

#include <anyar/plugin.h>
#include <anyar/shared_buffer.h>

#include <boost/fiber/mutex.hpp>
#include <boost/fiber/condition_variable.hpp>
#include <string>
#include <memory>

// Forward declarations — avoid leaking FFmpeg / libasyik headers into consumers
struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;

namespace anyar { class EventBus; }

namespace videoplayer {

/// Data extracted from probing a media file
struct ProbeResult {
    double       duration   = 0.0;   // seconds
    int          width      = 0;
    int          height     = 0;
    std::string  videoCodec;
    std::string  audioCodec;
    int64_t      fileSizeBytes = 0;
    double       fps        = 0.0;
    int          sampleRate = 0;
    int          channels   = 0;
};

/// Per-file bitrate analysis result
struct BitrateData {
    std::vector<double>  timestamps;   // seconds
    std::vector<double>  videoBps;     // bits per second per bucket
    std::vector<double>  audioBps;
};

/// Waveform peaks (normalised –1..1)
struct WaveformData {
    std::vector<float>   peaks;        // min/max interleaved: [min0, max0, min1, max1, …]
};

/// Shared state for a frame streaming WebSocket session
struct FrameStreamState {
    boost::fibers::mutex mtx;
    bool playing      = false;
    bool seek_pending  = false;
    double seek_time   = 0.0;
    bool stop          = false;
};

class VideoPlugin : public anyar::IAnyarPlugin {
public:
    std::string name() const override { return "video"; }
    void initialize(anyar::PluginContext& ctx) override;
    void shutdown() override;

private:
    // Currently opened file state (single-file model for simplicity)
    boost::fibers::mutex   mtx_;
    AVFormatContext*        fmt_ctx_     = nullptr;
    std::string            file_path_;
    ProbeResult            probe_;
    int                    video_stream_ = -1;
    int                    audio_stream_ = -1;

    // Cached analysis (computed once on open or first request)
    bool                   bitrate_ready_   = false;
    BitrateData            bitrate_;
    bool                   waveform_ready_  = false;
    WaveformData           waveform_;

    // Service pointer for launching async fibres after initialization
    asyik::service_ptr service_;

    // Event bus for emitting events to frontend
    anyar::EventBus* events_ = nullptr;

    // ── Frame-streaming state (decode loop ↔ control commands) ───────────
    bool   streaming_    = false;   // decode-loop fibre alive
    bool   playing_      = false;   // actively pushing frames
    double pending_seek_ = -1.0;    // next seek target (–1 = none)
    double audio_time_   = -1.0;    // audio clock from frontend (–1 = no sync)

    // SharedBufferPool for zero-copy frame delivery
    std::unique_ptr<anyar::SharedBufferPool> frame_pool_;

    // ── Internal helpers ────────────────────────────────────────────────────
    void open_file(const std::string& path);
    void close_file();
    void compute_bitrate(double step);
    void compute_waveform(int num_samples);
    void register_stream_route();
    void stop_streaming();
    void run_decode_loop();
};

} // namespace videoplayer
