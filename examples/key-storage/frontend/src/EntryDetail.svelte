<script>
  /**
   * EntryDetail — Right panel showing full entry details.
   * Inline editing: click a field to edit, auto-saves on blur.
   * Password field: toggle visibility, copy to clipboard.
   */

  import { invoke } from '@libanyar/api';

  let {
    entry = null,
    onupdate = null,
    ondelete = null,
    onrefresh = null,
  } = $props();

  let showPassword = $state(false);
  let copiedField = $state('');
  let iconUrl = $state(null);
  let attachments = $state([]);

  // Load icon when entry changes
  $effect(() => {
    if (!entry) { iconUrl = null; return; }
    if (entry.hasIcon) {
      invoke('ks:get_icon', { entryId: entry.id }).then(res => {
        if (res.dataBase64) {
          iconUrl = `data:${res.mimeType || 'image/png'};base64,${res.dataBase64}`;
        } else {
          iconUrl = null;
        }
      }).catch(() => { iconUrl = null; });
    } else {
      iconUrl = null;
    }
    // Reset state
    showPassword = false;
    copiedField = '';
  });

  // Load attachments
  $effect(() => {
    if (!entry) { attachments = []; return; }
    attachments = entry.attachments || [];
  });

  // ── Field editing ─────────────────────────────────────────────────────────
  function saveField(field, value) {
    if (!entry || !onupdate) return;
    if (entry[field] === value) return;
    onupdate({ id: entry.id, [field]: value });
  }

  function handleBlur(field, e) {
    saveField(field, e.target.value);
  }

  function handleKeydown(field, e) {
    if (e.key === 'Enter' && field !== 'notes') {
      e.target.blur();
    }
  }

  // ── Copy to clipboard ─────────────────────────────────────────────────────
  async function copyToClipboard(field, value) {
    try {
      await navigator.clipboard.writeText(value);
      copiedField = field;
      setTimeout(() => { if (copiedField === field) copiedField = ''; }, 1500);
    } catch {
      // fallback
      const ta = document.createElement('textarea');
      ta.value = value;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
      copiedField = field;
      setTimeout(() => { if (copiedField === field) copiedField = ''; }, 1500);
    }
  }

  // ── Password generator ────────────────────────────────────────────────────
  function generatePassword(length = 20) {
    const charset = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()-_=+[]{}|;:,.<>?';
    const arr = new Uint8Array(length);
    crypto.getRandomValues(arr);
    return Array.from(arr, b => charset[b % charset.length]).join('');
  }

  function handleGeneratePassword() {
    const pw = generatePassword();
    onupdate && onupdate({ id: entry.id, password: pw });
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
        onrefresh && onrefresh();
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
          onrefresh && onrefresh();
        };
        reader.readAsDataURL(file);
      }
    };
    input.click();
  }

  async function handleDeleteAttachment(attId) {
    await invoke('ks:delete_attachment', { id: attId });
    onrefresh && onrefresh();
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

{#if entry}
<div class="p-8 max-w-2xl mx-auto w-full">
  <!-- Header: icon + title -->
  <div class="flex items-start gap-5 mb-8 pb-6" style="border-bottom: 1px solid var(--border);">
    <!-- Icon -->
    <button
      class="w-16 h-16 rounded-xl flex items-center justify-center shrink-0 transition-colors overflow-hidden"
      style="background: var(--surface-2); border: 2px dashed var(--border-light);"
      onclick={handleIconUpload}
      title="Upload icon"
    >
      {#if iconUrl}
        <img src={iconUrl} alt="" class="w-full h-full object-cover rounded-xl" />
      {:else}
        <span class="text-2xl font-bold" style="color: var(--text-muted);">
          {entry.title ? entry.title[0].toUpperCase() : '?'}
        </span>
      {/if}
    </button>

    <div class="flex-1 min-w-0 pt-1">
      <input
        type="text"
        value={entry.title}
        placeholder="Entry title"
        class="w-full text-xl font-semibold bg-transparent border-none p-0 focus:ring-0"
        style="color: var(--text); outline: none;"
        onblur={(e) => handleBlur('title', e)}
        onkeydown={(e) => handleKeydown('title', e)}
      />
      <p class="text-xs mt-1" style="color: var(--text-muted);">
        Created {entry.createdAt} &middot; Updated {entry.updatedAt}
      </p>
    </div>

    <!-- Delete -->
    <button
      class="btn btn-danger btn-sm mt-1"
      onclick={ondelete}
      title="Delete entry"
    >
      Delete
    </button>
  </div>

  <!-- Fields -->
  <div class="space-y-6">
    <!-- URL -->
    <div>
      <label class="text-xs font-medium uppercase tracking-wider mb-1.5 block" style="color: var(--text-muted);">Website / URL</label>
      <div class="flex items-center gap-2">
        <input
          type="text"
          value={entry.url}
          placeholder="https://example.com"
          class="flex-1"
          onblur={(e) => handleBlur('url', e)}
          onkeydown={(e) => handleKeydown('url', e)}
        />
        {#if entry.url}
          <button
            class="btn btn-ghost btn-xs"
            onclick={() => copyToClipboard('url', entry.url)}
          >
            {copiedField === 'url' ? '✓ Copied' : 'Copy'}
          </button>
        {/if}
      </div>
    </div>

    <!-- Username -->
    <div>
      <label class="text-xs font-medium uppercase tracking-wider mb-1.5 block" style="color: var(--text-muted);">Username</label>
      <div class="flex items-center gap-2">
        <input
          type="text"
          value={entry.username}
          placeholder="username or email"
          class="flex-1"
          onblur={(e) => handleBlur('username', e)}
          onkeydown={(e) => handleKeydown('username', e)}
        />
        {#if entry.username}
          <button
            class="btn btn-ghost btn-xs"
            onclick={() => copyToClipboard('username', entry.username)}
          >
            {copiedField === 'username' ? '✓ Copied' : 'Copy'}
          </button>
        {/if}
      </div>
    </div>

    <!-- Password -->
    <div>
      <label class="text-xs font-medium uppercase tracking-wider mb-1.5 block" style="color: var(--text-muted);">Password</label>
      <div class="flex items-center gap-2">
        <div class="flex-1 relative">
          <input
            type={showPassword ? 'text' : 'password'}
            value={entry.password}
            placeholder="password"
            class="w-full pr-10"
            style="font-family: 'Courier New', monospace; font-size: 14px;"
            onblur={(e) => handleBlur('password', e)}
            onkeydown={(e) => handleKeydown('password', e)}
          />
          <button
            class="absolute right-3 top-1/2 -translate-y-1/2 w-5 h-5 flex items-center justify-center"
            style="color: var(--text-muted);"
            onclick={() => showPassword = !showPassword}
            title={showPassword ? 'Hide' : 'Show'}
          >
            {#if showPassword}
              <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13.875 18.825A10.05 10.05 0 0112 19c-4.478 0-8.268-2.943-9.543-7a9.97 9.97 0 011.563-3.029m5.858.908a3 3 0 114.243 4.243M9.878 9.878l4.242 4.242M9.878 9.878L3 3m6.878 6.878L21 21" />
              </svg>
            {:else}
              <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z" />
                <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M2.458 12C3.732 7.943 7.523 5 12 5c4.478 0 8.268 2.943 9.542 7-1.274 4.057-5.064 7-9.542 7-4.477 0-8.268-2.943-9.542-7z" />
              </svg>
            {/if}
          </button>
        </div>
        <button
          class="btn btn-ghost btn-xs"
          onclick={() => copyToClipboard('password', entry.password)}
        >
          {copiedField === 'password' ? '✓ Copied' : 'Copy'}
        </button>
        <button
          class="btn btn-accent-ghost btn-xs"
          onclick={handleGeneratePassword}
          title="Generate random password"
        >
          Generate
        </button>
      </div>
    </div>

    <!-- Expiration -->
    <div>
      <label class="text-xs font-medium uppercase tracking-wider mb-1.5 block" style="color: var(--text-muted);">Expiration Date</label>
      <input
        type="datetime-local"
        value={entry.expiresAt ? entry.expiresAt.replace(' ', 'T').slice(0, 16) : ''}
        class="w-auto"
        onblur={(e) => {
          const val = e.target.value ? e.target.value.replace('T', ' ') + ':00' : null;
          saveField('expiresAt', val);
        }}
      />
    </div>

    <!-- Notes -->
    <div>
      <label class="text-xs font-medium uppercase tracking-wider mb-1.5 block" style="color: var(--text-muted);">Notes</label>
      <textarea
        value={entry.notes}
        placeholder="Additional notes..."
        rows="4"
        class="w-full resize-y"
        style="min-height: 100px;"
        onblur={(e) => handleBlur('notes', e)}
      ></textarea>
    </div>

    <!-- Attachments -->
    <div>
      <div class="flex items-center justify-between mb-2">
        <label class="text-xs font-medium uppercase tracking-wider" style="color: var(--text-muted);">Attachments</label>
        <button
          class="btn btn-ghost btn-xs"
          onclick={handleAttachmentUpload}
        >
          + Add File
        </button>
      </div>

      {#if attachments.length > 0}
        <div class="space-y-1.5">
          {#each attachments as att (att.id)}
            <div class="flex items-center gap-3 px-3 py-2.5 rounded-lg transition-colors"
                 style="background: var(--surface); border: 1px solid var(--border);">
              <svg class="w-4 h-4 shrink-0" style="color: var(--text-muted);" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                      d="M15.172 7l-6.586 6.586a2 2 0 102.828 2.828l6.414-6.586a4 4 0 00-5.656-5.656l-6.415 6.585a6 6 0 108.486 8.486L20.5 13" />
              </svg>
              <span class="text-[13px] flex-1 truncate" style="color: var(--text-dim);">{att.name}</span>
              <span class="text-xs" style="color: var(--text-muted);">{formatBytes(att.size)}</span>
              <button
                class="text-xs px-2 py-1 rounded-md hover:bg-white/5"
                style="color: var(--accent);"
                onclick={() => handleDownloadAttachment(att.id)}
              >↓</button>
              <button
                class="text-xs px-2 py-1 rounded-md hover:bg-white/5"
                style="color: var(--danger);"
                onclick={() => handleDeleteAttachment(att.id)}
              >✕</button>
            </div>
          {/each}
        </div>
      {:else}
        <p class="text-xs" style="color: var(--text-muted);">No attachments</p>
      {/if}
    </div>
  </div>
</div>
{/if}
