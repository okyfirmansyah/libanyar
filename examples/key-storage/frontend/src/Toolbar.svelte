<script>
  /**
   * Toolbar — Top bar with file operations, search, and context-aware actions.
   */

  let {
    dbOpen = false,
    dbDirty = false,
    dbPath = '',
    searchQuery = '',
    onnew = null,
    onopen = null,
    onsave = null,
    onlock = null,
    onclose = null,
    onsearch = null,
    onclearsearch = null,
    oncreateentry = null,
  } = $props();

  let searchFocused = $state(false);

  function handleSearchInput(e) {
    onsearch?.(e.target.value);
  }

  function handleSearchKeydown(e) {
    if (e.key === 'Escape') {
      e.target.value = '';
      e.target.blur();
      onclearsearch?.();
    }
  }

  function fileName(path) {
    return path ? path.split('/').pop() : '';
  }
</script>

<header class="shrink-0 flex items-center gap-2 px-3 py-2.5"
        style="background: var(--surface); border-bottom: 1px solid var(--border); box-shadow: 0 1px 0 0 var(--gold-dim);">
  <!-- File operations - always visible -->
  <div class="flex items-center gap-1">
    <button class="toolbar-btn" onclick={onnew} title="New vault (Ctrl+Shift+N)">
      <svg class="w-[18px] h-[18px]" fill="none" stroke="currentColor" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
              d="M12 4v16m8-8H4" />
      </svg>
    </button>
    <button class="toolbar-btn" onclick={onopen} title="Open vault (Ctrl+O)">
      <svg class="w-[18px] h-[18px]" fill="none" stroke="currentColor" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
              d="M5 19a2 2 0 01-2-2V7a2 2 0 012-2h4l2 2h4a2 2 0 012 2v1M5 19h14a2 2 0 002-2v-5a2 2 0 00-2-2H9a2 2 0 00-2 2v5a2 2 0 01-2 2z" />
      </svg>
    </button>

    {#if dbOpen}
      <button class="toolbar-btn{dbDirty ? ' save-pulse' : ''}" onclick={onsave} title="Save (Ctrl+S)"
              style="{dbDirty ? '' : 'color: var(--text-dim);'}">
        <svg class="w-[18px] h-[18px]" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                d="M8 7H5a2 2 0 00-2 2v9a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-3m-1 4l-3 3m0 0l-3-3m3 3V4" />
        </svg>
      </button>
    {/if}
  </div>

  {#if dbOpen}
    <!-- Separator -->
    <div class="w-px h-5 mx-1.5" style="background: var(--border);"></div>

    <!-- Entry operations -->
    <div class="flex items-center gap-1">
      <button class="toolbar-btn" onclick={oncreateentry} title="New entry (Ctrl+N)">
        <svg class="w-[18px] h-[18px]" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                d="M15 7a2 2 0 012 2m4 0a6 6 0 01-7.743 5.743L11 17H9v2H7v2H4a1 1 0 01-1-1v-2.586a1 1 0 01.293-.707l5.964-5.964A6 6 0 1121 9z" />
        </svg>
        <span class="text-[13px] ml-1">Entry</span>
      </button>
    </div>

    <!-- Separator -->
    <div class="w-px h-5 mx-1.5" style="background: var(--border);"></div>

    <!-- Search -->
    <div class="relative flex-1 max-w-xs">
      <svg class="w-4 h-4 absolute left-3 top-1/2 -translate-y-1/2 pointer-events-none"
           style="color: var(--text-muted);" fill="none" stroke="currentColor" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
              d="M21 21l-6-6m2-5a7 7 0 11-14 0 7 7 0 0114 0z" />
      </svg>
      <input
        type="text"
        value={searchQuery}
        placeholder="Search entries…"
        class="w-full text-[13px] rounded-lg"
        style="background: var(--surface-2); border: 1px solid {searchFocused ? 'var(--accent)' : 'var(--border)'}; color: var(--text); padding: 7px 1.75rem 7px 2.25rem;"
        oninput={handleSearchInput}
        onkeydown={handleSearchKeydown}
        onfocus={() => searchFocused = true}
        onblur={() => searchFocused = false}
      />
      {#if searchQuery}
        <button
          class="absolute right-1.5 top-1/2 -translate-y-1/2 w-4 h-4 flex items-center justify-center rounded-sm hover:bg-white/10"
          style="color: var(--text-muted);"
          onclick={() => { onclearsearch?.(); }}
        >✕</button>
      {/if}
    </div>

    <!-- Spacer -->
    <div class="flex-1"></div>

    <!-- Right-side actions -->
    <div class="flex items-center gap-1">
      <button class="toolbar-btn" onclick={onlock} title="Lock vault">
        <svg class="w-[18px] h-[18px]" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                d="M12 15v2m-6 4h12a2 2 0 002-2v-6a2 2 0 00-2-2H6a2 2 0 00-2 2v6a2 2 0 002 2zm10-10V7a4 4 0 00-8 0v4h8z" />
        </svg>
      </button>
      <button class="toolbar-btn" onclick={onclose} title="Close vault">
        <svg class="w-[18px] h-[18px]" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12" />
        </svg>
      </button>
    </div>
  {:else}
    <!-- Spacer for when db isn't open -->
    <div class="flex-1"></div>
  {/if}

  <!-- App title (right side) -->
  <div class="text-[13px] font-medium ml-2 truncate" style="color: var(--text-dim);">
    {#if dbOpen}
      {fileName(dbPath)}
    {:else}
      Key Storage
    {/if}
  </div>
</header>

<style>
  :global(.toolbar-btn) {
    display: flex;
    align-items: center;
    padding: 6px 10px;
    border-radius: 8px;
    color: var(--text-dim);
    transition: background 0.15s ease, color 0.15s ease, filter 0.3s ease;
    font-size: 13px;
  }
  :global(.toolbar-btn:hover) {
    background: var(--surface-2);
    color: var(--text);
  }
</style>
