import './app.css';
import App from './App.svelte';
import EntryDetailPage from './EntryDetailPage.svelte';
import { mount } from 'svelte';

// ── Simple hash-based router ────────────────────────────────────────────────
// Main window:        no hash or #/
// Entry detail:       #/entry/:id   (opened as native modal child window)
// ─────────────────────────────────────────────────────────────────────────────

function getRoute() {
  const hash = window.location.hash || '';
  const entryMatch = hash.match(/^#\/entry\/(\d+)$/);
  if (entryMatch) {
    return { page: 'entry-detail', entryId: parseInt(entryMatch[1], 10) };
  }
  return { page: 'main' };
}

const route = getRoute();

let app;
if (route.page === 'entry-detail') {
  app = mount(EntryDetailPage, {
    target: document.getElementById('app'),
    props: { entryId: route.entryId },
  });
} else {
  app = mount(App, {
    target: document.getElementById('app'),
  });
}

export default app;
