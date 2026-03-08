<script>
  /**
   * GroupTree — Recursive tree of key groups.
   * Supports select, rename (double-click), delete, and create child group.
   */

  let {
    groups = [],
    selectedGroupId = null,
    parentId = null,       // internal: which parent level to render
    depth = 0,             // internal: nesting depth
    onselect = null,
    onrename = null,
    ondelete = null,
    oncreate = null,
  } = $props();

  let editingId = $state(null);
  let editName = $state('');
  let contextId = $state(null);

  // Filter groups for this level
  function childGroups(pid) {
    return groups.filter(g => {
      if (pid === null) return g.parentId === null;
      return g.parentId === pid;
    });
  }

  function startRename(g) {
    editingId = g.id;
    editName = g.name;
  }

  function commitRename() {
    if (editingId && editName.trim() && onrename) {
      onrename(editingId, editName.trim());
    }
    editingId = null;
  }

  function handleKeydown(e) {
    if (e.key === 'Enter') commitRename();
    if (e.key === 'Escape') editingId = null;
  }

  function toggleContext(id, e) {
    e.stopPropagation();
    contextId = contextId === id ? null : id;
  }
</script>

{#each childGroups(parentId) as group (group.id)}
  {@const isSelected = selectedGroupId === group.id}
  {@const isRoot = group.parentId === null}
  {@const hasChildren = groups.some(g => g.parentId === group.id)}
  
  <div>
    <!-- Group row -->
    <div
      class="flex items-center gap-2 py-1.5 px-2 text-[13px] cursor-pointer transition-colors group relative"
      style="padding-left: {14 + depth * 16}px;
             color: {isSelected ? 'var(--accent)' : 'var(--text-dim)'};
             background: {isSelected ? 'var(--accent-dim)' : 'transparent'};"
      onclick={() => onselect && onselect(group.id)}
      ondblclick={() => !isRoot && startRename(group)}
      role="button"
      tabindex="0"
    >
      <!-- Folder icon -->
      <svg class="w-4 h-4 shrink-0" fill="none" stroke="currentColor" viewBox="0 0 24 24"
           style="color: {isSelected ? 'var(--accent)' : 'var(--text-muted)'};">
        {#if hasChildren}
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                d="M3 7v10a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-6l-2-2H5a2 2 0 00-2 2z" />
        {:else}
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                d="M3 7v10a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-6l-2-2H5a2 2 0 00-2 2z" />
        {/if}
      </svg>

      <!-- Name / edit input -->
      {#if editingId === group.id}
        <input
          type="text"
          bind:value={editName}
          onblur={commitRename}
          onkeydown={handleKeydown}
          class="flex-1 min-w-0 text-[13px] py-0.5 px-1.5"
          style="background: var(--surface-2); border: 1px solid var(--accent); border-radius: 4px;"
          autofocus
        />
      {:else}
        <span class="truncate flex-1">{group.name}</span>
      {/if}

      <!-- Context menu button (show on hover) -->
      {#if !isRoot}
        <button
          class="w-5 h-5 flex items-center justify-center rounded-md opacity-0 group-hover:opacity-100 transition-opacity hover:bg-white/10"
          onclick={(e) => toggleContext(group.id, e)}
          title="More"
        >
          <svg class="w-3.5 h-3.5" fill="currentColor" viewBox="0 0 20 20">
            <path d="M6 10a2 2 0 11-4 0 2 2 0 014 0zm6 0a2 2 0 11-4 0 2 2 0 014 0zm6 0a2 2 0 11-4 0 2 2 0 014 0z" />
          </svg>
        </button>
      {/if}
    </div>

    <!-- Context menu dropdown -->
    {#if contextId === group.id}
      <div class="ml-6 mb-1 py-1 rounded-lg text-xs"
           style="background: var(--surface-3); border: 1px solid var(--border);">
        <button class="w-full text-left px-3 py-1.5 hover:bg-white/5 transition-colors"
                style="color: var(--text-dim);"
                onclick={() => { contextId = null; oncreate && oncreate(group.id); }}>
          New sub-group
        </button>
        <button class="w-full text-left px-3 py-1.5 hover:bg-white/5 transition-colors"
                style="color: var(--text-dim);"
                onclick={() => { contextId = null; startRename(group); }}>
          Rename
        </button>
        <button class="w-full text-left px-3 py-1.5 hover:bg-white/5 transition-colors"
                style="color: var(--danger);"
                onclick={() => { contextId = null; ondelete && ondelete(group.id); }}>
          Delete
        </button>
      </div>
    {/if}

    <!-- Recursive children -->
    {#if hasChildren}
      <svelte:self
        {groups}
        {selectedGroupId}
        parentId={group.id}
        depth={depth + 1}
        {onselect}
        {onrename}
        {ondelete}
        {oncreate}
      />
    {/if}
  </div>
{/each}
