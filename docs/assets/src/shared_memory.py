#!/usr/bin/env python3
"""Generate the README shared-memory diagram: docs/assets/shared-memory-{light,dark}.svg.

    python3 docs/assets/src/shared_memory.py

Same look and rules as architecture.py (whose themes and helpers it reuses).
The structure lives in STAGES/STEPS below; the drawing code only lays it out.
"""
from pathlib import Path

from architecture import FONT, MONO, THEMES, esc, text

W, H = 960, 276

STAGES = [
    dict(id="producer", title="C++ producer", sub="decoder · camera", code="acquire_write()",
         note="writes pixels in place", a="#0f766e", b="#0d9488", icon="cpu"),
    dict(id="pool", title="SharedBufferPool", sub="ring of N slots · lock-free", a="#f59e0b", b="#f97316"),
    dict(id="canvas", title="WebGL canvas", sub="RGBA · YUV · NV12", code="createBufferRenderer()",
         note="no JSON, no base64", a="#4f46e5", b="#7c3aed", icon="screen"),
]
# slot label, state, fill (None = theme chip colour)
SLOTS = [("on screen", "READY", "#6366f1"), ("filling", "WRITING", "#f59e0b"),
         ("", "FREE", None), ("", "FREE", None)]
PLATFORMS = [("Linux", "anyar-shm://"), ("Windows", "WebView2")]
STEPS = [  # (number, label, from, to)
    (1, "write pixels in place", "producer", "pool"),
    (2, "emit buffer:ready", "pool", "canvas"),
    (3, "release → FREE", "canvas", "pool"),
]

DESC = ("Zero-copy frame streaming. A C++ producer (decoder, camera, sensor) calls acquire_write() on a "
        "SharedBufferPool, a lock-free ring of N shared-memory slots whose states are FREE, WRITING and READY, "
        "and writes pixels in place (step 1). It then emits buffer:ready (step 2); the page's WebGL canvas, made "
        "with createBufferRenderer(), reads the slot (Linux: anyar-shm:// URI, Windows: WebView2 shared buffer) "
        "and draws RGBA, YUV420 or NV12 without JSON or base64. When drawn, the page releases the slot back to "
        "FREE (step 3).")


def glyph(kind, x, y):
    """24px white glyph with its top-left at (x, y)."""
    g = f'<g transform="translate({x},{y})" fill="none" stroke="#fff" stroke-width="2" stroke-linejoin="round" stroke-linecap="round">'
    if kind == "cpu":
        return (g + '<rect x="6" y="6" width="12" height="12" rx="2"/>'
                '<path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"/></g>')
    if kind == "screen":
        return g + '<rect x="2" y="4" width="20" height="13" rx="2"/><path d="M8 21h8M12 17v4"/></g>'
    if kind == "ring":
        return g + '<rect x="3" y="4" width="18" height="6" rx="2"/><rect x="3" y="14" width="18" height="6" rx="2"/></g>'
    raise ValueError(kind)


def badge(cx, cy, n, t):
    return (f'<circle cx="{cx}" cy="{cy}" r="11" fill="{t["text"]}"/>'
            + text(cx, cy + 4.5, str(n), 12.5, t["panel"], 800, "middle"))


