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
  <div class="flex-1 flex items-center justify-center p-8" style="color: var(--text-muted);">
    <div class="text-center">
      <svg class="w-12 h-12 mx-auto mb-3 opacity-30" fill="none" stroke="currentColor" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5"
              d="M15 7a2 2 0 012 2m4 0a6 6 0 01-7.743 5.743L11 17H9v2H7v2H4a1 1 0 01-1-1v-2.586a1 1 0 01.293-.707l5.964-5.964A6 6 0 1121 9z" />
      </svg>
      <p class="text-sm">No entries in this group</p>
      <p class="text-xs mt-1 opacity-50">Press Ctrl+N to create one</p>
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
                <span class="badge-expired">EXP</span>
              {:else if expiring}
                <span class="badge-expiring">EXP</span>
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
            <span class="truncate text-xs" style="color: var(--text-muted);">
              {domainFromUrl(entry.url)}
            </span>
          </td>
          <td>
            <span class="truncate text-xs" style="color: var(--text-muted);">
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
    padding: 8px 12px;
    font-size: 11px;
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
    background: rgba(255, 255, 255, 0.03);
  }

  .entry-row.selected {
    background: var(--accent-dim);
  }

  .entry-row td {
    padding: 8px 12px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    max-width: 0;
  }

  .title-cell {
    overflow: visible !important;
  }

  .entry-icon {
    width: 26px;
    height: 26px;
    border-radius: 6px;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
    font-size: 12px;
    font-weight: 600;
    background: var(--surface-3);
    color: var(--text-dim);
  }

  .entry-icon.selected {
    background: var(--accent);
    color: white;
  }

  .badge-expired {
    font-size: 9px;
    padding: 1px 5px;
    border-radius: 3px;
    font-weight: 600;
    background: var(--danger-dim);
    color: var(--danger);
    flex-shrink: 0;
  }

  .badge-expiring {
    font-size: 9px;
    padding: 1px 5px;
    border-radius: 3px;
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
    padding: 4px 0;
    border-radius: 8px;
    background: var(--surface-3);
    border: 1px solid var(--border);
    min-width: 120px;
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
  }

  .context-item {
    display: block;
    width: 100%;
    text-align: left;
    padding: 6px 12px;
    font-size: 12px;
    color: var(--text-dim);
    transition: background 0.1s;
    background: transparent;
  }

  .context-item:hover {
    background: rgba(255, 255, 255, 0.05);
  }

  .context-item.danger {
    color: var(--danger);
  }
</style>
