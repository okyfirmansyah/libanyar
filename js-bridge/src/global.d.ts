// ---------------------------------------------------------------------------
// @libanyar/api — Global type augmentation
// ---------------------------------------------------------------------------

export {};

declare global {
  interface Window {
    /** Port injected by LibAnyar C++ runtime via webview_init(). */
    __LIBANYAR_PORT__?: number;
    /**
     * Whether the native webview serves `anyar-shm://` (injected by the C++
     * runtime; `false` on Windows).  Absent → assumed true.
     */
    __LIBANYAR_SHM_SCHEME__?: boolean;
    /**
     * Whether SharedBuffers can be mapped into the page directly
     * (`buffer:attach`; WebView2 on Windows).  Injected by the C++ runtime.
     */
    __LIBANYAR_SHARED_BUFFERS__?: boolean;
    /** Global handle exposed for non-module / UMD usage. */
    __anyar__?: typeof import('./index');
  }
}
