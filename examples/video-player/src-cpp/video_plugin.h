#pragma once

// VideoPlugin — FFmpeg-based video analysis, HTTP streaming, and raw frame decoding
//
// Commands:
//   video:open         { path }         → { url, duration, width, height, videoCodec, audioCodec, ... }
//                                           (stops any previous playback, starts a paused session
//                                            and shows the first frame)
//   video:bitrate      { step? }        → { timestamps[], videoBps[], audioBps[] }
//   video:waveform     { samples? }     → { peaks[] }
//   video:play         {}               → { ok }
//   video:pause        {}               → { ok }
//   video:seek         { time, id }     → { ok }   (acknowledged by `video:seeked`)
//   video:sync         { time }         → { ok }   (audio master clock from the frontend)
//   video:pool-release { name }         → { ok }   (WebGL mode)
//   video:close        {}               → { closed }
//   video:get-mode     {}               → { mode: "pinhole" | "webgl" }
//
// Events:
//   video:ready      { width, height, fps, format }      — first frame decoded
//   video:seeked     { id, time, pts }                   — seek done, preview frame shown
//   video:frame-pts  { pts }                             — preview frame shown (pinhole mode)
//   buffer:ready     { name, pool, url, size, metadata } — per-frame (WebGL mode)
//   video:ended      {}                                  — end of stream (video-only files)
//   video:error      { message }
//
// Threading: every command and the decode loop run as fibers on the service
// thread; all FFmpeg work runs on the worker pool via anyar::run_blocking(),
// so IPC stays responsive while decoding/analysing.

#include <anyar/frame_mailbox.h>
#include <anyar/plugin.h>
#include <anyar/task.h>

#include <boost/fiber/future.hpp>
#include <boost/fiber/mutex.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anyar { class EventBus; class Pinhole; class SharedBufferPool; }

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

/// Immutable description of the currently opened file.  Shared with the
/// decode session and analysis jobs so they never read mutable plugin state.
struct MediaInfo {
    std::string  path;
    ProbeResult  probe;
    bool has_video() const { return !probe.videoCodec.empty(); }
    bool has_audio() const { return !probe.audioCodec.empty(); }
};

/// Audio-only copy of the opened file for the frontend's <audio> element.
/// Prepared in the background on open; the temp file is deleted when the
/// last holder (plugin or an in-flight HTTP response) releases it.
struct AudioStream {
    boost::fibers::shared_future<std::string> ready;   // path to serve ("" = use original)
    std::string temp_path;                            // owned temp file, if any
    ~AudioStream();
};

/// Per-file bitrate analysis result
struct BitrateData {
    std::vector<double>  timestamps;   // bucket centres, seconds
    std::vector<double>  videoBps;     // bits per second per bucket
    std::vector<double>  audioBps;
};

/// Waveform peaks (normalised –1..1)
struct WaveformData {
    std::vector<float>   peaks;        // min/max interleaved: [min0, max0, min1, max1, …]
};

/// Renderer selected at startup.  WebGL routes raw frames to the JS canvas
/// via a SharedBufferPool + buffer:ready event; Pinhole routes them to the
/// native GtkGLArea overlay (no JS in the hot path).
enum class RenderMode { WebGL, Pinhole };

/// Control block for one playback session.  Written by command fibers, read
/// by the decode fiber — both on the service thread, so plain fields suffice.
struct PlaybackControl {
    uint64_t session       = 0;
    bool     playing       = false;
    double   pending_seek  = -1.0;   // next seek target (–1 = none)
    uint64_t seek_id       = 0;      // echoed back in video:seeked
    double   audio_time    = -1.0;   // audio clock from frontend (–1 = unknown)
    std::unique_ptr<anyar::SharedBufferPool> pool;   // WebGL mode only
    ~PlaybackControl();
};

class VideoPlugin : public anyar::IAnyarPlugin {
public:
    /// @param mode  Render mode reported via `video:get-mode`.  Pinhole mode
    ///              also requires set_pinhole() before the first frame.
    explicit VideoPlugin(RenderMode mode = RenderMode::Pinhole) : mode_(mode) {}

    std::string name() const override { return "video"; }
    void initialize(anyar::PluginContext& ctx) override;
    void shutdown() override;

    /// Bind a Pinhole to draw into (main thread, before app.run()).  Installs
    /// an on_render callback that draws the latest frame from the mailbox.
    void set_pinhole(std::shared_ptr<anyar::Pinhole> pin);

private:
    // Serialises open / close / shutdown (session start + stop).
    boost::fibers::mutex                    mtx_;

    std::shared_ptr<const MediaInfo>        media_;   // null = no file open
    std::shared_ptr<AudioStream>            audio_;   // what /video/stream serves

    // Analysis caches, valid for the MediaInfo they were computed from.
    std::shared_ptr<const MediaInfo>        bitrate_for_;
    BitrateData                             bitrate_;
    std::shared_ptr<const MediaInfo>        waveform_for_;
    WaveformData                            waveform_;

    asyik::service_ptr                      service_;
    anyar::EventBus*                        events_ = nullptr;

    RenderMode                              mode_ = RenderMode::Pinhole;
    std::shared_ptr<anyar::Pinhole>         pinhole_;
    // Decoded frames; the pinhole's on_render reads latest() from it.
    std::shared_ptr<anyar::FrameMailbox>    mailbox_ = std::make_shared<anyar::FrameMailbox>(12);

    anyar::BackgroundTask                   decode_task_;
    std::shared_ptr<PlaybackControl>        playback_;
    uint64_t                                session_counter_ = 0;

    void start_playback(std::shared_ptr<const MediaInfo> media);
    void stop_playback();
    std::shared_ptr<AudioStream> prepare_audio_stream(std::shared_ptr<const MediaInfo> media);
    void run_decode_loop(anyar::StopToken stop,
                         std::shared_ptr<const MediaInfo> media,
                         std::shared_ptr<PlaybackControl> pb);
    void present_frame(PlaybackControl& pb, const std::shared_ptr<anyar::Frame>& frame,
                       bool preview);
    void emit(const std::string& event, const nlohmann::json& payload);
};

} // namespace videoplayer
