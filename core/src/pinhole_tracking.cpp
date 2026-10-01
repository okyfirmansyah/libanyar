// LibAnyar — Pinhole DOM tracking bootstrap (platform-neutral JS; injected
// once per window by Window::create_pinhole()).

#include <anyar/pinhole.h>

#include <string>

namespace anyar {

// ── tracking_js() ─────────────────────────────────────────────────────────────
// Self-contained JS bootstrap injected via webview_init once per window.
// Scans for [data-anyar-pinhole] elements, tracks their CSS-pixel rects via
// ResizeObserver, hides during scroll, shows on idle after scrollend.
// Sends IPC commands: pinhole:update_rect, pinhole:set_visible.
// Deliberately avoids ES module syntax — must run in plain webview JS context.

/* static */ std::string Pinhole::tracking_js() {
    return R"js(
(function () {
  if (typeof window.__anyar_pinhole_init__ !== 'undefined') return;
  window.__anyar_pinhole_init__ = true;

  var _tracked = {};   // id → {ro, io, scrollTimer}
  var _wlabel  = (window.__LIBANYAR_WINDOW_LABEL__ || 'main');

  function _ipc(cmd, args) {
    if (typeof window.__anyar_ipc__ !== 'function') return;
    args.window_label = _wlabel;
    window.__anyar_ipc__(JSON.stringify({
      id: 'ph_' + cmd + '_' + Date.now(),
      cmd: cmd,
      args: args
    })).catch(function(){});
  }

  function sendRect(id, el) {
    var r = el.getBoundingClientRect();
    _ipc('pinhole:update_rect', {
      id: id,
      x: Math.round(r.left),
      y: Math.round(r.top),
      width: Math.round(r.width),
      height: Math.round(r.height),
      dpr: window.devicePixelRatio || 1
    });
  }

  function sendVisible(id, visible) {
    _ipc('pinhole:set_visible', { id: id, visible: visible });
  }

  // Detach a tracked pinhole: clean up observers and signal C++.
  function _detachTracked(id) {
    if (!_tracked[id]) return;
    _ipc('pinhole:dom_detached', { id: id });
    _tracked[id].ro.disconnect();
    _tracked[id].io.disconnect();
    window.removeEventListener('scroll', _tracked[id].onScroll, { capture: true });
    delete _tracked[id];
  }

  // Best-effort: if a pinhole is covered by a higher-z sibling, hide it.
  // Only signals hide; the IntersectionObserver / scroll protocol re-shows.
  function checkZSiblings() {
    var ids = Object.keys(_tracked);
    if (ids.length < 2) return;
    var rects = {}, zidx = {};
    ids.forEach(function(id) {
      var el = document.querySelector('[data-anyar-pinhole="' + id + '"]');
      if (!el) return;
      rects[id] = el.getBoundingClientRect();
      zidx[id]  = parseInt(window.getComputedStyle(el).zIndex) || 0;
    });
    ids.forEach(function(id_a) {
      if (!rects[id_a]) return;
      var covered = ids.some(function(id_b) {
        if (id_a === id_b || !rects[id_b] || zidx[id_b] <= zidx[id_a]) return false;
        var a = rects[id_a], b = rects[id_b];
        return !(a.right <= b.left || b.right <= a.left ||
                 a.bottom <= b.top || b.bottom <= a.top);
      });
      if (covered) sendVisible(id_a, false);
    });
  }

  function setupElement(id, el) {
    if (_tracked[id]) return;

    var scrollTimer = null;
    var isVisible   = true;

    // ResizeObserver: position + size changes (including scroll reflow)
    var ro = new ResizeObserver(function () {
      if (isVisible) sendRect(id, el);
    });
    ro.observe(el);

    // IntersectionObserver: handles display:none, scroll out-of-view, off-screen
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        isVisible = e.isIntersecting;
        sendVisible(id, isVisible);
        if (isVisible) sendRect(id, el);
      });
    }, { threshold: 0 });
    io.observe(el);

    // Scroll-hide on any ancestor scroll
    function onScroll() {
      if (isVisible) sendVisible(id, false);
      clearTimeout(scrollTimer);
      scrollTimer = setTimeout(function () {
        requestAnimationFrame(function () {
          sendRect(id, el);
          sendVisible(id, true);
          isVisible = true;
        });
      }, 100);
    }
    window.addEventListener('scroll', onScroll, { passive: true, capture: true });

    // Initial rect after first layout
    requestAnimationFrame(function () { sendRect(id, el); });

    _tracked[id] = { ro: ro, io: io, onScroll: onScroll };

    // Check if a higher-z sibling already covers this newly registered element
    checkZSiblings();
  }

  function scanDOM() {
    var els = document.querySelectorAll('[data-anyar-pinhole]');
    for (var i = 0; i < els.length; i++) {
      var id = els[i].getAttribute('data-anyar-pinhole');
      if (id) setupElement(id, els[i]);
    }
  }

  // MutationObserver: track DOM additions and removals
  var mo = new MutationObserver(function (mutations) {
    mutations.forEach(function (m) {
      // Removed nodes → signal dom_detached, clean up observers
      m.removedNodes.forEach(function (n) {
        if (n.nodeType !== 1) return;
        var rid = n.getAttribute && n.getAttribute('data-anyar-pinhole');
        if (rid) _detachTracked(rid);
        var rnest = n.querySelectorAll && n.querySelectorAll('[data-anyar-pinhole]');
        if (rnest) for (var i = 0; i < rnest.length; i++) {
          var rnid = rnest[i].getAttribute('data-anyar-pinhole');
          if (rnid) _detachTracked(rnid);
        }
      });
      // Added nodes → set up tracking
      m.addedNodes.forEach(function (n) {
        if (n.nodeType !== 1) return;
        var id = n.getAttribute && n.getAttribute('data-anyar-pinhole');
        if (id) setupElement(id, n);
        var nested = n.querySelectorAll && n.querySelectorAll('[data-anyar-pinhole]');
        if (nested) {
          for (var i = 0; i < nested.length; i++) {
            var nid = nested[i].getAttribute('data-anyar-pinhole');
            if (nid) setupElement(nid, nested[i]);
          }
        }
      });
    });
  });

  function boot() {
    scanDOM();
    mo.observe(document.documentElement, { childList: true, subtree: true });
    // Re-scan on navigation (SPA hash/history changes)
    window.addEventListener('popstate', scanDOM);
    window.addEventListener('hashchange', scanDOM);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})();
)js";
}

} // namespace anyar
