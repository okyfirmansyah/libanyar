// ---------------------------------------------------------------------------
// @libanyar/api — Global type augmentation
// ---------------------------------------------------------------------------

export {};

declare global {
  interface Window {
    /** Port injected by LibAnyar C++ runtime via webview_init(). */
    __LIBANYAR_PORT__?: number;
    /** Global handle exposed for non-module / UMD usage. */
    __anyar__?: typeof import('./index');
  }
}
