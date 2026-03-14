// WifiPlugin — nl80211 WiFi spectrum analyzer
//
// Uses Linux kernel nl80211 netlink API (via libnl-3) to scan nearby
// access points, then renders a spectrum occupation bitmap into a
// SharedBuffer for zero-copy WebGL display.

#include "wifi_plugin.h"

#include <anyar/types.h>
#include <anyar/event_bus.h>

#include <libasyik/service.hpp>
#include <nlohmann/json.hpp>

#include <boost/fiber/operations.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

// nl80211 / netlink headers
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <linux/nl80211.h>

// For interface index lookup
#include <net/if.h>

using json = nlohmann::json;

namespace wifianalyzer {

// ── Channel ↔ Frequency helpers ─────────────────────────────────────────────

int WifiPlugin::freq_to_channel(int freq) {
    // 2.4 GHz band
    if (freq >= 2412 && freq <= 2484) {
        if (freq == 2484) return 14;
        return (freq - 2412) / 5 + 1;
    }
    // 5 GHz band
    if (freq >= 5170 && freq <= 5825) {
        return (freq - 5000) / 5;
    }
    return 0;
}

int WifiPlugin::channel_to_freq(int ch) {
    if (ch >= 1 && ch <= 13)  return 2412 + (ch - 1) * 5;
    if (ch == 14)             return 2484;
    if (ch >= 36 && ch <= 165) return 5000 + ch * 5;
    return 0;
}

// ── Detect the WiFi interface ───────────────────────────────────────────────

bool WifiPlugin::detect_interface() {
    // Look for a wireless interface in /sys/class/net/*/wireless
    for (auto& entry : std::filesystem::directory_iterator("/sys/class/net")) {
        std::string ifname = entry.path().filename().string();
        if (std::filesystem::exists(entry.path() / "wireless")) {
            iface_info_.name = ifname;

            // Read MAC address
            std::ifstream mac_file(entry.path() / "address");
            if (mac_file) {
                std::getline(mac_file, iface_info_.mac);
            }

            std::cout << "[WifiPlugin] Found wireless interface: "
                      << ifname << " (" << iface_info_.mac << ")" << std::endl;
            return true;
        }
    }
    return false;
}

// ── nl80211 scan — parse cached scan results ────────────────────────────────
//
// We use the NL80211_CMD_GET_SCAN to fetch the kernel's cached BSS data.
// This avoids triggering an active scan (which may require root and causes
// a brief disconnection). The kernel periodically scans in the background
// and keeps the BSS list up to date.

// Callback data for netlink message parsing
struct ScanCallbackData {
    std::vector<AccessPoint> aps;
};

// Parse a single BSS (Basic Service Set) entry from nl80211
static int parse_bss_cb(struct nl_msg* msg, void* arg) {
    auto* data = static_cast<ScanCallbackData*>(arg);

    struct nlattr* tb[NL80211_ATTR_MAX + 1];
    struct genlmsghdr* gnlh = static_cast<genlmsghdr*>(
        nlmsg_data(nlmsg_hdr(msg)));

    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
              genlmsg_attrlen(gnlh, 0), nullptr);

    if (!tb[NL80211_ATTR_BSS]) return NL_SKIP;

    struct nlattr* bss[NL80211_BSS_MAX + 1];
    static struct nla_policy bss_policy[NL80211_BSS_MAX + 1] = {};
    bss_policy[NL80211_BSS_BSSID].type = NLA_UNSPEC;
    bss_policy[NL80211_BSS_FREQUENCY].type = NLA_U32;
    bss_policy[NL80211_BSS_SIGNAL_MBM].type = NLA_U32;
    bss_policy[NL80211_BSS_INFORMATION_ELEMENTS].type = NLA_UNSPEC;
    bss_policy[NL80211_BSS_STATUS].type = NLA_U32;

    if (nla_parse_nested(bss, NL80211_BSS_MAX,
                         tb[NL80211_ATTR_BSS], bss_policy) < 0) {
        return NL_SKIP;
    }

    AccessPoint ap;

    // BSSID (MAC address)
    if (bss[NL80211_BSS_BSSID]) {
        uint8_t* mac = static_cast<uint8_t*>(nla_data(bss[NL80211_BSS_BSSID]));
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (int i = 0; i < 6; ++i) {
            if (i > 0) oss << ':';
            oss << std::setw(2) << static_cast<int>(mac[i]);
        }
        ap.bssid = oss.str();
    }

