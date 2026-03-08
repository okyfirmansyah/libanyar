<script>
  /**
   * EntryList — Displays a list of key entries in the center panel.
   * Each row shows title, username, URL, and expiry status.
   */

  let {
    entries = [],
    selectedEntryId = null,
    onselect = null,
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

  // Close any context menu when clicking elsewhere
  function handleWindowClick() {
    contextId = null;
  }
</script>

<svelte:window onclick={handleWindowClick} />

{#if entries.length === 0}
  <div class="flex-1 flex items-center justify-center p-4" style="color: var(--text-muted);">
    <p class="text-sm text-center opacity-60">No entries in this group</p>
  </div>
{:else}
  {#each entries as entry (entry.id)}
    {@const selected = selectedEntryId === entry.id}
    {@const expired = isExpired(entry.expiresAt)}
    {@const expiring = isExpiringSoon(entry.expiresAt)}
  
    <div class="relative">
      <button
        class="w-full text-left px-3 py-3 flex items-center gap-3 transition-colors"
        style="background: {selected ? 'var(--accent-dim)' : 'transparent'};
               border-bottom: 1px solid var(--border);"
        onclick={() => onselect && onselect(entry.id)}
        oncontextmenu={(e) => handleContextMenu(e, entry.id)}
      >
        <!-- Icon placeholder -->
        <div class="w-9 h-9 rounded-lg flex items-center justify-center shrink-0 text-sm font-semibold"
             style="background: {selected ? 'var(--accent)' : 'var(--surface-3)'}; color: {selected ? 'white' : 'var(--text-dim)'};">
          {entry.title ? entry.title[0].toUpperCase() : '?'}
        </div>

        <!-- Entry info -->
        <div class="flex-1 min-w-0">
          <div class="flex items-center gap-2">
            <span class="text-[13px] font-medium truncate" style="color: {selected ? 'var(--accent)' : 'var(--text)'};">
              {entry.title || 'Untitled'}
            </span>
            {#if expired}
              <span class="text-[10px] px-1.5 py-0.5 rounded font-medium" style="background: var(--danger-dim); color: var(--danger);">EXPIRED</span>
            {:else if expiring}
              <span class="text-[10px] px-1.5 py-0.5 rounded font-medium" style="background: var(--warning-dim); color: var(--warning);">EXPIRING</span>
            {/if}
          </div>
          <div class="flex items-center gap-2 mt-0.5">
            {#if entry.username}
              <span class="text-xs truncate" style="color: var(--text-dim);">{entry.username}</span>
            {/if}
            {#if entry.url}
              <span class="text-xs truncate" style="color: var(--text-muted);">{domainFromUrl(entry.url)}</span>
            {/if}
          </div>
        </div>
      </button>

      <!-- Context menu -->
      {#if contextId === entry.id}
        <div class="absolute right-2 top-2 z-50 py-1 rounded-lg shadow-lg text-xs"
             style="background: var(--surface-3); border: 1px solid var(--border); min-width: 120px;">
          <button class="w-full text-left px-3 py-1.5 hover:bg-white/5 transition-colors"
                  style="color: var(--text-dim);"
                  onclick={(e) => { e.stopPropagation(); contextId = null; onselect && onselect(entry.id); }}>
            Edit
          </button>
          <button class="w-full text-left px-3 py-1.5 hover:bg-white/5 transition-colors"
                  style="color: var(--danger);"
                  onclick={(e) => { e.stopPropagation(); contextId = null; ondelete && ondelete(entry.id); }}>
            Delete
          </button>
        </div>
      {/if}
    </div>
  {/each}
{/if}
