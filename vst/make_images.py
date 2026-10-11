#!/usr/bin/env python3
"""Draws the Plugin Manager skin's artwork into images/ (SVG). Run it after
changing the look; the layout (layout.conf) places these. Colours follow the mockup: near-black panels, grey type,
red for the main action, amber for updates and warnings, green for "tested"."""
import html
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "images")
BG, PANEL, LINE, INK, DIM, MUTED = "#0c0c0d", "#151517", "#26272a", "#ececed", "#9da1a7", "#b4b7bc"
RED, AMBER, AMBER_TX, GREEN = "#e0342c", "#f0a830", "#f0b44a", "#7fd6a8"
FONT = "font-family=\"Titillium Web\""


def svg(name, w, h, body):
    open(os.path.join(OUT, name), "w").write(
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">%s</svg>\n' % (w, h, w, h, body))


def text(x, y, s, size, fill, weight=600, anchor="start", spacing=0):
    return '<text x="%g" y="%g" %s font-size="%d" font-weight="%d" fill="%s" text-anchor="%s" letter-spacing="%g">%s</text>' % (
        x, y, FONT, size, weight, fill, anchor, spacing, html.escape(s))


ICON = {   # 24x24 stroke icons, drawn at (x, y) scaled to s
    "down": '<path d="M12 4v11"/><path d="M7 10.5l5 5 5-5"/><path d="M4 20h16"/>',
    "refresh": '<path d="M20 12a8 8 0 1 1-2.3-5.6"/><path d="M20 4v5h-5"/>',
    "check": '<path d="M4 12.5l5 5L20 6.5"/>',
    "clock": '<circle cx="12" cy="12" r="8.5"/><path d="M12 7.5V12l3 2"/>',
    "trash": '<path d="M4 7h16"/><path d="M9 7V4h6v3"/><path d="M6 7l1 13h10l1-13"/>',
    "warn": '<path d="M12 3L2 21h20L12 3z"/><path d="M12 10v5"/><path d="M12 18v.5"/>',
    "shield": '<path d="M12 3l8 3v6c0 4.5-3.4 8-8 9-4.6-1-8-4.5-8-9V6l8-3z"/>',
    "left": '<path d="M15 5l-7 7 7 7"/>',
    "right": '<path d="M9 5l7 7-7 7"/>',
    "nowifi": '<path d="M2 8.5a15 15 0 0 1 20 0"/><path d="M5.5 12a10 10 0 0 1 13 0"/><path d="M9 15.5a5 5 0 0 1 6 0"/><path d="M12 19h.01"/><path d="M3 3l18 18"/>',
    "lock": '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>',
}


def icon(kind, x, y, s, colour, width=2.5):
    return '<g transform="translate(%g %g) scale(%g)" fill="none" stroke="%s" stroke-width="%g" stroke-linecap="round" stroke-linejoin="round">%s</g>' % (
        x, y, s / 24.0, colour, width, ICON[kind])


def button(name, w, h, label, fill, tx, ic=None, stroke=None, dash=False, size=19):
    edge = ""
    if stroke:
        edge = ' stroke="%s" stroke-width="2"%s' % (stroke, ' stroke-dasharray="7 5"' if dash else "")
    body = '<rect x="1" y="1" width="%d" height="%d" rx="8" fill="%s"%s/>' % (w - 2, h - 2, fill, edge)
    tw = len(label) * size * 0.62 + (30 if ic else 0)
    x0 = (w - tw) / 2
    if ic:
        body += icon(ic, x0, h / 2 - 11, 22, tx)
        x0 += 30
    body += text(x0, h / 2 + size * 0.36, label, size, tx, 700, spacing=1.2)
    svg(name, w, h, body)


def text_w(s, size):   # Titillium Web SemiBold, close enough to size a label to its text
    return sum(0.3 if ch in "il.:,'" else 0.62 if ch.isupper() else 0.5 for ch in s) * size


BADGES = [("Stable", "#d6d8db", "#3a3c41", None), ("Beta", AMBER_TX, "#6b5220", None),
          ("High CPU", AMBER_TX, "#6b5220", "warn"), ("Tested", GREEN, "#2f5e46", "check"), ("Old install", "#c9ccd1", "#4a4c52", None)]


def badge_row(labels, y, h):
    """labels side by side from the left, each only as wide as its text (and icon)"""
    out, x = "", 0.5
    for label, colour, edge, ic in labels:
        w = text_w(label, 16) + 16 + (19 if ic else 0)
        out += '<rect x="%g" y="%g" width="%g" height="%d" rx="4" fill="none" stroke="%s"/>' % (x, y + 0.5, w, h - 1, edge)
        tx = x + 8
        if ic:
            out += icon(ic, x + 6, y + 3, 16, colour, 2.5)
            tx += 19
        out += text(tx, y + h / 2 + 5.6, label, 16, colour, 600)
        x += w + 7
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    # the page: header (logo, title, storage label) and footer; everything else is live or switched
    grid = "".join('<rect x="%d" y="%d" width="11" height="11" rx="1.5" fill="%s" stroke="%s" stroke-width="2"/>' % (
        x, y, RED if (x, y) == (39, 32) else "none", RED if (x, y) == (39, 32) else INK) for x in (24, 39) for y in (17, 32))
    svg("bg.svg", 1280, 628,
        '<rect width="1280" height="628" fill="%s"/>' % BG
        + '<rect width="1280" height="60" fill="%s"/><rect y="60" width="1280" height="1" fill="%s"/>' % (PANEL, LINE)
        + grid + text(66, 39, "PLUGIN MANAGER", 24, INK, 700, spacing=3)
        + '<rect x="956" y="15" width="1" height="30" fill="#2e3034"/>'
        + text(976, 26, "INTERNAL STORAGE", 16, MUTED, 600, spacing=1)
        + '<rect x="976" y="36" width="280" height="5" rx="2.5" fill="#2a2b2f"/>'
        + '<rect x="24" y="70" width="580" height="44" rx="8" fill="%s" stroke="%s"/>' % (PANEL, LINE)
        + '<rect x="1230" y="122" width="22" height="400" rx="11" fill="none"/>'
        + '<rect y="566" width="1280" height="62" fill="%s"/><rect y="566" width="1280" height="1" fill="%s"/>' % (PANEL, LINE))
    svg("net_on.svg", 100, 24, '<circle cx="5" cy="12" r="4.5" fill="#3fbf7f"/>' + text(16, 18, "ONLINE", 17, MUTED, 600, spacing=1))
    svg("net_off.svg", 100, 24, '<circle cx="5" cy="12" r="4.5" fill="%s"/>' % RED + text(16, 18, "OFFLINE", 17, "#ff9a93", 600, spacing=1))
    # a list row (the list tile's own picture; the plugin picture sits on its left)
    for name, fill, edge in (("row_off.svg", PANEL, "#222326"), ("row_on.svg", PANEL, "#222326")):   # no selection mark
        svg(name, 1190, 102, '<rect x="1" y="1" width="1188" height="100" rx="8" fill="%s" stroke="%s" stroke-width="2"/>' % (fill, edge))
    svg("hit.svg", 140, 69, '<rect width="140" height="69" fill="#000000" fill-opacity="0.01"/>')   # the picture's tap area
    # row buttons
    button("btn_install.svg", 170, 50, "INSTALL", RED, "#ffffff", "down", size=18)
    button("btn_update.svg", 170, 50, "UPDATE", AMBER, "#1a1205", "refresh", size=18)
    button("btn_installed.svg", 170, 50, "INSTALLED", "none", "#c9ccd1", "check", stroke="#3a3c41", size=17)
    button("btn_q_install.svg", 170, 50, "QUEUED", "none", AMBER_TX, "clock", stroke="#b88426", dash=True, size=17)
    button("btn_q_update.svg", 170, 50, "UPD. QUEUED", "none", AMBER_TX, "clock", stroke="#b88426", dash=True, size=15)
    button("btn_q_remove.svg", 170, 50, "DEL. QUEUED", "none", AMBER_TX, "clock", stroke="#b88426", dash=True, size=15)
    button("btn_disabled.svg", 170, 50, "INSTALL", "#222326", "#5c5f65", "lock", stroke="#2e3034", size=18)
    button("btn_reinstall.svg", 82, 50, "REINSTALL", "#26272b", INK, None, stroke="#3a3c41", size=12)
    button("btn_remove.svg", 82, 50, "REMOVE", "#2a1514", "#ff7a72", None, stroke="#6b2a26", size=12)
    svg("info.svg", 50, 50, '<circle cx="25" cy="25" r="17" fill="none" stroke="#3d8bff" stroke-width="2.8"/>'
        '<circle cx="25" cy="17.5" r="2.2" fill="#3d8bff"/><rect x="23.4" y="22" width="3.2" height="11.5" rx="1.6" fill="#3d8bff"/>')
    svg("trash.svg", 50, 50, icon("trash", 11, 11, 28, MUTED, 2.2))
    svg("trash_open.svg", 50, 50, '<rect x="1" y="1" width="48" height="48" rx="8" fill="#2a1514" stroke="#6b2a26"/>'
        + icon("trash", 11, 11, 28, "#ff7a72", 2.2))
    svg("close.svg", 50, 50, '<rect x="1" y="1" width="48" height="48" rx="8" fill="#26272b" stroke="#3a3c41"/>'
        '<path d="M17 17l16 16M33 17L17 33" stroke="%s" stroke-width="3" stroke-linecap="round"/>' % INK)
    # badges: every combination of channel x high CPU x tested x old install, one 18 px frame each (engine: badge_index)
    frames = ""
    for i in range(24):
        chan, cpu, tested, old = i // 8, (i >> 2) & 1, (i >> 1) & 1, i & 1
        labels = ([BADGES[0]] if chan == 1 else [BADGES[1]] if chan == 2 else []) + ([BADGES[2]] if cpu else []) \
            + ([BADGES[3]] if tested else []) + ([BADGES[4]] if old else [])
        frames += badge_row(labels, i * 22, 22)
    svg("badges.svg", 380, 22 * 24, frames)
    # page up / down, and the position bar between them (0: hidden; 1..11: the page's place)
    for d, path in (("up", "M10 38l8-10 8 10"), ("down", "M10 26l8 10 8-10")):
        for sfx, col in (("", INK), ("_dim", "#3a3c41")):
            svg("page_%s%s.svg" % (d, sfx), 36, 64, '<rect x="1" y="1" width="34" height="62" rx="8" fill="#1d1e21" stroke="#3a3c41" stroke-width="2"/>'
                '<path d="%s" fill="none" stroke="%s" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>' % (path, col))
    svg("clear_thumb.svg", 36, 40, '<rect width="36" height="40" fill="#000000" fill-opacity="0.01"/>')
    svg("clear_track.svg", 36, 250, '<rect width="36" height="250" fill="#000000" fill-opacity="0.01"/>')
    for k in range(12):
        svg("pos_%02d.svg" % k, 20, 294, ('<rect x="7" y="0" width="6" height="294" rx="3" fill="#202124"/>'
                                          '<rect x="4" y="%d" width="12" height="44" rx="6" fill="#6b6e74"/>' % (25 * (k - 1))) if k else "")
    # the info overlay, and the empty states
    svg("overlay.svg", 1232, 420, '<rect x="1" y="1" width="1230" height="418" rx="10" fill="#141416" stroke="#3a3c41" stroke-width="2"/>'
        '<rect x="24" y="88" width="1184" height="1" fill="%s"/>'
        '<rect x="600" y="218" width="612" height="1" fill="%s"/>' % (LINE, LINE))
    svg("overlay_note.svg", 1232, 420, '<rect x="1" y="1" width="1230" height="418" rx="10" fill="#141416" stroke="#3a3c41" stroke-width="2"/>'
        '<rect x="24" y="76" width="1184" height="1" fill="%s"/>' % LINE)
    svg("offline.svg", 1232, 438, '<rect x="1" y="1" width="1230" height="436" rx="10" fill="none" stroke="#2e3034" stroke-dasharray="6 6"/>'
        + icon("nowifi", 596, 100, 40, DIM, 2) + text(616, 190, "Can’t reach the plugin catalog", 24, INK, 700, "middle")
        + text(616, 222, "Check the MPC’s network connection in Preferences, then refresh.", 17, DIM, 400, "middle"))
    svg("empty.svg", 1232, 438, '<rect x="1" y="1" width="1230" height="436" rx="10" fill="none" stroke="#2e3034" stroke-dasharray="6 6"/>')
    button("btn_refresh.svg", 170, 48, "REFRESH", "none", INK, "refresh", stroke=INK, size=18)
    svg("upd_badge.svg", 24, 24, '<circle cx="12" cy="12" r="12" fill="%s"/>' % AMBER)
    svg("banner.svg", 636, 36, '<rect x="0.5" y="0.5" width="635" height="35" rx="8" fill="#2a1514" stroke="#6b2a26"/>'
        + icon("warn", 12, 10, 16, "#ff7a72", 2.5))
    # search field (its text is drawn live on top), clear button, the keyboard overlay and its keys
    svg("search_field.svg", 410, 40, '<rect x="1" y="1" width="408" height="38" rx="19" fill="%s" stroke="#3a3c41" stroke-width="2"/>' % PANEL
        + '<g transform="translate(16 8)" fill="none" stroke="%s" stroke-width="2.4" stroke-linecap="round">'
          '<circle cx="10" cy="10" r="7.5"/><path d="M15.5 15.5L22 22"/></g>' % DIM)
    svg("search_x.svg", 40, 40, '<circle cx="20" cy="20" r="14" fill="#3a3c41"/>'
        '<path d="M14 14l12 12M26 14L14 26" stroke="%s" stroke-width="2.6" stroke-linecap="round"/>' % INK)
    svg("overlay_kb.svg", 1232, 420, '<rect x="1" y="1" width="1230" height="418" rx="10" fill="#141416" stroke="#3a3c41" stroke-width="2"/>'
        '<rect x="24" y="72" width="1184" height="1" fill="%s"/>' % LINE)
    for ch in "1234567890QWERTYUIOPASDFGHJKLZXCVBNM":
        svg("key_%s.svg" % ch, 64, 56, '<rect x="1" y="1" width="62" height="54" rx="8" fill="#26272b" stroke="#3a3c41" stroke-width="2"/>'
            + text(32, 37, ch, 26, INK, 600, "middle"))
    for k, label, w, fill, tx in (("space", "SPACE", 300, "#26272b", INK), ("back", "DELETE", 130, "#26272b", INK),
                                  ("clear", "CLEAR", 130, "#2a1514", "#ff7a72"), ("done", "DONE", 150, RED, "#ffffff")):
        svg("key_%s.svg" % k, w, 56, '<rect x="1" y="1" width="%d" height="54" rx="8" fill="%s" stroke="#3a3c41" stroke-width="2"/>' % (w - 2, fill)
            + text(w / 2, 36, label, 20, tx, 700, "middle", spacing=1.2))
    # view tabs (the option names are drawn on top by the renderer)
    svg("tab_off.svg", 190, 36, '<rect width="190" height="36" rx="6" fill="none"/>')
    svg("tab_on.svg", 190, 36, '<rect x="0.5" y="0.5" width="189" height="35" rx="6" fill="#2a2b2f" stroke="#4a4c52"/>')
    # footer
    button("updall_on.svg", 170, 42, "UPDATE ALL", "none", AMBER_TX, None, stroke=AMBER, size=16)
    button("updall_off.svg", 170, 42, "UPDATE ALL", "none", "#4a4c52", None, stroke="#3a3c41", size=16)
    button("apply_off.svg", 210, 42, "APPLY", "#2a2b2f", "#6b6e74", size=17)
    button("apply_apply.svg", 210, 42, "APPLY", RED, "#ffffff", size=17)
    button("apply_preparing.svg", 210, 42, "PREPARING…", "#2a2b2f", MUTED, size=16)
    button("apply_ready.svg", 210, 42, "RESTART & APPLY", AMBER, "#1a1205", size=15)
    button("apply_retry.svg", 210, 42, "RETRY", AMBER, "#1a1205", "refresh", size=17)
    # the bars: one picture per step (MPC switches them); step 0 of the progress bar is empty, it shows only while downloading
    for k in range(11):
        svg("disk_%02d.svg" % k, 280, 5, '<rect width="%d" height="5" rx="2.5" fill="%s"/>' % (28 * k, DIM) if k else "")
    for k in range(21):
        svg("progress_%02d.svg" % k, 360, 4, ('<rect width="360" height="4" rx="2" fill="#2a2b2f"/>'
                                              '<rect width="%d" height="4" rx="2" fill="%s"/>' % (18 * k, RED)) if k else "")


if __name__ == "__main__":
    main()