    // Frequency
    if (bss[NL80211_BSS_FREQUENCY]) {
        ap.frequency = static_cast<int>(nla_get_u32(bss[NL80211_BSS_FREQUENCY]));
        ap.channel = WifiPlugin::freq_to_channel(ap.frequency);
    }

    // Signal strength (in mBm → dBm)
    if (bss[NL80211_BSS_SIGNAL_MBM]) {
        ap.signal_dbm = static_cast<int32_t>(nla_get_u32(bss[NL80211_BSS_SIGNAL_MBM])) / 100;
    }

    // Association status
    if (bss[NL80211_BSS_STATUS]) {
        uint32_t status = nla_get_u32(bss[NL80211_BSS_STATUS]);
        ap.associated = (status == NL80211_BSS_STATUS_ASSOCIATED ||
                         status == NL80211_BSS_STATUS_IBSS_JOINED);
    }

    // Parse Information Elements for SSID and RSN (security)
    if (bss[NL80211_BSS_INFORMATION_ELEMENTS]) {
        uint8_t* ie = static_cast<uint8_t*>(
            nla_data(bss[NL80211_BSS_INFORMATION_ELEMENTS]));
        int ie_len = nla_len(bss[NL80211_BSS_INFORMATION_ELEMENTS]);

        int pos = 0;
        bool has_rsn = false;
        bool has_wpa = false;

        while (pos + 2 <= ie_len) {
            uint8_t tag = ie[pos];
            uint8_t len = ie[pos + 1];
            if (pos + 2 + len > ie_len) break;

            if (tag == 0 && len > 0) {
                // SSID
                ap.ssid = std::string(reinterpret_cast<char*>(&ie[pos + 2]), len);
            } else if (tag == 48) {
                // RSN (WPA2/WPA3)
                has_rsn = true;
            } else if (tag == 221 && len >= 4) {
                // Vendor specific — check for WPA OUI
                if (ie[pos + 2] == 0x00 && ie[pos + 3] == 0x50 &&
                    ie[pos + 4] == 0xf2 && ie[pos + 5] == 0x01) {
                    has_wpa = true;
                }
            } else if (tag == 61 && len >= 1) {
                // HT Operation — extract secondary channel offset for bandwidth
                if (len >= 2) {
                    uint8_t sec_offset = ie[pos + 3] & 0x03;
                    if (sec_offset == 1 || sec_offset == 3) {
                        ap.bandwidth = 40;
                    }
                }
            } else if (tag == 192 && len >= 1) {
                // VHT Operation
                uint8_t ch_width = ie[pos + 2];
                if (ch_width == 1) ap.bandwidth = 80;
                else if (ch_width == 2) ap.bandwidth = 160;
            }

            pos += 2 + len;
        }

        if (has_rsn)      ap.security = "WPA2";
        else if (has_wpa) ap.security = "WPA";
        else              ap.security = "Open";
    }

    if (ap.ssid.empty()) ap.ssid = "(hidden)";
    if (ap.channel > 0) {
        data->aps.push_back(std::move(ap));
    }

    return NL_SKIP;
}

std::vector<AccessPoint> WifiPlugin::nl80211_scan() {
    ScanCallbackData cb_data;

    struct nl_sock* sk = nl_socket_alloc();
    if (!sk) return {};

    if (genl_connect(sk) < 0) {
        nl_socket_free(sk);
        return {};
    }

    int nl80211_id = genl_ctrl_resolve(sk, "nl80211");
    if (nl80211_id < 0) {
        nl_socket_free(sk);
        return {};
    }

    unsigned int ifindex = if_nametoindex(iface_info_.name.c_str());
    if (ifindex == 0) {
        nl_socket_free(sk);
        return {};
    }

    // Build GET_SCAN message — retrieves cached BSS list
    struct nl_msg* msg = nlmsg_alloc();
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, nl80211_id, 0,
                NLM_F_DUMP, NL80211_CMD_GET_SCAN, 0);
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifindex);

    // Install callback
    struct nl_cb* cb = nl_cb_alloc(NL_CB_DEFAULT);
    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, parse_bss_cb, &cb_data);

    nl_send_auto(sk, msg);
    nl_recvmsgs(sk, cb);

    nl_cb_put(cb);
    nlmsg_free(msg);
    nl_socket_free(sk);

    return cb_data.aps;
}

// ── nl80211 active scan ─ triggers a fresh scan on all channels ────────────
//
// Requires CAP_NET_ADMIN (typically root). Briefly disrupts the current
// connection while the radio cycles through channels.
// Returns true if the trigger succeeded (results arrive asynchronously
// in the kernel BSS cache, picked up by the next nl80211_scan() call).

