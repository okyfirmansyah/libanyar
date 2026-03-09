<script>
  /**
   * UnlockDialog — Full-screen overlay for entering the master password.
   * Modes: 'new' (create vault), 'open' (decrypt existing), 'reopen' (re-enter after lock)
   */

  let {
    mode = 'open',
    path = '',
    onunlock = null,
    oncancel = null,
  } = $props();

  let password = $state('');
  let confirm = $state('');
  let showPw = $state(false);
  let error = $state('');
  let busy = $state(false);

  const needsConfirm = $derived(mode === 'new');

  const titleText = $derived(
    mode === 'new'    ? 'Create New Vault' :
    mode === 'reopen' ? 'Vault Locked' :
                        'Unlock Vault'
  );

  const subtitleText = $derived(
    mode === 'new' ? 'Choose a master password to protect your vault' :
                     'Enter the master password to decrypt'
  );

  const buttonLabel = $derived(
    mode === 'new' ? 'Create' : 'Unlock'
  );

  function fileName(p) {
    return p ? p.split('/').pop() : '';
  }

  async function handleSubmit() {
    error = '';

    if (!password) {
      error = 'Password is required';
      return;
    }

    if (needsConfirm) {
      if (password.length < 4) {
        error = 'Password must be at least 4 characters';
        return;
      }
      if (password !== confirm) {
        error = 'Passwords do not match';
        return;
      }
    }

    busy = true;
    try {
      await onunlock?.(password);
    } catch (e) {
      error = e?.message || 'Failed to unlock';
    } finally {
      busy = false;
    }
  }

  function handleKeydown(e) {
    if (e.key === 'Enter') handleSubmit();
    if (e.key === 'Escape') oncancel?.();
  }
</script>

<!-- svelte-ignore a11y_no_static_element_interactions -->
<div
  class="fixed inset-0 z-50 flex items-center justify-center"
  style="background: var(--bg);"
  onkeydown={handleKeydown}
>
  <div class="w-full max-w-sm px-6">
    <!-- Lock icon -->
    <div class="flex flex-col items-center mb-6">
      <div class="w-12 h-12 rounded-xl flex items-center justify-center mb-4"
           style="background: var(--surface-2); border: 1px solid var(--border);">
        <svg class="w-6 h-6" style="color: var(--text-muted);" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          {#if mode === 'new'}
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5"
                  d="M12 4v16m8-8H4" />
          {:else}
            <path stroke-linecap="round" stroke-linejoin="round" stroke-width="1.5"
                  d="M12 15v2m-6 4h12a2 2 0 002-2v-6a2 2 0 00-2-2H6a2 2 0 00-2 2v6a2 2 0 002 2zm10-10V7a4 4 0 00-8 0v4h8z" />
          {/if}
        </svg>
      </div>
      <h2 class="text-base font-semibold" style="color: var(--text);">{titleText}</h2>
      <p class="text-xs mt-1" style="color: var(--text-muted);">{subtitleText}</p>
      {#if path}
        <p class="text-[11px] mt-2 px-3 py-1 rounded-md truncate max-w-[300px]"
           style="background: var(--surface); color: var(--text-dim); border: 1px solid var(--border);">
          {fileName(path)}
        </p>
      {/if}
    </div>

    <!-- Form -->
    <div class="space-y-3">
      <div>
        <label class="text-[11px] font-medium uppercase tracking-wider mb-1 block" style="color: var(--text-muted);">
          Master Password
        </label>
        <div class="relative">
          <!-- svelte-ignore a11y_autofocus -->
          <input
            type={showPw ? 'text' : 'password'}
            bind:value={password}
            placeholder={needsConfirm ? 'Choose a strong password' : 'Enter master password'}
            class="w-full pr-9"
            autofocus
            disabled={busy}
          />
          <button
            class="absolute right-2 top-1/2 -translate-y-1/2 w-5 h-5 flex items-center justify-center"
            style="color: var(--text-muted);"
            onclick={() => showPw = !showPw}
            type="button"
          >
            {#if showPw}
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
      </div>

      {#if needsConfirm}
        <div>
          <label class="text-[11px] font-medium uppercase tracking-wider mb-1 block" style="color: var(--text-muted);">
            Confirm Password
          </label>
          <input
            type={showPw ? 'text' : 'password'}
            bind:value={confirm}
            placeholder="Repeat password"
            class="w-full"
            disabled={busy}
          />
        </div>
      {/if}

      {#if error}
        <p class="text-xs font-medium" style="color: var(--danger);">{error}</p>
      {/if}
    </div>

    <!-- Buttons -->
    <div class="flex gap-3 mt-6">
      <button
        class="btn btn-secondary flex-1"
        onclick={oncancel}
        disabled={busy}
      >
        Cancel
      </button>
      <button
        class="btn btn-primary flex-1"
        onclick={handleSubmit}
        disabled={busy}
      >
        {busy ? 'Working…' : buttonLabel}
      </button>
    </div>
  </div>
</div>