def build(t):
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
         f'role="img" aria-labelledby="title desc">',
         '<title id="title">LibAnyar shared-memory streaming</title>',
         f'<desc id="desc">{esc(DESC)}</desc>', '<defs>']
    for s in STAGES:
        o.append(f'<linearGradient id="g-{s["id"]}" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{s["a"]}"/>'
                 f'<stop offset="1" stop-color="{s["b"]}"/></linearGradient>')
    o.append(f'<filter id="shadow" x="-10%" y="-10%" width="120%" height="140%">'
             f'<feDropShadow dx="0" dy="4" stdDeviation="8" flood-color="#0f172a" flood-opacity="{t["shadow"]}"/></filter>')
    o.append(f'<marker id="arr" viewBox="0 0 10 10" refX="5" refY="5" markerWidth="5" markerHeight="5" orient="auto-start-reverse">'
             f'<path d="M0 0 10 5 0 10z" fill="{t["arrow"]}"/></marker>')
    o.append('</defs>')

    Y, CH, GAP = 24, 176, 48
    side, mid_w = 224, 960 - 48 - 2 * 224 - 2 * GAP
    boxes = {"producer": (24, side), "pool": (24 + side + GAP, mid_w), "canvas": (24 + side + GAP + mid_w + GAP, side)}

    def card(s):
        x, w = boxes[s["id"]]
        links = " ".join(sorted({b for n, _, a, b in STEPS if a == s["id"]} | {a for n, _, a, b in STEPS if b == s["id"]}))
        o.append(f'<g id="{s["id"]}" data-links="{links}">')
        o.append(f'<rect x="{x}" y="{Y}" width="{w}" height="{CH}" rx="16" fill="{t["panel"]}" '
                 f'stroke="{t["panel_border"]}" filter="url(#shadow)"/>')
        o.append(f'<clipPath id="clip-{s["id"]}"><rect x="{x}" y="{Y}" width="{w}" height="{CH}" rx="16"/></clipPath>')
        o.append(f'<rect x="{x}" y="{Y}" width="{w}" height="5" fill="url(#g-{s["id"]})" clip-path="url(#clip-{s["id"]})"/>')
        o.append(f'<rect x="{x+20}" y="{Y+24}" width="44" height="44" rx="12" fill="url(#g-{s["id"]})"/>')
        o.append(glyph(s.get("icon", "ring"), x + 30, Y + 34))
        o.append(text(x + 78, Y + 42, s["title"], 16, t["text"], 700))
        o.append(text(x + 78, Y + 62, s["sub"], 12.5, t["muted"]))
        return x, w

    # side cards: code chip + note
    for s in (STAGES[0], STAGES[2]):
        x, w = card(s)
        cw = len(s["code"]) * 12 * 0.6 + 24  # monospace advance ≈ 0.6 em
        o.append(f'<rect x="{x+20}" y="{Y+94}" width="{cw:.0f}" height="30" rx="8" fill="{t["chip"]}" stroke="{t["chip_border"]}"/>')
        o.append(text(x + 32, Y + 113.5, s["code"], 12, t["chip_text"], 600, family=MONO))
        o.append(text(x + 20, Y + 150, s["note"], 13, t["muted"]))
        o.append('</g>')

    # pool card: slot ring + platform badges
    x, w = card(STAGES[1])
    sw, sg = 76, 8
    sx = x + (w - (len(SLOTS) * sw + (len(SLOTS) - 1) * sg)) / 2
    for i, (lbl, state, fill) in enumerate(SLOTS):
        bx = sx + i * (sw + sg)
        f = fill or t["chip"]
        stroke = "none" if fill else t["chip_border"]
        dash = "" if fill else ' stroke-dasharray="4 3"'
        o.append(f'<rect x="{bx:.1f}" y="{Y+86}" width="{sw}" height="42" rx="9" fill="{f}" stroke="{stroke}"{dash}/>')
        col = "#fff" if fill else t["muted"]
        o.append(text(round(bx + sw / 2, 1), Y + (104 if lbl else 111), state, 11.5, col, 700, "middle", MONO))
        if lbl:
            o.append(text(round(bx + sw / 2, 1), Y + 120, lbl, 11, "rgba(255,255,255,0.85)", 500, "middle"))
    px = x + 20
    for os_name, how in PLATFORMS:
        label_w = len(os_name + " · " + how) * 6.6 + 30
        o.append(f'<rect x="{px:.0f}" y="{Y+140}" width="{label_w:.0f}" height="24" rx="12" fill="{t["chip"]}" stroke="{t["chip_border"]}"/>')
        o.append(f'<circle cx="{px+13:.0f}" cy="{Y+152}" r="3.5" fill="#22c55e"/>')
        o.append(f'<text x="{px+22:.0f}" y="{Y+156.5}" font-family="{FONT}" font-size="11.5">'
                 f'<tspan font-weight="600" fill="{t["chip_text"]}">{esc(os_name)}</tspan>'
                 f'<tspan fill="{t["muted"]}"> · {esc(how)}</tspan></text>')
        px += label_w + 10
    o.append('</g>')

    # ── steps ────────────────────────────────────────────────────────────────
    ay = Y + CH / 2
    for n, lbl, a, b in STEPS[:2]:
        x1 = boxes[a][0] + boxes[a][1] + 4
        x2 = boxes[b][0] - 4
        o.append(f'<g id="step{n}" data-from="{a}" data-to="{b}">')
        o.append(f'<line x1="{x1}" y1="{ay}" x2="{x2}" y2="{ay}" stroke="{t["arrow"]}" stroke-width="2" marker-end="url(#arr)"/>')
        o.append(badge((x1 + x2) / 2, ay - 22, n, t))
        o.append('</g>')

    # release: canvas bottom → pool bottom, along a rail under the cards
    n, lbl, a, b = STEPS[2]
    cx_ = boxes["canvas"][0] + boxes["canvas"][1] / 2
    px_ = boxes["pool"][0] + boxes["pool"][1] * 0.75
    by, ry = Y + CH + 4, Y + CH + 46
    o.append(f'<g id="step{n}" data-from="{a}" data-to="{b}">')
    o.append(f'<path d="M{cx_} {by} V{ry-12} Q{cx_} {ry} {cx_-12} {ry} H{px_+12} Q{px_} {ry} {px_} {ry-12} V{by+6}" '
             f'fill="none" stroke="{t["arrow"]}" stroke-width="2" stroke-dasharray="6 5" marker-end="url(#arr)"/>')
    mx = (cx_ + px_) / 2
    lw = len(lbl) * 11.5 * 0.6 + 44
    o.append(f'<rect x="{mx-lw/2:.0f}" y="{ry-13}" width="{lw:.0f}" height="26" rx="13" fill="{t["band"]}" stroke="{t["panel_border"]}"/>')
    o.append(badge(mx - lw / 2 + 14, ry, n, t))
    o.append(text(mx - lw / 2 + 30, ry + 4.5, lbl, 11.5, t["muted"], 600, family=MONO))
    o.append('</g>')

    # legend for steps 1-2 (the gaps are too narrow for their labels)
    lx = 24
    for n, lbl, a, b in STEPS[:2]:
        lw2 = len(lbl) * 11.5 * 0.6 + 44
        o.append(f'<rect x="{lx:.0f}" y="{ry-13}" width="{lw2:.0f}" height="26" rx="13" fill="{t["band"]}" stroke="{t["panel_border"]}"/>')
        o.append(badge(lx + 14, ry, n, t))
        o.append(text(lx + 30, ry + 4.5, lbl, 11.5, t["muted"], 600, family=MONO))
        lx += lw2 + 10

    o.append('</svg>')
    return "\n".join(o) + "\n"


if __name__ == "__main__":
    out = Path(__file__).resolve().parent.parent
    for name, theme in THEMES.items():
        (out / f"shared-memory-{name}.svg").write_text(build(theme))
        print(out / f"shared-memory-{name}.svg")
