// ---------------------------------------------------------------------------
// @libanyar/api — Main entry point
// ---------------------------------------------------------------------------

// ── Core API ───────────────────────────────────────────────────────────────
export { invoke } from './invoke';
export { listen, emit, once, onReady } from './events';
export { getPort, setPort, getBaseUrl, getWsUrl, isNativeIpc } from './config';

// ── Types ──────────────────────────────────────────────────────────────────
export type {
  IpcRequest,
  IpcResponse,
  IpcError,
  EventMessage,
  EventHandler,
  UnlistenFn,
} from './types';

// ── Expose on window for non-module usage ──────────────────────────────────
import { invoke } from './invoke';
import { listen, emit, once, onReady } from './events';
import { getPort, setPort, isNativeIpc } from './config';

if (typeof window !== 'undefined') {
  (window as any).__anyar__ = {
    invoke,
    listen,
    emit,
    once,
    onReady,
    getPort,
    setPort,
    isNativeIpc,
  };
}