// Callback: wait for scan completion notification
static int scan_done_cb(struct nl_msg* msg, void* arg) {
    auto* done = static_cast<bool*>(arg);
    struct genlmsghdr* gnlh = static_cast<genlmsghdr*>(
        nlmsg_data(nlmsg_hdr(msg)));
    if (gnlh->cmd == NL80211_CMD_NEW_SCAN_RESULTS ||
        gnlh->cmd == NL80211_CMD_SCAN_ABORTED) {
        *done = true;
    }
    return NL_SKIP;
}

static int no_seq_check_cb(struct nl_msg* /*msg*/, void* /*arg*/) {
    return NL_OK;
}

bool WifiPlugin::nl80211_trigger_scan() {
    struct nl_sock* sk = nl_socket_alloc();
    if (!sk) return false;

    if (genl_connect(sk) < 0) {
        nl_socket_free(sk);
        return false;
    }

    int nl80211_id = genl_ctrl_resolve(sk, "nl80211");
    if (nl80211_id < 0) {
        nl_socket_free(sk);
        return false;
    }

    unsigned int ifindex = if_nametoindex(iface_info_.name.c_str());
    if (ifindex == 0) {
        nl_socket_free(sk);
        return false;
    }

    // Subscribe to scan multicast group to get completion notification
    int mcid = genl_ctrl_resolve_grp(sk, "nl80211", "scan");
    if (mcid >= 0) {
        nl_socket_add_membership(sk, mcid);
    }

    // Build TRIGGER_SCAN message
    struct nl_msg* msg = nlmsg_alloc();
    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, nl80211_id, 0,
                0, NL80211_CMD_TRIGGER_SCAN, 0);
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifindex);

    int err = nl_send_auto(sk, msg);
    nlmsg_free(msg);

    if (err < 0) {
        std::cerr << "[WifiPlugin] TRIGGER_SCAN send failed: "
                  << nl_geterror(err) << std::endl;
        nl_socket_free(sk);
        return false;
    }

    // Wait for ACK
    nl_recvmsgs_default(sk);

    // Wait for scan-complete event (with timeout)
    bool scan_done = false;
    struct nl_cb* cb = nl_cb_alloc(NL_CB_DEFAULT);
    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, scan_done_cb, &scan_done);
    nl_cb_set(cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, no_seq_check_cb, nullptr);

    // Poll for up to 10 seconds
    nl_socket_set_nonblocking(sk);
    for (int i = 0; i < 100 && !scan_done; ++i) {
        nl_recvmsgs(sk, cb);
        if (!scan_done) {
            boost::this_fiber::sleep_for(std::chrono::milliseconds(100));
        }
    }

    nl_cb_put(cb);
    if (mcid >= 0) {
        nl_socket_drop_membership(sk, mcid);
    }
    nl_socket_free(sk);

    if (scan_done) {
        std::cout << "[WifiPlugin] Active scan completed" << std::endl;
    } else {
        std::cerr << "[WifiPlugin] Active scan timed out" << std::endl;
    }

    return scan_done;
}

// ── Spectrum bitmap rendering ───────────────────────────────────────────────
//
// Draws a 2D spectrum plot:
//   X-axis: WiFi channels (2.4 GHz: 1–14 on left, 5 GHz: 36–165 on right)
//   Y-axis: Signal strength (–100 dBm at bottom, –20 dBm at top)
//   Each AP → a bell curve centered on its channel, width = bandwidth

