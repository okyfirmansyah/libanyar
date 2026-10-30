#!/usr/bin/env python3
"""Generate the README architecture hero: docs/assets/architecture-{light,dark}.svg.

    python3 docs/assets/src/architecture.py

Hand-laid SVG (no Mermaid): one layout, two colour themes. GitHub serves README
images through a sanitizer, so the SVG uses only shapes, gradients and the
system font stack: no external fonts, scripts or <foreignObject>.
"""
from pathlib import Path

W, H = 960, 612
FONT = "-apple-system, BlinkMacSystemFont, 'Segoe UI', 'Noto Sans', Helvetica, Arial, sans-serif"
MONO = "ui-monospace, SFMono-Regular, 'SF Mono', Menlo, Consolas, 'Liberation Mono', monospace"

THEMES = {
    "light": dict(
        text="#0f172a", muted="#475569", faint="#94a3b8",
        panel="#ffffff", panel_border="#e2e8f0", band="#f8fafc",
        chip="#f1f5f9", chip_text="#0f172a", chip_border="#e2e8f0",
        core_a="#0f766e", core_b="#0d9488", core_chip="rgba(255,255,255,0.16)",
        base="#f1f5f9", base_border="#e2e8f0", base_text="#334155",
        arrow="#94a3b8", shadow="0.10",
        metrics=("#4f46e5", "#0284c7", "#d97706"),
    ),
    "dark": dict(
        text="#e6edf3", muted="#9da7b3", faint="#6e7681",
        panel="#161b22", panel_border="#30363d", band="#0d1117",
        chip="#21262d", chip_text="#e6edf3", chip_border="#30363d",
        core_a="#115e59", core_b="#0f766e", core_chip="rgba(255,255,255,0.12)",
        base="#161b22", base_border="#30363d", base_text="#c9d1d9",
        arrow="#6e7681", shadow="0.45",
        metrics=("#a5b4fc", "#38bdf8", "#fbbf24"),
    ),
}

# Channel accents: same in both themes.
CHANNELS = [
    dict(title="Native IPC", metric="~0.01 ms", sub="invoke() → C++ command", sub2="in-process, no HTTP", a="#6366f1", b="#8b5cf6", icon="bolt"),
    dict(title="Events", metric="any window", sub="emit · listen", sub2="push to one window or all", a="#0ea5e9", b="#6366f1", icon="broadcast"),
    dict(title="Binary & video", metric="zero-copy", sub="SharedBuffer · Pinhole", sub2="no JSON, no base64", a="#f59e0b", b="#f97316", icon="layers"),
]
CORE_CHIPS = ["Commands", "Plugins", "Event bus", "Multi-window",
              "SharedBuffer", "Pinhole", "run_blocking", "Debug tools"]
WEBVIEWS = [("Linux", "WebKitGTK", True), ("Windows", "WebView2", True), ("macOS", "WKWebView · soon", False)]


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, s, size, fill, weight=400, anchor="start", family=FONT, extra=""):
    return (f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" '
            f'font-weight="{weight}" fill="{fill}" text-anchor="{anchor}"{extra}>{esc(s)}</text>')


def icon(kind, cx, cy):
    """24px white glyph centred on (cx, cy)."""
    if kind == "bolt":
        return (f'<path transform="translate({cx-12},{cy-12})" d="M13 2 4 14h7l-1 8 9-12h-7l1-8z" '
                f'fill="#fff"/>')
    if kind == "broadcast":
        return (f'<g transform="translate({cx-12},{cy-12})" fill="none" stroke="#fff" stroke-width="2" stroke-linecap="round">'
                f'<circle cx="12" cy="12" r="2.2" fill="#fff" stroke="none"/>'
                f'<path d="M8 8a5.6 5.6 0 0 0 0 8M16 8a5.6 5.6 0 0 1 0 8"/>'
                f'<path d="M5 5a9.8 9.8 0 0 0 0 14M19 5a9.8 9.8 0 0 1 0 14"/></g>')
    if kind == "layers":
        return (f'<g transform="translate({cx-12},{cy-12})" fill="none" stroke="#fff" stroke-width="2" stroke-linejoin="round">'
                f'<path d="M12 3 2 8l10 5 10-5-10-5z" fill="#fff"/>'
                f'<path d="M2 12.5l10 5 10-5"/><path d="M2 17l10 5 10-5"/></g>')
    raise ValueError(kind)


def chip_width(label, size=13):
    return int(len(label) * size * 0.58) + 26


