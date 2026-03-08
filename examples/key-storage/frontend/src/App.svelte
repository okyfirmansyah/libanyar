<script>
  import { invoke } from '@libanyar/api';
  import GroupTree from './GroupTree.svelte';
  import EntryList from './EntryList.svelte';
  import EntryDetail from './EntryDetail.svelte';
  import UnlockDialog from './UnlockDialog.svelte';
  import Toolbar from './Toolbar.svelte';

  // ── App state ──────────────────────────────────────────────────────────────
  let dbOpen = $state(false);
  let dbPath = $state('');
  let dbDirty = $state(false);
  let dbLocked = $state(false);

  let groups = $state([]);
  let selectedGroupId = $state(null);  // null = show all entries
  let entries = $state([]);
  let selectedEntryId = $state(null);
  let selectedEntry = $state(null);
  let searchQuery = $state('');
  let searchMode = $state(false);

  // Unlock dialog state
  let showUnlock = $state(false);
  let unlockMode = $state('open');  // 'open' | 'new' | 'reopen'
  let unlockPath = $state('');

  let errorMsg = $state('');
  let statusMsg = $state('');

  // ── Data loading ──────────────────────────────────────────────────────────
  async function loadGroups() {
    try {
      const res = await invoke('ks:groups');
      groups = res.groups;
    } catch (e) { errorMsg = e.message; }
  }

  async function loadEntries() {
    try {
      if (searchMode && searchQuery.trim()) {
        const res = await invoke('ks:search', { query: searchQuery.trim() });
        entries = res.entries;
      } else if (selectedGroupId != null) {
        const res = await invoke('ks:entries', { groupId: selectedGroupId });
        entries = res.entries;
      } else {
        const res = await invoke('ks:entries', {});
        entries = res.entries;
      }
    } catch (e) { errorMsg = e.message; }
  }

  async function loadEntry(id) {
    try {
      const res = await invoke('ks:get_entry', { id });
      selectedEntry = res.entry;
    } catch (e) { errorMsg = e.message; }
  }

  async function refreshAll() {
    await loadGroups();
    await loadEntries();
    if (selectedEntryId != null) await loadEntry(selectedEntryId);
  }

  // ── File operations ───────────────────────────────────────────────────────
  async function handleNew() {
    try {
      let path = await invoke('dialog:save', {
        title: 'Create New Key Storage',
        filters: [{ name: 'AnyarKS Files', extensions: ['anyarks'] }]
      });
      if (!path) return;
      // GTK save dialog doesn't auto-append extension
      if (!path.endsWith('.anyarks')) path += '.anyarks';
      unlockPath = path;
      unlockMode = 'new';
      showUnlock = true;
    } catch (e) { errorMsg = e.message; }
  }

  async function handleOpen() {
    try {
      const paths = await invoke('dialog:open', {
        title: 'Open Key Storage',
        filters: [
          { name: 'AnyarKS Files', extensions: ['anyarks'] },
          { name: 'All Files', extensions: ['*'] }
        ]
      });
      if (!paths || paths.length === 0) return;
      unlockPath = paths[0];
      unlockMode = 'open';
      showUnlock = true;
    } catch (e) { errorMsg = e.message; }
  }

  async function handleUnlock(password) {
    try {
      errorMsg = '';
      if (unlockMode === 'new') {
        await invoke('ks:new', { path: unlockPath, password });
      } else {
        await invoke('ks:open', { path: unlockPath, password });
      }
      dbOpen = true;
      dbPath = unlockPath;
      dbLocked = false;
      dbDirty = false;
      showUnlock = false;
      selectedGroupId = null;
      selectedEntryId = null;
      selectedEntry = null;
      searchQuery = '';
      searchMode = false;
      await refreshAll();
      statusMsg = `Opened: ${dbPath.split('/').pop()}`;
    } catch (e) {
      throw e;  // Let UnlockDialog show the error
    }
  }

  async function handleSave() {
    try {
      await invoke('ks:save');
      dbDirty = false;
      statusMsg = 'Saved';
      setTimeout(() => { if (statusMsg === 'Saved') statusMsg = ''; }, 2000);
    } catch (e) { errorMsg = e.message; }
  }

  async function handleLock() {
    try {
      const res = await invoke('ks:lock');
      dbOpen = false;
      dbLocked = true;
      unlockPath = res.path || dbPath;
      groups = [];
      entries = [];
      selectedEntry = null;
      selectedEntryId = null;
      unlockMode = 'reopen';
      showUnlock = true;
    } catch (e) { errorMsg = e.message; }
  }

  async function handleClose() {
    try {
      await invoke('ks:close');
      dbOpen = false;
      dbPath = '';
      dbDirty = false;
      dbLocked = false;
      groups = [];
      entries = [];
      selectedEntry = null;
      selectedEntryId = null;
      searchQuery = '';
      searchMode = false;
      statusMsg = '';
    } catch (e) { errorMsg = e.message; }
  }

  // ── Group operations ──────────────────────────────────────────────────────
  function handleSelectGroup(id) {
    selectedGroupId = id;
    selectedEntryId = null;
    selectedEntry = null;
    searchMode = false;
    searchQuery = '';
    loadEntries();
  }

  async function handleCreateGroup(parentId) {
    try {
      const res = await invoke('ks:create_group', { parentId: parentId || 1, name: 'New Group' });
      dbDirty = true;
      await loadGroups();
      return res.id;
    } catch (e) { errorMsg = e.message; }
  }

  async function handleRenameGroup(id, name) {
    try {
      await invoke('ks:rename_group', { id, name });
      dbDirty = true;
      await loadGroups();
    } catch (e) { errorMsg = e.message; }
  }

  async function handleDeleteGroup(id) {
    try {
      await invoke('ks:delete_group', { id });
      dbDirty = true;
      if (selectedGroupId === id) selectedGroupId = null;
      await refreshAll();
    } catch (e) { errorMsg = e.message; }
  }

  // ── Entry operations ──────────────────────────────────────────────────────
  function handleSelectEntry(id) {
    selectedEntryId = id;
    loadEntry(id);
  }

  async function handleCreateEntry() {
    try {
      const gid = selectedGroupId || 1;
      const res = await invoke('ks:create_entry', { groupId: gid, title: 'New Entry' });
      dbDirty = true;
      await loadEntries();
      handleSelectEntry(res.id);
    } catch (e) { errorMsg = e.message; }
  }

  async function handleUpdateEntry(data) {
    try {
      await invoke('ks:update_entry', data);
      dbDirty = true;
      await loadEntries();
      if (selectedEntryId === data.id) await loadEntry(data.id);
    } catch (e) { errorMsg = e.message; }
  }

  async function handleDeleteEntry(id) {
    try {
      await invoke('ks:delete_entry', { id });
      dbDirty = true;
      if (selectedEntryId === id) {
        selectedEntryId = null;
        selectedEntry = null;
      }
      await loadEntries();
    } catch (e) { errorMsg = e.message; }
  }

  // ── Search ────────────────────────────────────────────────────────────────
  let searchTimer = null;
  function handleSearch(query) {
    searchQuery = query;
    if (query.trim()) {
      searchMode = true;
      clearTimeout(searchTimer);
      searchTimer = setTimeout(() => loadEntries(), 200);
    } else {
      searchMode = false;
      loadEntries();
    }
  }

  function clearSearch() {
    searchQuery = '';
    searchMode = false;
    loadEntries();
  }

  // ── File name helper ──────────────────────────────────────────────────────
  function fileName(path) {
    return path ? path.split('/').pop() : '';
  }
