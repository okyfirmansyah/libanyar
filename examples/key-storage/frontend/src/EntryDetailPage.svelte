<script>
  /**
   * EntryDetailPage — Standalone page for entry detail in a native modal window.
   *
   * Loaded when the hash route is /#/entry/:id.
   * Communicates with the main window via LibAnyar events.
   * All data operations (update, delete) go directly through IPC invoke
   * since this window shares the same backend server.
   */
  import { invoke, emit, listen, getLabel, closeWindow } from '@libanyar/api';
  import EntryDetail from './EntryDetail.svelte';

  let { entryId } = $props();

  let entry = $state(null);
  let loading = $state(true);
  let error = $state('');

  // Load the entry on mount
  $effect(() => {
    if (entryId) {
      loadEntry(entryId);
    }
  });

  // Listen for external refresh requests (e.g. main window says "refresh")
  $effect(() => {
    const unlisten = listen('entry:refresh', (msg) => {
      if (msg.payload?.id === entryId) {
        loadEntry(entryId);
      }
    });
    return () => { unlisten(); };
  });

  async function loadEntry(id) {
    try {
      loading = true;
      error = '';
      const res = await invoke('ks:get_entry', { id });
      entry = res.entry;
    } catch (e) {
      error = e.message || 'Failed to load entry';
    } finally {
      loading = false;
    }
  }

  async function handleUpdate(data) {
    try {
      await invoke('ks:update_entry', data);
      // Reload locally to reflect changes
      await loadEntry(entryId);
      // Notify main window to refresh its list
      emit('entry:updated', { id: data.id });
    } catch (e) {
      error = e.message || 'Failed to update entry';
    }
  }

  async function handleDelete() {
    try {
      await invoke('ks:delete_entry', { id: entryId });
      // Notify main window
      emit('entry:deleted', { id: entryId });
      // Close this modal window
      closeWindow();
    } catch (e) {
      error = e.message || 'Failed to delete entry';
    }
  }

  async function handleRefresh() {
    await loadEntry(entryId);
  }

  function handleClose() {
    closeWindow();
  }
</script>

<main class="h-screen flex flex-col overflow-hidden" style="background: var(--bg);">
  <!-- Title bar with close button -->
  <header class="shrink-0 flex items-center justify-between px-4 py-2"
          style="background: var(--surface); border-bottom: 1px solid var(--border);">
    <div class="flex items-center gap-2">
      <svg class="w-4 h-4" style="color: var(--accent);" fill="none" stroke="currentColor" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
              d="M15 7a2 2 0 012 2m4 0a6 6 0 01-7.743 5.743L11 17H9v2H7v2H4a1 1 0 01-1-1v-2.586a1 1 0 01.293-.707l5.964-5.964A6 6 0 1121 9z" />
      </svg>
      <span class="text-sm font-medium" style="color: var(--text);">
        {entry ? entry.title : 'Entry Detail'}
      </span>
    </div>
    <button
      class="w-7 h-7 flex items-center justify-center rounded-md hover:bg-white/10 transition-colors"
      style="color: var(--text-muted);"
      onclick={handleClose}
      title="Close"
    >
      <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M6 18L18 6M6 6l12 12" />
      </svg>
    </button>
  </header>

  <!-- Error bar -->
  {#if error}
    <div class="px-4 py-1.5 text-xs flex items-center gap-2 shrink-0"
         style="background: var(--danger-dim); color: var(--danger);">
      <span class="flex-1">{error}</span>
      <button class="opacity-60 hover:opacity-100" onclick={() => error = ''}>✕</button>
    </div>
  {/if}

  <!-- Content -->
  {#if loading}
    <div class="flex-1 flex items-center justify-center">
      <span class="text-sm" style="color: var(--text-muted);">Loading...</span>
    </div>
  {:else if entry}
    <div class="flex-1 overflow-y-auto">
      <EntryDetail
        {entry}
        onupdate={handleUpdate}
        ondelete={handleDelete}
        onrefresh={handleRefresh}
      />
    </div>
  {:else}
    <div class="flex-1 flex items-center justify-center">
      <span class="text-sm" style="color: var(--text-muted);">Entry not found</span>
    </div>
  {/if}

  <!-- Status bar -->
  <footer class="shrink-0 px-4 py-1 flex items-center justify-between text-xs"
          style="background: var(--surface); border-top: 1px solid var(--border); color: var(--text-muted);">
    <span>Entry #{entryId}</span>
    <span>Auto-saves on change</span>
  </footer>
</main>
