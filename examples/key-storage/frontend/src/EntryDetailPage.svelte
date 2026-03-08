<script>
  /**
   * EntryDetailPage — Standalone dialog for entry detail in a native child window.
   *
   * Loaded when the hash route is /#/entry/:id.
   * Uses Save/Cancel pattern (no auto-save). Enter = Save, Escape = Cancel.
   * Communicates with the main window via LibAnyar events.
   */
  import { invoke, emit, listen, closeWindow } from '@libanyar/api';

  let { entryId } = $props();

  let entry = $state(null);
  let draft = $state(null);        // working copy for editing
  let loading = $state(true);
  let saving = $state(false);
  let error = $state('');
  let showPassword = $state(false);
  let copiedField = $state('');
  let iconUrl = $state(null);
  let attachments = $state([]);
  let dirty = $state(false);       // has unsaved changes

  // Load the entry on mount
  $effect(() => {
    if (entryId) {
      loadEntry(entryId);
    }
  });

  // Listen for external refresh requests
  $effect(() => {
    const unlisten = listen('entry:refresh', (msg) => {
      if (msg.payload?.id === entryId) {
        loadEntry(entryId);
      }
    });
    return () => { unlisten(); };
  });

  // Global keyboard shortcuts
  $effect(() => {
    function onKeydown(e) {
      if (e.key === 'Escape') {
        e.preventDefault();
        handleCancel();
      }
      if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
        e.preventDefault();
        handleSave();
      }
    }
    window.addEventListener('keydown', onKeydown);
    return () => window.removeEventListener('keydown', onKeydown);
  });

  async function loadEntry(id) {
    try {
      loading = true;
      error = '';
      const res = await invoke('ks:get_entry', { id });
      entry = res.entry;
      draft = { ...res.entry };
      dirty = false;
      attachments = res.entry.attachments || [];
      // Load icon
      if (entry.hasIcon) {
        try {
          const iconRes = await invoke('ks:get_icon', { entryId: entry.id });
          iconUrl = iconRes.dataBase64
            ? `data:${iconRes.mimeType || 'image/png'};base64,${iconRes.dataBase64}`
            : null;
        } catch { iconUrl = null; }
      } else {
        iconUrl = null;
      }
    } catch (e) {
      error = e.message || 'Failed to load entry';
    } finally {
      loading = false;
    }
  }

  function markDirty() {
    dirty = true;
  }

  function updateDraft(field, value) {
    draft = { ...draft, [field]: value };
    markDirty();
  }

  async function handleSave() {
    if (!dirty || !draft) return;
    try {
      saving = true;
      error = '';
      await invoke('ks:update_entry', {
        id: draft.id,
        title: draft.title,
        url: draft.url,
        username: draft.username,
        password: draft.password,
        notes: draft.notes,
        expiresAt: draft.expiresAt,
      });
      // Notify main window to refresh + mark dirty
      emit('entry:updated', { id: draft.id });
      // Close on successful save
      closeWindow();
    } catch (e) {
      error = e.message || 'Failed to save entry';
    } finally {
      saving = false;
    }
  }

  function handleCancel() {
    closeWindow();
  }

  // ── Copy to clipboard ─────────────────────────────────────────────────────
  async function copyToClipboard(field, value) {
    try {
      await navigator.clipboard.writeText(value);
    } catch {
      const ta = document.createElement('textarea');
      ta.value = value;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
    }
    copiedField = field;
    setTimeout(() => { if (copiedField === field) copiedField = ''; }, 1500);
  }

  // ── Password generator ────────────────────────────────────────────────────
  function generatePassword(length = 20) {
    const charset = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()-_=+[]{}|;:,.<>?';
    const arr = new Uint8Array(length);
    crypto.getRandomValues(arr);
    return Array.from(arr, b => charset[b % charset.length]).join('');
  }

  // ── Icon upload ───────────────────────────────────────────────────────────
  function handleIconUpload() {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = 'image/*';
    input.onchange = async (e) => {
      const file = e.target.files?.[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = async () => {
        const b64 = reader.result.split(',')[1];
        await invoke('ks:set_icon', {
          entryId: entry.id,
          dataBase64: b64,
          mimeType: file.type || 'image/png',
        });
        await loadEntry(entryId);
      };
      reader.readAsDataURL(file);
    };
    input.click();
  }

  // ── Attachment upload ─────────────────────────────────────────────────────
  function handleAttachmentUpload() {
    const input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    input.onchange = async (e) => {
      for (const file of e.target.files) {
        const reader = new FileReader();
        reader.onload = async () => {
          const b64 = reader.result.split(',')[1];
          await invoke('ks:add_attachment', {
            entryId: entry.id,
            name: file.name,
            dataBase64: b64,
            mimeType: file.type || 'application/octet-stream',
          });
          await loadEntry(entryId);
        };
        reader.readAsDataURL(file);
      }
    };
    input.click();
  }

  async function handleDeleteAttachment(attId) {
    await invoke('ks:delete_attachment', { id: attId });
    await loadEntry(entryId);
  }

  async function handleDownloadAttachment(attId) {
    const res = await invoke('ks:get_attachment', { id: attId });
    const blob = await fetch(`data:${res.mimeType};base64,${res.dataBase64}`).then(r => r.blob());
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = res.name;
    a.click();
    URL.revokeObjectURL(url);
  }

  function formatBytes(bytes) {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
    return (bytes / 1048576).toFixed(1) + ' MB';
  }
</script>

<main class="entry-dialog">
  <!-- Error bar -->
  {#if error}
    <div class="error-bar">
      <span class="flex-1">{error}</span>
      <button class="opacity-60 hover:opacity-100 bg-transparent"
              style="color: var(--danger);" onclick={() => error = ''}>✕</button>
    </div>
  {/if}

  {#if loading}
    <div class="flex-1 flex items-center justify-center">
      <span class="text-sm" style="color: var(--text-muted);">Loading...</span>
    </div>
  {:else if draft}
    <!-- Scrollable content -->
    <div class="dialog-body">
      <!-- Header: icon + title + metadata -->
      <div class="entry-header">
        <button class="icon-button" onclick={handleIconUpload} title="Change icon">
          {#if iconUrl}
            <img src={iconUrl} alt="" class="w-full h-full object-cover rounded-lg" />
          {:else}
            <span class="icon-letter">
              {draft.title ? draft.title[0].toUpperCase() : '?'}
            </span>
          {/if}
        </button>
        <div class="flex-1 min-w-0">
          <input
            type="text"
            value={draft.title}
            placeholder="Entry title"
            class="title-input"
            oninput={(e) => updateDraft('title', e.target.value)}
          />
          <p class="meta-text">
            Created {entry.createdAt} · Updated {entry.updatedAt}
          </p>
        </div>
      </div>

      <!-- Form fields -->
      <div class="field-grid">
        <!-- URL -->
        <div class="field-group">
          <label class="field-label">Website / URL</label>
          <div class="field-row">
            <input
              type="text"
              value={draft.url}
              placeholder="https://example.com"
              class="flex-1"
              oninput={(e) => updateDraft('url', e.target.value)}
            />
            {#if draft.url}
              <button class="copy-btn" onclick={() => copyToClipboard('url', draft.url)}>
                {copiedField === 'url' ? '✓' : 'Copy'}
              </button>
            {/if}
          </div>
        </div>

        <!-- Username -->
        <div class="field-group">
          <label class="field-label">Username</label>
          <div class="field-row">
            <input
              type="text"
              value={draft.username}
              placeholder="username or email"
              class="flex-1"
              oninput={(e) => updateDraft('username', e.target.value)}
            />
            {#if draft.username}
              <button class="copy-btn" onclick={() => copyToClipboard('username', draft.username)}>
                {copiedField === 'username' ? '✓' : 'Copy'}
              </button>
            {/if}
          </div>
        </div>

        <!-- Password -->
        <div class="field-group">
          <label class="field-label">Password</label>
          <div class="field-row">
            <div class="password-wrap">
              <input
                type={showPassword ? 'text' : 'password'}
                value={draft.password}
                placeholder="password"
                class="password-input"
                oninput={(e) => updateDraft('password', e.target.value)}
              />
              <button class="toggle-vis" onclick={() => showPassword = !showPassword}
                      title={showPassword ? 'Hide' : 'Show'}>
                {#if showPassword}
                  <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                    <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                          d="M13.875 18.825A10.05 10.05 0 0112 19c-4.478 0-8.268-2.943-9.543-7a9.97 9.97 0 011.563-3.029m5.858.908a3 3 0 114.243 4.243M9.878 9.878l4.242 4.242M9.878 9.878L3 3m6.878 6.878L21 21" />
                  </svg>
                {:else}
                  <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                    <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                          d="M15 12a3 3 0 11-6 0 3 3 0 016 0z" />
                    <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                          d="M2.458 12C3.732 7.943 7.523 5 12 5c4.478 0 8.268 2.943 9.542 7-1.274 4.057-5.064 7-9.542 7-4.477 0-8.268-2.943-9.542-7z" />
                  </svg>
                {/if}
              </button>
            </div>
            <button class="copy-btn" onclick={() => copyToClipboard('password', draft.password)}>
              {copiedField === 'password' ? '✓' : 'Copy'}
            </button>
            <button class="generate-btn" onclick={() => { updateDraft('password', generatePassword()); }}>
              Generate
            </button>
          </div>
        </div>

        <!-- Expiration -->
        <div class="field-group">
          <label class="field-label">Expiration Date</label>
          <input
            type="datetime-local"
            value={draft.expiresAt ? draft.expiresAt.replace(' ', 'T').slice(0, 16) : ''}
            class="w-auto"
            oninput={(e) => {
              const val = e.target.value ? e.target.value.replace('T', ' ') + ':00' : null;
              updateDraft('expiresAt', val);
            }}
          />
        </div>

        <!-- Notes -->
        <div class="field-group">
          <label class="field-label">Notes</label>
          <textarea
            value={draft.notes}
            placeholder="Additional notes..."
            rows="3"
            class="w-full resize-y"
            style="min-height: 72px;"
            oninput={(e) => updateDraft('notes', e.target.value)}
          ></textarea>
        </div>

        <!-- Attachments -->
        <div class="field-group">
          <div class="flex items-center justify-between mb-1.5">
            <label class="field-label" style="margin-bottom: 0;">Attachments</label>
            <button class="copy-btn" onclick={handleAttachmentUpload}>+ Add File</button>
          </div>
          {#if attachments.length > 0}
            <div class="attachment-list">
              {#each attachments as att (att.id)}
                <div class="attachment-item">
                  <svg class="w-3.5 h-3.5 shrink-0" style="color: var(--text-muted);" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                    <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                          d="M15.172 7l-6.586 6.586a2 2 0 102.828 2.828l6.414-6.586a4 4 0 00-5.656-5.656l-6.415 6.585a6 6 0 108.486 8.486L20.5 13" />
                  </svg>
                  <span class="text-xs flex-1 truncate" style="color: var(--text-dim);">{att.name}</span>
                  <span class="text-[10px]" style="color: var(--text-muted);">{formatBytes(att.size)}</span>
                  <button class="att-btn" style="color: var(--accent);"
                          onclick={() => handleDownloadAttachment(att.id)}>↓</button>
                  <button class="att-btn" style="color: var(--danger);"
                          onclick={() => handleDeleteAttachment(att.id)}>✕</button>
                </div>
              {/each}
            </div>
          {:else}
            <p class="text-xs" style="color: var(--text-muted);">No attachments</p>
          {/if}
        </div>
      </div>
    </div>

    <!-- Footer with Save / Cancel -->
    <footer class="dialog-footer">
      <div class="footer-hint">
        <kbd>Ctrl+Enter</kbd> Save · <kbd>Esc</kbd> Cancel
      </div>
      <div class="footer-actions">
        <button class="btn btn-secondary btn-sm" onclick={handleCancel}>
          Cancel
        </button>
        <button class="btn btn-primary btn-sm" onclick={handleSave}
                disabled={!dirty || saving}>
          {saving ? 'Saving...' : 'Save'}
        </button>
      </div>
    </footer>
  {:else}
    <div class="flex-1 flex items-center justify-center">
      <span class="text-sm" style="color: var(--text-muted);">Entry not found</span>
    </div>
  {/if}
</main>

<style>
  .entry-dialog {
    height: 100vh;
    display: flex;
    flex-direction: column;
    overflow: hidden;
    background: var(--bg);
  }

  .error-bar {
    padding: 6px 16px;
    font-size: 12px;
    display: flex;
    align-items: center;
    gap: 8px;
    flex-shrink: 0;
    background: var(--danger-dim);
    color: var(--danger);
  }

  .dialog-body {
    flex: 1;
    overflow-y: auto;
    padding: 20px 24px 12px;
  }

  /* ── Header ─────────────────────────────────────────────────────────── */
  .entry-header {
    display: flex;
    align-items: flex-start;
    gap: 14px;
    margin-bottom: 20px;
    padding-bottom: 16px;
    border-bottom: 1px solid var(--border);
  }
  .icon-button {
    width: 48px;
    height: 48px;
    border-radius: 10px;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
    overflow: hidden;
    background: var(--surface-2);
    border: 1.5px dashed var(--border-light);
    transition: border-color 0.15s;
    cursor: pointer;
  }
  .icon-button:hover {
    border-color: var(--accent);
  }
  .icon-letter {
    font-size: 20px;
    font-weight: 700;
    color: var(--text-muted);
  }
  .title-input {
    width: 100%;
    font-size: 17px;
    font-weight: 600;
    background: transparent !important;
    border: none !important;
    padding: 0 !important;
    color: var(--text);
    outline: none;
  }
  .title-input:focus {
    border: none !important;
    box-shadow: none;
  }
  .meta-text {
    font-size: 11px;
    margin-top: 3px;
    color: var(--text-muted);
  }

  /* ── Fields ─────────────────────────────────────────────────────────── */
  .field-grid {
    display: flex;
    flex-direction: column;
    gap: 16px;
  }
  .field-group {
    display: flex;
    flex-direction: column;
  }
  .field-label {
    font-size: 11px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-muted);
    margin-bottom: 5px;
  }
  .field-row {
    display: flex;
    align-items: center;
    gap: 6px;
  }

  /* ── Inline buttons ─────────────────────────────────────────────────── */
  .copy-btn, .generate-btn, .att-btn {
    font-size: 11px;
    padding: 4px 8px;
    border-radius: 5px;
    background: transparent;
    color: var(--text-dim);
    white-space: nowrap;
    transition: background 0.12s, color 0.12s;
    flex-shrink: 0;
  }
  .copy-btn:hover, .att-btn:hover {
    background: rgba(255,255,255,0.06);
    color: var(--text);
  }
  .generate-btn {
    color: var(--accent);
    background: var(--accent-dim);
  }
  .generate-btn:hover {
    background: rgba(124,107,246,0.22);
  }

  /* ── Password ───────────────────────────────────────────────────────── */
  .password-wrap {
    flex: 1;
    position: relative;
  }
  .password-input {
    width: 100%;
    padding-right: 36px !important;
    font-family: 'Courier New', monospace;
    font-size: 13px;
  }
  .toggle-vis {
    position: absolute;
    right: 10px;
    top: 50%;
    transform: translateY(-50%);
    background: transparent;
    color: var(--text-muted);
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 2px;
  }
  .toggle-vis:hover { color: var(--text); }

  /* ── Attachments ────────────────────────────────────────────────────── */
  .attachment-list {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .attachment-item {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 6px 10px;
    border-radius: 6px;
    background: var(--surface);
    border: 1px solid var(--border);
    font-size: 12px;
  }

  /* ── Footer ─────────────────────────────────────────────────────────── */
  .dialog-footer {
    flex-shrink: 0;
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 10px 24px;
    background: var(--surface);
    border-top: 1px solid var(--border);
  }
  .footer-hint {
    font-size: 11px;
    color: var(--text-muted);
  }
  .footer-hint kbd {
    display: inline-block;
    font-family: inherit;
    font-size: 10px;
    padding: 1px 5px;
    border-radius: 3px;
    background: var(--surface-2);
    border: 1px solid var(--border);
    color: var(--text-dim);
  }
  .footer-actions {
    display: flex;
    gap: 8px;
  }
</style>