void WifiPlugin::render_spectrum(const std::vector<AccessPoint>& aps,
                                  uint8_t* rgba, int w, int h) {
    // Clear to dark background
    for (int i = 0; i < w * h; ++i) {
        rgba[i * 4 + 0] = 18;   // R
        rgba[i * 4 + 1] = 18;   // G
        rgba[i * 4 + 2] = 30;   // B
        rgba[i * 4 + 3] = 255;  // A
    }

    // Layout: left half = 2.4 GHz (channels 1–14), right half = 5 GHz
    const int margin_left   = 50;
    const int margin_right  = 20;
    const int margin_top    = 30;
    const int margin_bottom = 40;
    const int plot_w = w - margin_left - margin_right;
    const int plot_h = h - margin_top - margin_bottom;

    // Split: 40% for 2.4 GHz, 60% for 5 GHz
    const int band_gap = 20;
    const int w_24 = static_cast<int>(plot_w * 0.38);
    const int w_5  = plot_w - w_24 - band_gap;

    // dBm range
    const double dbm_min = -100.0;
    const double dbm_max = -20.0;

    // Helper: draw a horizontal line
    auto hline = [&](int y, int x0, int x1, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        if (y < 0 || y >= h) return;
        for (int x = std::max(0, x0); x <= std::min(w - 1, x1); ++x) {
            int idx = (y * w + x) * 4;
            rgba[idx + 0] = r; rgba[idx + 1] = g;
            rgba[idx + 2] = b; rgba[idx + 3] = a;
        }
    };

    // Helper: draw a vertical line
    auto vline = [&](int x, int y0, int y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        if (x < 0 || x >= w) return;
        for (int y = std::max(0, y0); y <= std::min(h - 1, y1); ++y) {
            int idx = (y * w + x) * 4;
            rgba[idx + 0] = r; rgba[idx + 1] = g;
            rgba[idx + 2] = b; rgba[idx + 3] = a;
        }
    };

    // Helper: set pixel (with alpha blending)
    auto setpx = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (x < 0 || x >= w || y < 0 || y >= h) return;
        int idx = (y * w + x) * 4;
        if (a == 255) {
            rgba[idx + 0] = r; rgba[idx + 1] = g;
            rgba[idx + 2] = b; rgba[idx + 3] = 255;
        } else {
            float fa = a / 255.0f;
            rgba[idx + 0] = static_cast<uint8_t>(r * fa + rgba[idx + 0] * (1 - fa));
            rgba[idx + 1] = static_cast<uint8_t>(g * fa + rgba[idx + 1] * (1 - fa));
            rgba[idx + 2] = static_cast<uint8_t>(b * fa + rgba[idx + 2] * (1 - fa));
            rgba[idx + 3] = 255;
        }
    };

    // ── Draw grid ───────────────────────────────────────────────────────

    // Axes
    hline(margin_top + plot_h, margin_left, margin_left + plot_w, 60, 60, 80);
    vline(margin_left, margin_top, margin_top + plot_h, 60, 60, 80);

    // Y-axis: dBm grid lines (every 20 dBm)
    const int dbm_values[] = {-100, -80, -60, -40, -20};
    for (int dbm : dbm_values) {
        double norm = (dbm - dbm_min) / (dbm_max - dbm_min);
        int y = margin_top + plot_h - static_cast<int>(norm * plot_h);

        // Grid line (skip for -100, that's the baseline)
        if (dbm > static_cast<int>(dbm_min))
            hline(y, margin_left, margin_left + plot_w, 40, 40, 55);
    }

    // 2.4 GHz channel markers
    for (int ch = 1; ch <= 14; ++ch) {
        double norm_x = (ch - 1.0) / 13.0;
        int x = margin_left + static_cast<int>(norm_x * w_24);
        vline(x, margin_top + plot_h, margin_top + plot_h + 5, 80, 80, 100);
    }

    // 5 GHz channel markers
    const int ch5[] = {36, 40, 44, 48, 52, 56, 60, 64,
                       100, 104, 108, 112, 116, 120, 124, 128,
                       132, 136, 140, 144, 149, 153, 157, 161, 165};
    int n_ch5 = sizeof(ch5) / sizeof(ch5[0]);
    for (int i = 0; i < n_ch5; ++i) {
        double norm_x = static_cast<double>(i) / (n_ch5 - 1);
        int x = margin_left + w_24 + band_gap + static_cast<int>(norm_x * w_5);
        vline(x, margin_top + plot_h, margin_top + plot_h + 5, 80, 80, 100);
    }

    // ── Channel → pixel-X mapping ──────────────────────────────────────

    auto ch_to_px = [&](int ch, int bw_mhz) -> std::pair<double, double> {
        // Returns (center_x, half_width_px)
        if (ch >= 1 && ch <= 14) {
            double norm = (ch - 1.0) / 13.0;
            double cx = margin_left + norm * w_24;
            // Each 2.4 GHz channel is 5 MHz apart; full width
            double ch_px = w_24 / 13.0;
            double half = (bw_mhz / 5.0) * ch_px * 0.5;
            return {cx, half};
        }
        // 5 GHz
        for (int i = 0; i < n_ch5; ++i) {
            if (ch5[i] == ch) {
                double norm = static_cast<double>(i) / (n_ch5 - 1);
                double cx = margin_left + w_24 + band_gap + norm * w_5;
                double ch_px = w_5 / static_cast<double>(n_ch5 - 1);
                double half = (bw_mhz / 20.0) * ch_px * 0.5;
                return {cx, std::max(half, ch_px * 0.5)};
            }
        }
        return {0, 0};
    };

    // ── Color palette for APs ───────────────────────────────────────────
    struct Color { uint8_t r, g, b; };
    const Color palette[] = {
        {66, 165, 245},   // blue
        {239, 83, 80},    // red
        {102, 187, 106},  // green
        {255, 167, 38},   // orange
        {171, 71, 188},   // purple
        {38, 198, 218},   // cyan
        {255, 241, 118},  // yellow
        {141, 110, 99},   // brown
        {236, 64, 122},   // pink
        {0, 230, 118},    // teal
    };
    const int n_colors = sizeof(palette) / sizeof(palette[0]);

    // ── Draw each AP as a bell curve ────────────────────────────────────

    for (size_t ai = 0; ai < aps.size(); ++ai) {
        const auto& ap = aps[ai];
        auto [center_x, half_w] = ch_to_px(ap.channel, ap.bandwidth);
        if (center_x == 0) continue;

        double dbm_norm = std::clamp((ap.signal_dbm - dbm_min) / (dbm_max - dbm_min), 0.0, 1.0);
        int peak_y = margin_top + plot_h - static_cast<int>(dbm_norm * plot_h);

        const Color& col = palette[ai % n_colors];
        uint8_t fill_alpha = ap.associated ? 100 : 60;

        // Draw bell curve (Gaussian-like) and fill under it
        double sigma = half_w / 2.0;
        if (sigma < 3) sigma = 3;

        int x_start = static_cast<int>(center_x - half_w * 1.5);
        int x_end   = static_cast<int>(center_x + half_w * 1.5);
        x_start = std::max(margin_left, x_start);
        x_end   = std::min(margin_left + plot_w, x_end);

        for (int x = x_start; x <= x_end; ++x) {
            double dx = x - center_x;
            double gauss = std::exp(-(dx * dx) / (2.0 * sigma * sigma));
            int curve_y = margin_top + plot_h - static_cast<int>(dbm_norm * gauss * plot_h);

            // Fill from curve to baseline
            for (int y = curve_y; y <= margin_top + plot_h; ++y) {
                setpx(x, y, col.r, col.g, col.b, fill_alpha);
            }

            // Draw curve line
            setpx(x, curve_y, col.r, col.g, col.b, 220);
            if (curve_y + 1 <= margin_top + plot_h)
                setpx(x, curve_y + 1, col.r, col.g, col.b, 140);
        }

        // Draw a brighter dot at the peak
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                setpx(static_cast<int>(center_x) + dx, peak_y + dy,
                      col.r, col.g, col.b, 255);
    }
}