def build(t):
    o = []
    o.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
             f'role="img" aria-labelledby="title desc">')
    o.append('<title id="title">LibAnyar architecture</title>')
    o.append('<desc id="desc">A web frontend runs in the OS webview and talks to a C++17 core over three '
             'channels: native IPC, events, and zero-copy binary data. The core runs on LibAsyik.</desc>')
    o.append('<defs>')
    o.append(f'<linearGradient id="fe" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#4f46e5"/>'
             f'<stop offset="1" stop-color="#7c3aed"/></linearGradient>')
    o.append(f'<linearGradient id="core" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{t["core_a"]}"/>'
             f'<stop offset="1" stop-color="{t["core_b"]}"/></linearGradient>')
    for i, c in enumerate(CHANNELS):
        o.append(f'<linearGradient id="ch{i}" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{c["a"]}"/>'
                 f'<stop offset="1" stop-color="{c["b"]}"/></linearGradient>')
    o.append(f'<filter id="shadow" x="-10%" y="-10%" width="120%" height="140%">'
             f'<feDropShadow dx="0" dy="4" stdDeviation="8" flood-color="#0f172a" flood-opacity="{t["shadow"]}"/></filter>')
    o.append(f'<marker id="arr" viewBox="0 0 10 10" refX="5" refY="5" markerWidth="5" markerHeight="5" orient="auto-start-reverse">'
             f'<path d="M0 0 10 5 0 10z" fill="{t["arrow"]}"/></marker>')
    o.append('</defs>')

    X, CW = 24, W - 48

    # ── 1. Frontend ───────────────────────────────────────────────────────────
    y = 24
    o.append(f'<rect x="{X}" y="{y}" width="{CW}" height="92" rx="18" fill="url(#fe)" filter="url(#shadow)"/>')
    # window glyph
    o.append(f'<g transform="translate({X+28},{y+26})"><rect width="40" height="40" rx="10" fill="rgba(255,255,255,0.18)"/>'
             f'<rect x="9" y="11" width="22" height="18" rx="3" fill="none" stroke="#fff" stroke-width="2"/>'
             f'<path d="M9 16h22" stroke="#fff" stroke-width="2"/></g>')
    o.append(text(X + 84, y + 42, "Your web frontend", 22, "#fff", 700))
    o.append(text(X + 84, y + 66, "Svelte · React · Vue · vanilla — any Vite app, hot reload in dev", 14, "rgba(255,255,255,0.85)"))
    api = "@libanyar/api"
    aw = chip_width(api, 14) + 6
    o.append(f'<rect x="{X+CW-28-aw}" y="{y+29}" width="{aw}" height="34" rx="17" fill="rgba(255,255,255,0.18)" '
             f'stroke="rgba(255,255,255,0.35)"/>')
    o.append(text(X + CW - 28 - aw / 2, y + 51, api, 14, "#fff", 600, "middle", MONO))

    # ── 2. OS webview band ────────────────────────────────────────────────────
    y = 132
    o.append(f'<rect x="{X}" y="{y}" width="{CW}" height="52" rx="14" fill="{t["band"]}" stroke="{t["panel_border"]}"/>')
    o.append(text(X + 24, y + 31, "OS webview", 15, t["text"], 700))
    o.append(text(X + 118, y + 31, "native, not bundled", 13, t["muted"]))
    bx = X + CW - 20
    for os_name, engine, ok in reversed(WEBVIEWS):
        label = f"{os_name} · {engine}"
        bw = int(len(label) * 7.3) + 40
        bx -= bw
        dash = "" if ok else ' stroke-dasharray="4 3"'
        o.append(f'<rect x="{bx}" y="{y+12}" width="{bw}" height="28" rx="14" fill="{t["chip"]}" stroke="{t["chip_border"]}"{dash}/>')
        dot = "#22c55e" if ok else t["faint"]
        o.append(f'<circle cx="{bx+16}" cy="{y+26}" r="4" fill="{dot}"/>')
        o.append(f'<text x="{bx+27}" y="{y+31}" font-family="{FONT}" font-size="13">'
                 f'<tspan font-weight="600" fill="{t["chip_text"] if ok else t["muted"]}">{esc(os_name)}</tspan>'
                 f'<tspan fill="{t["muted"]}"> · {esc(engine)}</tspan></text>')
        bx -= 10

    # ── 3. Channels ───────────────────────────────────────────────────────────
    y = 214
    gap = 24
    cw = (CW - 2 * gap) / 3
    for i, c in enumerate(CHANNELS):
        cx = X + i * (cw + gap)
        mid = cx + cw / 2
        # connectors: webview band ↔ channel ↔ core
        o.append(f'<line x1="{mid}" y1="188" x2="{mid}" y2="{y-4}" stroke="{t["arrow"]}" stroke-width="2" '
                 f'marker-start="url(#arr)" marker-end="url(#arr)"/>')
        o.append(f'<line x1="{mid}" y1="{y+136}" x2="{mid}" y2="{y+164}" stroke="{t["arrow"]}" stroke-width="2" '
                 f'marker-start="url(#arr)" marker-end="url(#arr)"/>')
        o.append(f'<rect x="{cx}" y="{y}" width="{cw}" height="132" rx="16" fill="{t["panel"]}" '
                 f'stroke="{t["panel_border"]}" filter="url(#shadow)"/>')
        o.append(f'<clipPath id="clip{i}"><rect x="{cx}" y="{y}" width="{cw}" height="132" rx="16"/></clipPath>')
        o.append(f'<rect x="{cx}" y="{y}" width="{cw}" height="5" fill="url(#ch{i})" clip-path="url(#clip{i})"/>')
        o.append(f'<rect x="{cx+20}" y="{y+24}" width="44" height="44" rx="12" fill="url(#ch{i})"/>')
        o.append(icon(c["icon"], cx + 42, y + 46))
        o.append(text(cx + 78, y + 42, c["title"], 16, t["text"], 700))
        o.append(text(cx + 78, y + 64, c["metric"], 20, t["metrics"][i], 800))
        o.append(text(cx + 20, y + 98, c["sub"], 13, t["text"], 500))
        o.append(text(cx + 20, y + 117, c["sub2"], 13, t["muted"]))

    # ── 4. Core ───────────────────────────────────────────────────────────────
    y = 382
    o.append(f'<rect x="{X}" y="{y}" width="{CW}" height="134" rx="18" fill="url(#core)" filter="url(#shadow)"/>')
    o.append(f'<g transform="translate({X+28},{y+26})"><rect width="40" height="40" rx="10" fill="rgba(255,255,255,0.16)"/>'
             f'<path d="M14 13 8 20l6 7M26 13l6 7-6 7" fill="none" stroke="#fff" stroke-width="2.4" '
             f'stroke-linecap="round" stroke-linejoin="round"/></g>')
    o.append(text(X + 84, y + 44, "LibAnyar core", 22, "#fff", 700))
    o.append(text(X + 84 + 152, y + 44, "C++17", 16, "rgba(255,255,255,0.75)", 600))
    o.append(text(X + 84, y + 66, "one service thread of fibers + a worker pool for blocking work", 14, "rgba(255,255,255,0.85)"))
    # one row of chips, spread evenly across the card
    row_y = y + 86
    widths = [len(lbl) * 7.7 + 22 for lbl in CORE_CHIPS]
    spare = (CW - 56) - sum(widths)
    gapc = spare / (len(CORE_CHIPS) - 1)
    cx = X + 28
    for lbl, w in zip(CORE_CHIPS, widths):
        o.append(f'<rect x="{cx:.1f}" y="{row_y}" width="{w:.1f}" height="30" rx="15" fill="{t["core_chip"]}" '
                 f'stroke="rgba(255,255,255,0.22)"/>')
        o.append(text(round(cx + w / 2, 1), row_y + 20, lbl, 12.5, "#fff", 600, "middle"))
        cx += w + gapc

    # ── 5. Foundation ─────────────────────────────────────────────────────────
    y = 532
    o.append(f'<rect x="{X}" y="{y}" width="{CW}" height="56" rx="14" fill="{t["base"]}" stroke="{t["base_border"]}"/>')
    o.append(text(X + 24, y + 34, "LibAsyik", 15, t["text"], 700))
    o.append(text(X + 100, y + 34, "foundation", 13, t["muted"]))
    items = ["Boost fibers", "HTTP · WebSocket", "SQL via SOCI", "MIT licensed"]
    ix = X + CW - 20
    for lbl in reversed(items):
        w = int(len(lbl) * 7.3) + 28
        ix -= w
        o.append(f'<rect x="{ix}" y="{y+14}" width="{w}" height="28" rx="14" fill="{t["chip"]}" stroke="{t["chip_border"]}"/>')
        o.append(text(ix + w / 2, y + 33, lbl, 13, t["base_text"], 500, "middle"))
        ix -= 10

    o.append('</svg>')
    return "\n".join(o) + "\n"


if __name__ == "__main__":
    out = Path(__file__).resolve().parent.parent
    for name, theme in THEMES.items():
        (out / f"architecture-{name}.svg").write_text(build(theme))
        print(out / f"architecture-{name}.svg")
