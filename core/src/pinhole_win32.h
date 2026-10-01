#pragma once

// LibAnyar — private: per-window Pinhole composition host (Windows).
//
// The WebView2 controller keeps webview/webview's windowed hosting.  Pinholes
// are DirectComposition visuals on the webview's host ("widget") HWND,
// attached with a NON-topmost target so they render BELOW the WebView2 child
// window; the webview background is made transparent so HTML composites on
// top of them — the same layering as the Linux GtkOverlay (ADR-012).

#include <memory>

namespace anyar {

struct PinholeHost;

/// UI thread.  Create the composition host for one window: transparent
/// WebView2 background + D3D11 device + DComp target below the webview.
/// Returns nullptr if D3D11 / DirectComposition are unavailable (pinholes
/// then use the canvas fallback).
/// @param widget_hwnd  HWND that hosts the WebView2 (WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET)
/// @param controller   ICoreWebView2Controller*
std::shared_ptr<PinholeHost> create_pinhole_host(void* widget_hwnd, void* controller);

/// UI thread.  The window is being destroyed: drop the DComp target and tree.
void pinhole_host_window_destroyed(PinholeHost& host);

} // namespace anyar
