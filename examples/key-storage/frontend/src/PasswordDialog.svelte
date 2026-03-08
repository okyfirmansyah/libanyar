<script>
  /**
   * PasswordDialog — Modal overlay for entering the master password.
   * Modes: 'new' (create DB), 'unlock' (open DB), 'change' (change password)
   */

  let {
    mode = 'unlock',      // 'new' | 'unlock' | 'change'
    filename = '',
    onsubmit = null,
    oncancel = null,
  } = $props();

  let password = $state('');
  let confirm = $state('');
  let currentPw = $state('');
  let showPw = $state(false);
  let error = $state('');
  let busy = $state(false);

  const title = $derived(
    mode === 'new'    ? 'Create New Database' :
    mode === 'change' ? 'Change Master Password' :
                        'Unlock Database'
  );

  function handleSubmit() {
    error = '';

    if (mode === 'new' || mode === 'change') {
      if (password.length < 4) {
        error = 'Password must be at least 4 characters';
        return;
      }
      if (password !== confirm) {
        error = 'Passwords do not match';
        return;
      }
    }

    if (mode === 'change' && !currentPw) {
      error = 'Current password is required';
      return;
    }

    if (mode === 'unlock' && !password) {
      error = 'Enter the master password';
      return;
    }

    busy = true;

    const payload = mode === 'change'
      ? { currentPassword: currentPw, newPassword: password }
      : { password };

    try {
      onsubmit && onsubmit(payload);
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
  style="background: rgba(0,0,0,0.6); backdrop-filter: blur(4px);"
  onkeydown={handleKeydown}
>
  <!-- Card -->
  <div
    class="w-full max-w-sm rounded-xl shadow-2xl p-6"
    style="background: var(--surface); border: 1px solid var(--border);"
    onclick|stopPropagation
  >
    <!-- Lock icon -->
    <div class="flex flex-col items-center mb-5">
      <div class="w-14 h-14 rounded-full flex items-center justify-center mb-3"
           style="background: var(--accent-dim);">
        <svg class="w-7 h-7" style="color: var(--accent);" fill="none" stroke="currentColor" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" stroke-width="2"
                d="M12 15v2m-6 4h12a2 2 0 002-2v-6a2 2 0 00-2-2H6a2 2 0 00-2 2v6a2 2 0 002 2zm10-10V7a4 4 0 00-8 0v4h8z" />
        </svg>
      </div>
      <h2 class="text-base font-semibold" style="color: var(--text);">{title}</h2>
      {#if filename}
        <p class="text-[11px] mt-0.5 truncate max-w-[280px]" style="color: var(--text-muted);">{filename}</p>
      {/if}
    </div>

    <!-- Form -->
    <div class="space-y-3">
      {#if mode === 'change'}
        <div>
          <label class="text-[11px] font-medium uppercase tracking-wider mb-1 block" style="color: var(--text-muted);">Current Password</label>
          <input
            type="password"
            bind:value={currentPw}
            placeholder="Enter current password"
            class="w-full"
            autofocus
          />
        </div>
      {/if}

      <div>
        <label class="text-[11px] font-medium uppercase tracking-wider mb-1 block" style="color: var(--text-muted);">
          {mode === 'change' ? 'New Password' : 'Master Password'}
        </label>
        <div class="relative">
          <input
            type={showPw ? 'text' : 'password'}
            bind:value={password}
            placeholder={mode === 'unlock' ? 'Enter master password' : 'Choose a strong password'}
            class="w-full pr-9"
            autofocus={mode !== 'change'}
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

      {#if mode === 'new' || mode === 'change'}
        <div>
          <label class="text-[11px] font-medium uppercase tracking-wider mb-1 block" style="color: var(--text-muted);">Confirm Password</label>
          <input
            type={showPw ? 'text' : 'password'}
            bind:value={confirm}
            placeholder="Repeat password"
            class="w-full"
          />
        </div>
      {/if}

      {#if error}
        <p class="text-xs font-medium px-1" style="color: var(--danger);">{error}</p>
      {/if}
    </div>

    <!-- Buttons -->
    <div class="flex gap-2 mt-5">
      <button
        class="flex-1 py-2 rounded-lg text-sm font-medium transition-colors"
        style="background: var(--surface-2); color: var(--text-dim);"
        onclick={oncancel}
      >
        Cancel
      </button>
      <button
        class="flex-1 py-2 rounded-lg text-sm font-medium transition-colors"
        style="background: var(--accent); color: white;"
        onclick={handleSubmit}
        disabled={busy}
      >
        {#if busy}
          Unlocking…
        {:else if mode === 'new'}
          Create
        {:else if mode === 'change'}
          Change
        {:else}
          Unlock
        {/if}
      </button>
    </div>
  </div>
</div>
