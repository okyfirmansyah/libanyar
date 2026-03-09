#pragma once

// WifiPlugin — nl80211-based WiFi spectrum analyzer
//
// Scans nearby access points via the Linux kernel nl80211 netlink API
// and renders a real-time spectrum bitmap directly into a SharedBuffer
// for zero-copy WebGL display.
//
// Commands:
//   wifi:start      {}               → { ok }       — begin periodic scanning
//   wifi:stop       {}               → { ok }       — stop scanning
//   wifi:scan-once  {}               → { networks } — trigger a single scan
//
// Events:
//   buffer:ready  { name, pool, url, size, metadata }  — per-frame bitmap
//   wifi:status   { scanning, interface, ... }
//
// Shared Memory:
//   Pool "wifi-spectrum" (2 buffers) served via anyar-shm:// URI scheme

#include <anyar/plugin.h>
#include <anyar/shared_buffer.h>

#include <boost/fiber/mutex.hpp>
#include <string>
#include <vector>
#include <memory>

namespace anyar { class EventBus; }

namespace wifianalyzer {

/// Represents a detected access point
struct AccessPoint {
    std::string ssid;
    std::string bssid;
    int         frequency  = 0;     // MHz
    int         channel    = 0;     // 1–14 (2.4 GHz) or 36–165 (5 GHz)
    int         signal_dbm = -100;  // dBm (e.g. -40 = strong, -90 = weak)
    int         bandwidth  = 20;    // channel width in MHz (20, 40, 80, 160)
    std::string security;           // "WPA2", "WPA3", "Open", etc.
    bool        associated = false; // currently connected to this AP
};

/// Information about the local WiFi interface
struct InterfaceInfo {
    std::string name;       // e.g. "wlp3s0"
    std::string mac;        // e.g. "04:68:74:5a:98:25"
    int         frequency;  // current freq in MHz
    int         channel;    // current channel
    int         bandwidth;  // current channel width in MHz
};

class WifiPlugin : public anyar::IAnyarPlugin {
public:
    std::string name() const override { return "wifi"; }
    void initialize(anyar::PluginContext& ctx) override;
    void shutdown() override;

private:
    // Service / event bus references
    asyik::service_ptr     service_;
    anyar::EventBus*       events_ = nullptr;

    // Scanning state
    bool scanning_   = false;
    bool stop_scan_  = false;

    // Detected networks (updated each scan cycle)
    boost::fibers::mutex          ap_mutex_;
    std::vector<AccessPoint>      access_points_;
    InterfaceInfo                 iface_info_;

    // SharedBufferPool for the rendered spectrum bitmap
    std::unique_ptr<anyar::SharedBufferPool> spectrum_pool_;
    std::string last_spectrum_buf_;  // track last written buffer for release

    // Bitmap dimensions
    static constexpr int BMP_WIDTH  = 800;
    static constexpr int BMP_HEIGHT = 400;

    // ── Internal helpers ────────────────────────────────────────────────
    bool detect_interface();
    std::vector<AccessPoint> nl80211_scan();
    bool nl80211_trigger_scan();
    void render_spectrum(const std::vector<AccessPoint>& aps,
                         uint8_t* rgba, int w, int h);
    void scan_loop();

    // Channel ↔ frequency helpers
public:
    static int freq_to_channel(int freq_mhz);
    static int channel_to_freq(int ch);
};

} // namespace wifianalyzer