</script>

{#if showUnlock}
  <UnlockDialog
    mode={unlockMode}
    path={unlockPath}
    onunlock={handleUnlock}
    oncancel={() => { showUnlock = false; if (!dbOpen && !dbLocked) {} }}
  />
{/if}

<main class="h-screen flex flex-col overflow-hidden" style="background: var(--bg);">
  <!-- Toolbar -->
  <Toolbar
    {dbOpen}
    {dbDirty}
    {dbPath}
    {searchQuery}
    onnew={handleNew}
    onopen={handleOpen}
    onsave={handleSave}
    onlock={handleLock}
    onclose={handleClose}
    onsearch={handleSearch}
    onclearsearch={clearSearch}
    oncreateentry={handleCreateEntry}
  />

  <!-- Error bar -->
  {#if errorMsg}
    <div class="px-4 py-1.5 text-xs flex items-center gap-2 shrink-0"
         style="background: var(--danger-dim); color: var(--danger);">
      <svg class="w-3.5 h-3.5 shrink-0" fill="none" stroke="currentColor" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
              d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-2.5L13.732 4.5c-.77-.833-2.694-.833-3.464 0L3.34 16.5c-.77.833.192 2.5 1.732 2.5z" />
      </svg>
      <span class="flex-1">{errorMsg}</span>
      <button class="opacity-60 hover:opacity-100" onclick={() => errorMsg = ''}>✕</button>
    </div>
  {/if}

  {#if dbOpen}
    <!-- Three-panel layout -->
    <div class="flex-1 flex min-h-0">
      <!-- Left sidebar: groups -->
      <aside class="flex flex-col shrink-0" style="width: 240px; background: var(--surface); border-right: 1px solid var(--border);">
        <div class="px-4 py-2.5 text-xs font-semibold uppercase tracking-wider flex items-center justify-between"
             style="color: var(--text-muted); border-bottom: 1px solid var(--border);">
          <span>Groups</span>
          <button
            class="w-6 h-6 flex items-center justify-center rounded-md hover:bg-white/5 transition-colors"
            style="color: var(--text-dim);"
            title="New group"
            onclick={() => handleCreateGroup(selectedGroupId || 1)}
          >
            <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4" />
            </svg>
          </button>
        </div>

        <!-- "All Entries" item -->
        <button
          class="w-full text-left px-4 py-2 text-[13px] flex items-center gap-2.5 transition-colors"
          style="color: {selectedGroupId == null && !searchMode ? 'var(--accent)' : 'var(--text-dim)'}; background: {selectedGroupId == null && !searchMode ? 'var(--accent-dim)' : 'transparent'};"
          onclick={() => handleSelectGroup(null)}
        >
          <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 11H5m14 0a2 2 0 012 2v6a2 2 0 01-2 2H5a2 2 0 01-2-2v-6a2 2 0 012-2m14 0V9a2 2 0 00-2-2M5 11V9a2 2 0 012-2m0 0V5a2 2 0 012-2h6a2 2 0 012 2v2M7 7h10" />
          </svg>
          All Entries
        </button>

        <!-- Group tree -->
        <div class="flex-1 overflow-y-auto py-1">
          <GroupTree
            {groups}
            {selectedGroupId}
            onselect={handleSelectGroup}
            onrename={handleRenameGroup}
            ondelete={handleDeleteGroup}
            oncreate={handleCreateGroup}
          />
        </div>
      </aside>

      <!-- Center: entry list -->
      <section class="flex flex-col shrink-0" style="width: 320px; border-right: 1px solid var(--border);">
        <div class="px-4 py-2.5 text-xs font-semibold uppercase tracking-wider flex items-center justify-between"
             style="color: var(--text-muted); border-bottom: 1px solid var(--border);">
          <span>
            {#if searchMode}
              Search: "{searchQuery}"
            {:else if selectedGroupId == null}
              All Entries
            {:else}
              {groups.find(g => g.id === selectedGroupId)?.name || 'Entries'}
            {/if}
          </span>
          <span class="text-[11px] font-normal" style="color: var(--text-muted);">{entries.length}</span>
        </div>
        <div class="flex-1 overflow-y-auto">
          <EntryList
            {entries}
            {selectedEntryId}
            onselect={handleSelectEntry}
            ondelete={handleDeleteEntry}
          />
        </div>
      </section>

      <!-- Right: entry detail -->
      <section class="flex-1 min-w-0 flex flex-col overflow-y-auto" style="background: var(--bg); border-left: 1px solid var(--border);">
        {#if selectedEntry}
          <EntryDetail
            entry={selectedEntry}
            onupdate={handleUpdateEntry}
            ondelete={() => handleDeleteEntry(selectedEntry.id)}
            onrefresh={() => loadEntry(selectedEntry.id)}
          />
        {:else}
          <div class="flex-1 flex items-center justify-center" style="color: var(--text-muted);">
            <div class="text-center">
              <svg class="w-12 h-12 mx-auto mb-3 opacity-30" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5"
                      d="M15 7a2 2 0 012 2m4 0a6 6 0 01-7.743 5.743L11 17H9v2H7v2H4a1 1 0 01-1-1v-2.586a1 1 0 01.293-.707l5.964-5.964A6 6 0 1121 9z" />
              </svg>
              <p class="text-sm">Select an entry to view details</p>
              <p class="text-xs mt-1 opacity-50">or press Ctrl+N to create one</p>
            </div>
          </div>
        {/if}
      </section>
    </div>
  {:else}
    <!-- Welcome / empty state -->
    <div class="flex-1 w-full flex items-center justify-center">
      <div class="flex flex-col items-center text-center max-w-sm">
        <div class="w-20 h-20 mx-auto mb-6 rounded-2xl flex items-center justify-center"
             style="background: var(--accent-dim);">
          <svg class="w-10 h-10" style="color: var(--accent);" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5"
                  d="M12 15v2m-6 4h12a2 2 0 002-2v-6a2 2 0 00-2-2H6a2 2 0 00-2 2v6a2 2 0 002 2zm10-10V7a4 4 0 00-8 0v4h8z" />
          </svg>
        </div>
        <h2 class="text-xl font-semibold mb-2">Secure Key Storage</h2>
        <p class="text-sm mb-6" style="color: var(--text-dim);">
          Create a new encrypted vault or open an existing one to manage your credentials securely.
        </p>
        <div class="flex gap-3 justify-center">
          <button class="btn btn-primary" onclick={handleNew}>
            <svg fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 4v16m8-8H4" />
            </svg>
            New Vault
          </button>
          <button class="btn btn-secondary" onclick={handleOpen}>
            <svg fill="none" stroke="currentColor" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M5 19a2 2 0 01-2-2V7a2 2 0 012-2h4l2 2h4a2 2 0 012 2v1M5 19h14a2 2 0 002-2v-5a2 2 0 00-2-2H9a2 2 0 00-2 2v5a2 2 0 01-2 2z" />
            </svg>
            Open Vault
          </button>
        </div>
      </div>
    </div>
  {/if}

  <!-- Status bar -->
  <footer class="shrink-0 px-4 py-1.5 flex items-center justify-between text-xs"
          style="background: var(--surface); border-top: 1px solid var(--border); color: var(--text-muted);">
    <div class="flex items-center gap-3">
      {#if dbOpen}
        <span class="flex items-center gap-1">
          <span class="w-1.5 h-1.5 rounded-full" style="background: {dbDirty ? 'var(--warning)' : 'var(--success)'};"></span>
          {dbDirty ? 'Unsaved changes' : 'Saved'}
        </span>
        <span style="color: var(--border);">|</span>
        <span>{fileName(dbPath)}</span>
      {:else}
        <span>No vault open</span>
      {/if}
    </div>
    <div>
      {#if statusMsg}
        <span>{statusMsg}</span>
      {:else}
        <span>AES-256-GCM + PBKDF2</span>
      {/if}
    </div>
  </footer>
</main>
