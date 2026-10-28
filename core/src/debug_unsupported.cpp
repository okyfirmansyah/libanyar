// LibAnyar — debug tooling for platforms without a native implementation yet.
//
// Windows (WebView2) and macOS (WKWebView) compile this file until their
// debug_<os> hooks land.  Planned backends:
//   - Windows: ICoreWebView2::CapturePreview (PNG) for capture;
//     SendInput into the WebView2 child HWND (or
//     ICoreWebView2CompositionController::SendMouseInput) for input.
//   - macOS:   -[WKWebView takeSnapshotWithConfiguration:completionHandler:]
//     for capture; synthesized NSEvents sent to the WKWebView for input.
// Console forwarding and test scripts work everywhere: they only use the JS
// bridge.

#include "platform.h"

namespace anyar::platform {

bool can_capture_webview() { return false; }
bool can_inject_webview_input() { return false; }

void capture_webview(void*, SnapshotCallback done) {
    done({}, "webview capture is not implemented on this platform yet");
}

std::string inject_webview_input(void*, const InputEvent&) {
    return "synthetic input is not implemented on this platform yet";
}

} // namespace anyar::platform
