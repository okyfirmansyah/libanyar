<script>
  /**
   * EntryList — Table view of key entries (KeePass-style).
   * Columns: Title, Username, Password (masked), URL, Notes.
   * Single-click selects a row, double-click opens the detail modal.
   */

  let {
    entries = [],
    selectedEntryId = null,
    onselect = null,
    onopen = null,
    ondelete = null,
  } = $props();

  let contextId = $state(null);

  function handleContextMenu(e, id) {
    e.preventDefault();
    contextId = contextId === id ? null : id;
  }

  function isExpired(expiresAt) {
    if (!expiresAt) return false;
    return new Date(expiresAt) < new Date();
  }

  function isExpiringSoon(expiresAt) {
    if (!expiresAt) return false;
    const d = new Date(expiresAt);
    const now = new Date();
    const diff = d - now;
    return diff > 0 && diff < 30 * 24 * 60 * 60 * 1000; // 30 days
  }

  function domainFromUrl(url) {
    if (!url) return '';
    try {
      return new URL(url.startsWith('http') ? url : 'https://' + url).hostname;
    } catch {
      return url;
    }
  }

  function truncate(str, len = 40) {
    if (!str) return '';
    return str.length > len ? str.slice(0, len) + '…' : str;
  }

  // Close any context menu when clicking elsewhere
  function handleWindowClick() {
    contextId = null;
  }
</script>

<svelte:window onclick={handleWindowClick} />

{#if entries.length === 0}
  <div class="h-full flex items-center justify-center p-8" style="color: var(--text-muted);">
    <div class="flex flex-col items-center text-center">
      <svg class="w-10 h-10 mb-3 opacity-20" fill="none" stroke="currentColor" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5"
              d="M15 7a2 2 0 012 2m4 0a6 6 0 01-7.743 5.743L11 17H9v2H7v2H4a1 1 0 01-1-1v-2.586a1 1 0 01.293-.707l5.964-5.964A6 6 0 1121 9z" />
      </svg>
      <p class="text-[13px] mb-1" style="color: var(--text-muted);">No entries</p>
      <p class="text-xs opacity-40 flex items-center gap-1"><span class="kbd">Ctrl+N</span> to create one</p>
    </div>
  </div>
{:else}
  <table class="entry-table w-full">
    <thead>
      <tr>
        <th class="text-left">Title</th>
        <th class="text-left">Username</th>
        <th class="text-left">Password</th>
        <th class="text-left">URL</th>
        <th class="text-left">Notes</th>
      </tr>
    </thead>
    <tbody>
      {#each entries as entry (entry.id)}
        {@const selected = selectedEntryId === entry.id}
        {@const expired = isExpired(entry.expiresAt)}
        {@const expiring = isExpiringSoon(entry.expiresAt)}

        <tr
          class="entry-row"
          class:selected
          onclick={() => onselect && onselect(entry.id)}
          ondblclick={() => onopen && onopen(entry.id)}
          oncontextmenu={(e) => handleContextMenu(e, entry.id)}
          role="row"
          tabindex="0"
        >
          <td class="title-cell">
            <div class="flex items-center gap-2">
              <div class="entry-icon" class:selected>
                {entry.title ? entry.title[0].toUpperCase() : '?'}
              </div>
              <span class="truncate font-medium" style="color: {selected ? 'var(--accent)' : 'var(--text)'};">
                {entry.title || 'Untitled'}
              </span>
              {#if expired}
                <span class="badge badge-danger">EXP</span>
              {:else if expiring}
                <span class="badge badge-warning">EXP</span>
              {/if}
            </div>
          </td>
          <td>
            <span class="truncate" style="color: var(--text-dim);">{entry.username || ''}</span>
          </td>
          <td>
            <span class="font-mono text-xs" style="color: var(--text-muted); letter-spacing: 2px;">
              {entry.password ? '••••••••' : ''}
            </span>
          </td>
          <td>
            <span class="truncate" style="color: var(--text-dim);">
              {domainFromUrl(entry.url)}
            </span>
          </td>
          <td>
            <span class="truncate" style="color: var(--text-dim);">
              {truncate(entry.notes, 30)}
            </span>
          </td>
        </tr>

        <!-- Context menu -->
        {#if contextId === entry.id}
          <tr class="context-row">
            <td colspan="5">
              <div class="context-menu">
                <button class="context-item"
                        onclick={(e) => { e.stopPropagation(); contextId = null; onopen && onopen(entry.id); }}>
                  Open
                </button>
                <button class="context-item danger"
                        onclick={(e) => { e.stopPropagation(); contextId = null; ondelete && ondelete(entry.id); }}>
                  Delete
                </button>
              </div>
            </td>
          </tr>
        {/if}
      {/each}
    </tbody>
  </table>
{/if}

<style>
  .entry-table {
    border-collapse: collapse;
    table-layout: fixed;
    font-size: 13px;
  }

  .entry-table thead th {
    position: sticky;
    top: 0;
    z-index: 2;
    padding: 10px 14px;
    font-size: 11.5px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-muted);
    background: var(--surface);
    border-bottom: 1px solid var(--border);
    white-space: nowrap;
  }

  /* Column widths */
  .entry-table th:nth-child(1) { width: 25%; }
  .entry-table th:nth-child(2) { width: 18%; }
  .entry-table th:nth-child(3) { width: 14%; }
  .entry-table th:nth-child(4) { width: 20%; }
  .entry-table th:nth-child(5) { width: 23%; }

  .entry-row {
    cursor: pointer;
    transition: background 0.1s;
    border-bottom: 1px solid var(--border);
  }

  .entry-row:hover {
    background: rgba(255, 255, 255, 0.02);
  }

  .entry-row.selected {
    background: rgba(182, 171, 124, 0.04);
    box-shadow: inset 2px 0 0 var(--accent);
  }

  .entry-row td {
    padding: 10px 14px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    max-width: 0;
  }

  .title-cell {
    overflow: visible !important;
  }

  .entry-icon {
    width: 28px;
    height: 28px;
    border-radius: 7px;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
    font-size: 12px;
    font-weight: 600;
    background: var(--surface-2);
    color: var(--text-muted);
    border: 1px solid var(--border);
  }

  .entry-icon.selected {
    background: var(--accent);
    color: var(--bg);
    border-color: var(--accent);
  }

  .badge-expired {
    font-size: 10px;
    padding: 2px 7px;
    border-radius: 9999px;
    font-weight: 600;
    background: var(--danger-dim);
    color: var(--danger);
    flex-shrink: 0;
  }

  .badge-expiring {
    font-size: 10px;
    padding: 2px 7px;
    border-radius: 9999px;
    font-weight: 600;
    background: var(--warning-dim);
    color: var(--warning);
    flex-shrink: 0;
  }

  .context-row {
    background: transparent;
  }

  .context-row td {
    padding: 0;
    position: relative;
  }

  .context-menu {
    position: absolute;
    right: 12px;
    top: -4px;
    z-index: 50;
    padding: 4px;
    border-radius: 8px;
    background: var(--surface-2);
    border: 1px solid var(--border);
    min-width: 130px;
    box-shadow: 0 8px 30px rgba(0, 0, 0, 0.4);
  }

  .context-item {
    display: block;
    width: 100%;
    text-align: left;
    padding: 7px 10px;
    font-size: 12.5px;
    color: var(--text-dim);
    transition: background 0.1s;
    background: transparent;
    border-radius: 5px;
  }

  .context-item:hover {
    background: rgba(255, 255, 255, 0.06);
    color: var(--text);
  }

  .context-item.danger:hover {
    background: var(--danger-dim);
    color: var(--danger);
  }
</style>