// ── Continuous scan loop (runs as a fibre) ──────────────────────────────────

void WifiPlugin::scan_loop() {
    while (scanning_ && !stop_scan_) {
        // Fetch latest scan data from nl80211
        auto aps = nl80211_scan();

        {
            std::lock_guard<boost::fibers::mutex> lock(ap_mutex_);
            access_points_ = aps;
        }

        // Release previous buffer so the pool has a FREE slot
        if (!last_spectrum_buf_.empty()) {
            spectrum_pool_->release_read(last_spectrum_buf_);
        }

        // Render spectrum bitmap
        auto& buf = spectrum_pool_->acquire_write();
        render_spectrum(aps, reinterpret_cast<uint8_t*>(buf.data()),
                        BMP_WIDTH, BMP_HEIGHT);

        // Emit buffer:ready
        if (events_) {
            // Build AP list for metadata
            json ap_list = json::array();
            for (auto& ap : aps) {
                ap_list.push_back({
                    {"ssid", ap.ssid},
                    {"bssid", ap.bssid},
                    {"channel", ap.channel},
                    {"frequency", ap.frequency},
                    {"signal", ap.signal_dbm},
                    {"bandwidth", ap.bandwidth},
                    {"security", ap.security},
                    {"associated", ap.associated}
                });
            }

            spectrum_pool_->release_write(buf, "{}");
            last_spectrum_buf_ = buf.name();
            events_->emit("buffer:ready", {
                {"name", buf.name()},
                {"pool", "wifi-spectrum"},
                {"url", "anyar-shm://" + buf.name()},
                {"size", buf.size()},
                {"metadata", {
                    {"width", BMP_WIDTH},
                    {"height", BMP_HEIGHT},
                    {"format", "rgba"},
                    {"networks", ap_list}
                }}
            });
        }

        // Scan interval: ~3 seconds (nl80211 cached scan updates periodically)
        for (int i = 0; i < 30 && scanning_ && !stop_scan_; ++i) {
            boost::this_fiber::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

// ── Plugin initialization ───────────────────────────────────────────────────

void WifiPlugin::initialize(anyar::PluginContext& ctx) {
    service_ = ctx.service;
    events_ = &ctx.events;
    auto& cmds = ctx.commands;

    if (!detect_interface()) {
        std::cerr << "[WifiPlugin] No wireless interface found!" << std::endl;
    }

    // Create the SharedBufferPool once (lives for the plugin's lifetime)
    size_t bmp_bytes = static_cast<size_t>(BMP_WIDTH) * BMP_HEIGHT * 4;
    spectrum_pool_ = std::make_unique<anyar::SharedBufferPool>(
        "wifi-spectrum", bmp_bytes, 2);

    // ── wifi:start — Begin periodic scanning ────────────────────────────
    cmds.add("wifi:start", [this](const json& /*args*/) -> json {
        if (scanning_) return {{"ok", true}, {"message", "Already scanning"}};

        scanning_   = true;
        stop_scan_  = false;

        service_->execute([this]() {
            try { scan_loop(); } catch (const std::exception& e) {
                std::cerr << "[WifiPlugin] Scan loop error: " << e.what() << std::endl;
            }
            scanning_ = false;
        });

        return {{"ok", true}, {"interface", iface_info_.name}};
    });

    // ── wifi:stop — Stop scanning ───────────────────────────────────────
    cmds.add("wifi:stop", [this](const json& /*args*/) -> json {
        stop_scan_ = true;
        scanning_  = false;
        return {{"ok", true}};
    });

    // ── wifi:scan-once — Trigger a single scan and return results ────────
    cmds.add("wifi:scan-once", [this](const json& /*args*/) -> json {
        auto aps = nl80211_scan();

        json networks = json::array();
        for (auto& ap : aps) {
            networks.push_back({
                {"ssid", ap.ssid},
                {"bssid", ap.bssid},
                {"channel", ap.channel},
                {"frequency", ap.frequency},
                {"signal", ap.signal_dbm},
                {"bandwidth", ap.bandwidth},
                {"security", ap.security},
                {"associated", ap.associated}
            });
        }

        return {
            {"networks", networks},
            {"interface", iface_info_.name},
            {"count", aps.size()}
        };
    });

    // ── wifi:info — Return interface information ────────────────────────
    cmds.add("wifi:info", [this](const json& /*args*/) -> json {
        return {
            {"interface", iface_info_.name},
            {"mac", iface_info_.mac},
            {"scanning", scanning_}
        };
    });

    // ── wifi:active-scan — Trigger a kernel active scan (needs root) ──
    //     Dispatches to a background fibre and returns immediately.
    //     Emits wifi:active-scan-done when finished.
    cmds.add("wifi:active-scan", [this](const json& /*args*/) -> json {
        service_->execute([this]() {
            std::cout << "[WifiPlugin] Active scan fiber started" << std::endl;
            bool ok = nl80211_trigger_scan();
            if (!ok) {
                std::cerr << "[WifiPlugin] Active scan trigger failed" << std::endl;
                if (events_) {
                    events_->emit("wifi:active-scan-done", {
                        {"ok", false},
                        {"error", "Active scan failed (requires root/CAP_NET_ADMIN)"}
                    });
                }
                return;
            }

            // Read fresh results and update shared AP list.
            // The scan_loop will pick these up and render the next frame.
            auto aps = nl80211_scan();
            {
                std::lock_guard<boost::fibers::mutex> lock(ap_mutex_);
                access_points_ = aps;
            }
            std::cout << "[WifiPlugin] Active scan done, got " << aps.size()
                      << " APs, emitting wifi:active-scan-done" << std::endl;

            if (events_) {
                events_->emit("wifi:active-scan-done", {
                    {"ok", true},
                    {"count", aps.size()}
                });
            }
        });

        return {{"ok", true}, {"message", "Active scan started"}};
    });

    std::cout << "[WifiPlugin] Initialized — wifi:start/stop/scan-once/active-scan/info"
              << " + SharedBuffer spectrum bitmap" << std::endl;
}

void WifiPlugin::shutdown() {
    stop_scan_ = true;
    scanning_  = false;
    // Let the scan loop fibre exit naturally
    std::cout << "[WifiPlugin] Shutdown" << std::endl;
}

} // namespace wifianalyzer
