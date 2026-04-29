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
#include <mutex>
#include <string>
#include <memory>

// Forward declarations — avoid leaking FFmpeg / libasyik headers into consumers
struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;

namespace anyar { class EventBus; class Pinhole; }

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

/// Renderer selected at startup.  WebGL routes raw frames to the JS canvas
/// via the SharedBufferPool + buffer:ready event; Pinhole routes them to the
/// native GtkGLArea overlay (no JS in the hot path).
enum class RenderMode { WebGL, Pinhole };

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
    /// @param mode  Default render mode reported via `video:get-mode`.
    ///              Pinhole mode also requires set_pinhole() to be called
    ///              before the first `video:play` command.
    explicit VideoPlugin(RenderMode mode = RenderMode::Pinhole) : mode_(mode) {}

    std::string name() const override { return "video"; }
    void initialize(anyar::PluginContext& ctx) override;
    void shutdown() override;

    /// Bind a Pinhole to drive in pinhole mode.  Must be called from main
    /// before `video:play`; the plugin installs an `on_render` callback
    /// that draws whatever frame the decode loop most recently published.
    /// No-op in WebGL mode.
    void set_pinhole(std::shared_ptr<anyar::Pinhole> pin);

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

    // ── Pinhole-mode state ───────────────────────────────────────────────
    RenderMode                       mode_       = RenderMode::Pinhole;
    std::shared_ptr<anyar::Pinhole>  pinhole_;

    // Latest frame published to the pinhole.  Owned by `frame_pool_`;
    // we hold the WRITING-side reference until the next frame swaps in,
    // at which point the previous slot is recycled (release_write+release_read).
    // Guarded by `latest_mu_` (std::mutex — locked from both fiber thread
    // and GTK main thread, neither of which may use boost::fibers::mutex).
    std::mutex                       latest_mu_;
    anyar::SharedBuffer*             latest_buf_ = nullptr;
    int                              latest_w_   = 0;
    int                              latest_h_   = 0;
    std::string                      latest_fmt_;  // "yuv420" / "nv12" / "rgba" / ...

    // ── Internal helpers ────────────────────────────────────────────────────
    void open_file(const std::string& path);
    void close_file();
    void compute_bitrate(double step);
    void compute_waveform(int num_samples);
    void register_stream_route();
    void stop_streaming();
    void run_decode_loop();
    /// Drop the currently-held latest frame back into the pool (FREE).
    /// Caller must NOT hold latest_mu_.
    void release_latest_frame();
};

} // namespace videoplayer
