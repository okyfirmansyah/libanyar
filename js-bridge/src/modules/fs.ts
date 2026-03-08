// ---------------------------------------------------------------------------
// @libanyar/api/fs — File-system commands
// ---------------------------------------------------------------------------

import { invoke } from '../invoke';

/** Metadata returned by `stat()`. */
export interface FileMetadata {
  size: number;
  isDirectory: boolean;
  isFile: boolean;
  modifiedTime: string;
}

/** Entry returned by `readDir()`. */
export interface DirEntry {
  name: string;
  isDirectory: boolean;
  isFile: boolean;
}

/** Options for `readFile`. */
export interface ReadFileOptions {
  /** Encoding. Omit for raw base64 binary. */
  encoding?: 'utf-8' | 'base64';
}

/**
 * Read a file's contents.
 *
 * @param path  Absolute or app-relative path.
 * @param opts  Encoding options (defaults to UTF-8).
 * @returns     File content as a string.
 */
export async function readFile(
  path: string,
  opts: ReadFileOptions = { encoding: 'utf-8' },
): Promise<string> {
  return invoke<string>('fs:readFile', { path, ...opts });
}

/**
 * Write content to a file (creates or overwrites).
 *
 * @param path     Absolute or app-relative path.
 * @param content  String content to write.
 */
export async function writeFile(path: string, content: string): Promise<void> {
  await invoke<void>('fs:writeFile', { path, content });
}

/**
 * List the contents of a directory.
 */
export async function readDir(path: string): Promise<DirEntry[]> {
  return invoke<DirEntry[]>('fs:readDir', { path });
}

/**
 * Check whether a path exists.
 */
export async function exists(path: string): Promise<boolean> {
  return invoke<boolean>('fs:exists', { path });
}

/**
 * Create a directory (recursively).
 */
export async function mkdir(path: string): Promise<void> {
  await invoke<void>('fs:mkdir', { path });
}

/**
 * Remove a file or directory.
 *
 * @param path      Path to remove.
 * @param recursive Remove non-empty directories if true.
 */
export async function remove(
  path: string,
  recursive = false,
): Promise<void> {
  await invoke<void>('fs:remove', { path, recursive });
}

/**
 * Get file/directory metadata.
 */
export async function metadata(path: string): Promise<FileMetadata> {
  return invoke<FileMetadata>('fs:metadata', { path });
}
