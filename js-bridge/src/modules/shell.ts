// ---------------------------------------------------------------------------
// @libanyar/api/shell — Shell / OS integration commands
// ---------------------------------------------------------------------------

import { invoke } from '../invoke';

/** Result of a shell command execution. */
export interface CommandOutput {
  code: number;
  stdout: string;
  stderr: string;
}

/**
 * Open a URL in the user's default web browser.
 */
export async function openUrl(url: string): Promise<void> {
  await invoke<void>('shell:openUrl', { url });
}

/**
 * Open a file or folder with the OS default application.
 */
export async function openPath(path: string): Promise<void> {
  await invoke<void>('shell:openPath', { path });
}

/**
 * Execute a shell command and return its output.
 *
 * @param program  Program to execute.
 * @param args     Command-line arguments.
 * @param options  Optional settings (cwd, env).
 * @returns        Exit code, stdout, and stderr.
 */
export async function execute(
  program: string,
  args: string[] = [],
  options: { cwd?: string; env?: Record<string, string> } = {},
): Promise<CommandOutput> {
  return invoke<CommandOutput>('shell:execute', { program, args, ...options });
}
