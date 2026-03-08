// ---------------------------------------------------------------------------
// @libanyar/api — Shared types
// ---------------------------------------------------------------------------

/** IPC request sent to C++ backend via HTTP POST. */
export interface IpcRequest {
  id: string;
  cmd: string;
  args: Record<string, unknown>;
}

/** IPC response returned by C++ backend. */
export interface IpcResponse<T = unknown> {
  id: string;
  data: T | null;
  error: IpcError | null;
}

/** Error payload inside an IPC response. */
export interface IpcError {
  code: number;
  message: string;
}

/** WebSocket event message (bidirectional). */
export interface EventMessage {
  type: 'event';
  event: string;
  payload: unknown;
}

/** A function that removes a subscription when called. */
export type UnlistenFn = () => void;

/** Handler called when an event is received. */
export type EventHandler<T = unknown> = (payload: T) => void;
