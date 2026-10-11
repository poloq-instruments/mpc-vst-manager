#!/usr/bin/env python3
"""Writes params.json and layout.conf: one MPC tab per category (ALL, INSTRUMENTS, EFFECTS, ADDINS), each with its
own list, scroll position and info overlay. MPC doesn't tell a plugin which of its tabs is showing, so every tab has
its own parameters (prefix a_, i_, e_, d_) and the engine keeps all four lists current. The header, the view tabs
(DISCOVER, INSTALLED, UPDATES) and the footer's buttons are shared. Run after make_thumbs.py (it reads thumbs.txt)."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
CATS = [("a", "ALL"), ("i", "INSTRUMENTS"), ("e", "EFFECTS"), ("d", "ADDINS"), ("p", "COMPANION")]
BIG_PER = 12288 // 275   # the info picture's frames per strip (make_thumbs.py)
ROWS, ROW_Y, ROW_H, ROW_STEP = 4, 208, 102, 108
STATES = ["none", "install", "update", "installed", "q_install", "q_update", "q_remove", "disabled", "menu"]
NBADGES = 24   # channel (none, stable, beta) x high CPU x tested x old install: make_images.py draws them
INFO_LINES = 8
KEY_ROWS = ["1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"]   # the search keyboard (MPC has none for plugins)
SPECIAL = [("space", "Space", 300), ("back", "Delete", 130), ("clear", "Clear", 130), ("done", "Done", 150)]


def ro(key, name):
    return {"key": key, "name": name, "type": "readout", "display": "string"}


def opt(key, name, options):
    return {"key": key, "name": name, "options": options}


def trig(key, name):
    return {"key": key, "name": name, "type": "trigger", "momentary": True}


def params(nthumbs):
    p = [opt("tab", "View", ["DISCOVER", "INSTALLED", "UPDATES"]),
         opt("net", "Network", ["online", "offline"]),
         opt("disk", "Disk used", [str(k) for k in range(11)]),
         ro("disk_txt", "Disk free"),
         opt("upd_badge", "Updates badge", ["off", "on"]),
         ro("upd_count", "Updates"),
         opt("problem", "Problem", ["no", "yes"]),
         ro("problem_txt", "Problem text"),
         trig("refresh", "Refresh"),
         opt("foot", "Footer", ["counts", "warn", "error"]),
         ro("status", "Status"),
         opt("progress", "Progress", [str(k) for k in range(21)]),
         trig("update_all", "Update all"),
         opt("upd_all", "Update all on", ["off", "on"]),
         trig("apply", "Apply"),
         opt("apply_state", "Apply state", ["off", "apply", "preparing", "ready", "retry"]),
         trig("noop", "Badge"),
         trig("search", "Search"),
         ro("search_txt", "Search text"),
         opt("search_set", "Search set", ["off", "on"]),
         trig("search_clear", "Search clear"),
         opt("kb", "Keyboard", ["off", "on"]),
         ro("kb_txt", "Keyboard text"),
         ro("kb_count", "Keyboard matches")]
    p += [trig("kb_" + ch, "Key " + ch) for row in KEY_ROWS for ch in row]
    p += [trig("kb_" + k, "Key " + name) for k, name, _w in SPECIAL]
    thumbs = ["%d" % k for k in range(nthumbs)]
    for c, title in CATS:
        t = title.title()
        # the list's first row: the data wheel (on the invisible slider over the position bar) and Q-Link 1 step it;
        # nudge_pct: a wheel click or a Q-Link event is one row whatever the range
        p += [{"key": c + "_scroll", "name": t + " scroll", "min": 0, "max": 127, "default": 0, "display": "int",
               "nudge_pct": 5, "dynamic_display": True},
              opt(c + "_sb", t + " scroll shown", ["off", "on"]),
              trig(c + "_up", t + " page up"), trig(c + "_down", t + " page down"),
              opt(c + "_upst", t + " up shown", ["hidden", "dim", "on"]), opt(c + "_downst", t + " down shown", ["hidden", "dim", "on"]),
              opt(c + "_pos", t + " position", ["%d" % k for k in range(12)]),
              opt(c + "_empty", t + " empty", ["no", "offline", "empty"]),
              ro(c + "_empty_txt", t + " empty text"),
              ro(c + "_count", t + " count"),
              opt(c + "_ov", t + " info", ["off", "on", "note"]),
              ro(c + "_note_name", t + " note name"), ro(c + "_note_txt", t + " note text"), ro(c + "_note_hp", t + " note homepage"),
              trig(c + "_info_close", t + " info close"),
              opt(c + "_info_bs", t + " info picture strip", ["none"] + ["s%d" % k for k in range(-(-nthumbs // BIG_PER))]),
              opt(c + "_info_bf", t + " info picture frame", ["%d" % k for k in range(BIG_PER)]),
              ro(c + "_info_name", t + " info name"),
              ro(c + "_info_by", t + " info by")]
        p += [ro("%s_info_d%d" % (c, k), "%s info text %d" % (t, k)) for k in range(1, 5)]
        p += [ro("%s_info_l%d" % (c, k), "%s info line %d" % (t, k)) for k in range(1, INFO_LINES + 1)]
        for r in range(1, ROWS + 1):
            n, k = "%s %d" % (t, r), "%s_r%d_" % (c, r)
            p += [{"key": "%s_card%d_1" % (c, r), "name": n, "type": "slot", "display": "string"},
                  opt(k + "vis", n + " shown", ["off", "on"]),
                  opt(k + "img", n + " picture", thumbs),
                  ro(k + "meta", n + " meta"),
                  opt(k + "badges", n + " badges", ["%d" % b for b in range(NBADGES)]),
                  ro(k + "ver", n + " version"),
                  ro(k + "size", n + " size"),
                  opt(k + "state", n + " state", STATES),
                  trig(k + "act", n + " action"),
                  trig(k + "info", n + " info"),
                  opt(k + "trash", n + " trash", ["off", "on", "open"]),
                  trig(k + "del", n + " trash tap"),
                  trig(k + "reinstall", n + " reinstall"),
                  trig(k + "remove", n + " remove")]
    return {"name": "Plugin Manager", "params": p}


HEAD = """# Plugin Manager screen, written by make_layout.py (edit that, not this). Live text sizes are MPC font heights
# = 1.52 x the CSS font size; 1280 x 628 plugin area; shadow coordinates, y = skin y + 86. The artwork is in images/
# (make_images.py, make_thumbs.py); everything live or switched is a parameter the engine computes (src/manager.c).
theme_bg=0c0c0d
theme_ink=9da1a7
theme_seg_active_tx=ffffff
theme_accent=ececed
theme_line=26272a
art_css=skin.css
"""


def tab(c, title, nthumbs):
    L = ["", "[tab %s]" % title, "art file=images/bg.svg"]
    # header (shared): logo and title are in bg.svg; online status, internal storage
    L += ["art file=images/net_on.svg x=820 y=104 w=100 h=24 when=net:online",
          "art file=images/net_off.svg x=820 y=104 w=100 h=24 when=net:offline",
          "readout box=0 w=130 h=28 cx=1191 cy=106 label=\"\" key=disk_txt tsize=25 tcolor=ececed talign=right tpad=0",
          "picture x=976 y=122 w=280 h=5 key=disk files=\"%s\"" % ",".join("images/disk_%02d.svg" % k for k in range(11))]
    # view tabs (shared) and the device problem banner
    L += ["enum_h cx=314 cy=178 sw=188 sh=36 key=tab img=images/tab_off.svg img_on=images/tab_on.svg",
          "button cx=578 cy=178 label=\"\" key=noop img=images/upd_badge.svg w=24 h=24 when=upd_badge:on",
          "readout box=0 w=24 h=25 cx=578 cy=178 label=\"\" key=upd_count tsize=23 tweight=bold tcolor=1a1205 talign=center tpad=0 when=upd_badge:on",
          "button cx=1050 cy=178 label=\"\" key=search img=images/search_field.svg w=410 h=40 get=search_txt tsize=26 when=problem:no",
          "button cx=1228 cy=178 label=\"\" key=search_clear img=images/search_x.svg w=40 h=40 when=search_set:on",
          "art file=images/banner.svg x=620 y=160 w=636 h=36 when=problem:yes",
          "readout box=0 w=590 h=30 cx=957 cy=178 label=\"\" key=problem_txt tsize=23 tcolor=ffb4ae tpad=0 when=problem:yes"]
    # empty states
    L += ["art file=images/offline.svg x=24 y=208 w=1232 h=436 when=%s_empty:offline" % c,
          "button cx=640 cy=490 label=\"\" key=refresh img=images/btn_refresh.svg w=170 h=48 when=%s_empty:offline" % c,
          "art file=images/empty.svg x=24 y=208 w=1232 h=438 when=%s_empty:empty" % c,
          "readout box=0 w=800 h=32 cx=640 cy=427 label=\"\" key=%s_empty_txt tsize=30 tweight=regular tcolor=9da1a7 talign=center tpad=0 when=%s_empty:empty" % (c, c)]
    # the rows
    for r in range(1, ROWS + 1):
        y = ROW_Y + (r - 1) * ROW_STEP
        cy = y + ROW_H // 2
        k = "%s_r%d_" % (c, r)
        L += ["",
              "list x=24 y=%d w=1190 h=%d cols=1 rows=1 th=%d gap=0 key=%s_card%d img=images/row_off.svg img_on=images/row_on.svg "
              "tx=170 ty=8 ttw=480 tth=38 tsize=34 tweight=bold tcolor=ececed when=%svis:on" % (y, ROW_H, ROW_H, c, r, k),
              # the plugin picture; a tap on it opens the info overlay like the (i) button
              "meter cx=104 cy=%d w=140 h=69 key=%simg strip=images/thumbs.png frames=%d when=%svis:on" % (cy, k, nthumbs, k),
              "button cx=104 cy=%d label=\"\" key=%sinfo img=images/hit.svg w=140 h=69 when=%svis:on" % (cy, k, k),
              "readout box=0 w=480 h=26 cx=434 cy=%d label=\"\" key=%smeta tsize=24 tweight=regular tcolor=b4b7bc talign=left tpad=0 when=%svis:on" % (y + 60, k, k),
              "meter cx=384 cy=%d w=380 h=22 key=%sbadges strip=images/badges.svg frames=%d when=%svis:on" % (y + 86, k, NBADGES, k),
              "readout box=0 w=170 h=30 cx=800 cy=%d label=\"\" key=%sver tsize=28 tcolor=ececed tpad=0 when=%svis:on" % (y + 38, k, k),
              "readout box=0 w=170 h=26 cx=800 cy=%d label=\"\" key=%ssize tsize=24 tweight=regular tcolor=9da1a7 tpad=0 when=%svis:on" % (y + 68, k, k),
              # right to left, 12 px apart and 12 px in from the row's edge (1214): trash, the action, info
              "button cx=933 cy=%d label=\"\" key=%sinfo img=images/info.svg w=50 h=50 when=%svis:on" % (cy, k, k)]
        for st in STATES[1:-1]:
            L.append("button cx=1055 cy=%d label=\"\" key=%sact img=images/btn_%s.svg w=170 h=50 when=%sstate:%s" % (cy, k, st, k, st))
        L += ["button cx=1011 cy=%d label=\"\" key=%sreinstall img=images/btn_reinstall.svg w=82 h=50 when=%sstate:menu" % (cy, k, k),
              "button cx=1099 cy=%d label=\"\" key=%sremove img=images/btn_remove.svg w=82 h=50 when=%sstate:menu" % (cy, k, k),
              "button cx=1177 cy=%d label=\"\" key=%sdel img=images/trash.svg w=50 h=50 when=%strash:on" % (cy, k, k),
              "button cx=1177 cy=%d label=\"\" key=%sdel img=images/trash_open.svg w=50 h=50 when=%strash:open" % (cy, k, k)]
    # page up / page down (a dragged slider didn't follow the finger), and where the page is between them
    L += ["",
          "button cx=1240 cy=240 label=\"\" key=%s_up img=images/page_up.svg w=36 h=64 when=%s_upst:on" % (c, c),
          "button cx=1240 cy=240 label=\"\" key=%s_up img=images/page_up_dim.svg w=36 h=64 when=%s_upst:dim" % (c, c),
          "button cx=1240 cy=614 label=\"\" key=%s_down img=images/page_down.svg w=36 h=64 when=%s_downst:on" % (c, c),
          "button cx=1240 cy=614 label=\"\" key=%s_down img=images/page_down_dim.svg w=36 h=64 when=%s_downst:dim" % (c, c),
          "picture x=1230 y=280 w=20 h=294 key=%s_pos files=\"%s\"" % (c, ",".join("images/pos_%02d.svg" % k for k in range(12))),
          # invisible: takes the focus when tapped, so the data wheel scrolls (its value text, below it, is blank)
          "slider_v cx=1240 cy=412 w=36 h=250 bw=36 ns=0 vs=10 label=\"\" key=%s_scroll img=images/clear_thumb.svg "
          "base=images/clear_track.svg when=%s_sb:on" % (c, c)]
    # the info overlay (over the list; the rows hide while it shows). Its texts show only with it: a readout takes the
    # touches over its box even when empty, and these lie over the rows' buttons (INSTALL got no taps). It ends above the footer's text, whose when=
    # images reach up to y 631.
    L += ["", "art file=images/overlay.svg x=24 y=208 w=1232 h=420 when=%s_ov:on" % c,
          "button cx=1222 cy=244 label=\"\" key=%s_info_close img=images/close.svg w=50 h=50 when=%s_ov:on" % (c, c),
          "readout box=0 w=1100 h=42 cx=600 cy=240 label=\"\" key=%s_info_name tsize=40 tweight=bold tcolor=ececed talign=left tpad=0 when=%s_ov:on" % (c, c),
          "readout box=0 w=1100 h=30 cx=600 cy=275 label=\"\" key=%s_info_by tsize=27 tweight=regular tcolor=b4b7bc talign=left tpad=0 when=%s_ov:on" % (c, c)]
    # a companion app's INSTALL: a text note in a plain panel
    L += ["art file=images/overlay_note.svg x=24 y=208 w=1232 h=420 when=%s_ov:note" % c,
          "button cx=1222 cy=244 label=\"\" key=%s_info_close img=images/close.svg w=50 h=50 when=%s_ov:note" % (c, c),
          "readout box=0 w=1100 h=42 cx=600 cy=240 label=\"\" key=%s_note_name tsize=40 tweight=bold tcolor=ececed talign=left tpad=0 when=%s_ov:note" % (c, c),
          "readout box=0 w=1180 h=32 cx=638 cy=330 label=\"\" key=%s_note_txt tsize=29 tweight=regular tcolor=ececed talign=left tpad=0 when=%s_ov:note" % (c, c),
          "readout box=0 w=1180 h=32 cx=638 cy=390 label=\"\" key=%s_note_hp tsize=29 tweight=regular tcolor=ececed talign=left tpad=0 when=%s_ov:note" % (c, c)]
    # the picture, half the overlay wide: three strips (a strip holds BIG_PER frames), the engine shows the right one
    for s in range(-(-nthumbs // BIG_PER)):
        L.append("meter cx=330 cy=446 w=560 h=275 key=%s_info_bf strip=images/thumbs_big_%d.png frames=%d when=%s_info_bs:s%d"
                 % (c, s, BIG_PER, c, s))
    # the text beside it
    for d in range(4):
        L.append("readout box=0 w=610 h=30 cx=931 cy=%d label=\"\" key=%s_info_d%d tsize=26 tweight=regular tcolor=ececed talign=left tpad=0 when=%s_ov:on"
                 % (322 + d * 28, c, d + 1, c))
    for l in range(INFO_LINES):
        L.append("readout box=0 w=610 h=27 cx=931 cy=%d label=\"\" key=%s_info_l%d tsize=24 tweight=regular tcolor=c9ccd1 talign=left tpad=0 when=%s_ov:on"
                 % (442 + l * 24, c, l + 1, c))
    # the search keyboard (shared by the four tabs; the rows hide while it shows)
    L += ["", "art file=images/overlay_kb.svg x=24 y=208 w=1232 h=420 when=kb:on",
          "button cx=1222 cy=244 label=\"\" key=kb_done img=images/close.svg w=50 h=50 when=kb:on",
          "readout box=0 w=760 h=46 cx=444 cy=250 label=\"\" key=kb_txt tsize=42 tweight=bold tcolor=ececed talign=left tpad=0 when=kb:on",
          "readout box=0 w=300 h=30 cx=1010 cy=250 label=\"\" key=kb_count tsize=27 tweight=regular tcolor=b4b7bc talign=right tpad=0 when=kb:on"]
    for ri, row in enumerate(KEY_ROWS):
        x0 = 640 - (len(row) * 70 - 6) // 2 + 32
        for ki, ch in enumerate(row):
            L.append("button cx=%d cy=%d label=\"\" key=kb_%s img=images/key_%s.svg w=64 h=56 when=kb:on" % (x0 + ki * 70, 318 + ri * 64, ch, ch))
    x = 640 - (sum(w for _k, _n, w in SPECIAL) + 6 * (len(SPECIAL) - 1)) // 2
    for k, _name, w in SPECIAL:
        L.append("button cx=%d cy=574 label=\"\" key=kb_%s img=images/key_%s.svg w=%d h=56 when=kb:on" % (x + w // 2, k, k, w))
        x += w + 6
    # footer: counts (or a warning), progress while downloading, UPDATE ALL, APPLY
    L += ["",
          "readout box=0 w=600 h=30 cx=330 cy=684 label=\"\" key=%s_count tsize=28 tweight=regular tcolor=ececed talign=left tpad=0 when=foot:counts" % c,
          "readout box=0 w=600 h=30 cx=330 cy=684 label=\"\" key=status tsize=26 tweight=regular tcolor=f0b44a talign=left tpad=0 when=foot:warn",
          "readout box=0 w=600 h=30 cx=330 cy=684 label=\"\" key=status tsize=26 tweight=regular tcolor=ff7a72 talign=left tpad=0 when=foot:error",
          "picture x=30 y=706 w=360 h=4 key=progress files=\"%s\"" % ",".join("images/progress_%02d.svg" % k for k in range(21)),
          "button cx=949 cy=684 label=\"\" key=update_all img=images/updall_on.svg w=170 h=42 when=upd_all:on",
          "button cx=949 cy=684 label=\"\" key=update_all img=images/updall_off.svg w=170 h=42 when=upd_all:off"]
    for st in ("off", "apply", "preparing", "ready", "retry"):
        L.append("button cx=1151 cy=684 label=\"\" key=apply img=images/apply_%s.svg w=210 h=42 when=apply_state:%s" % (st, st))
    L += ["", "# Q-Link 1 scrolls the list", "qlinks \"%s\" = %s_scroll" % (title, c)]
    return L


def main():
    ids = [l for l in open(os.path.join(HERE, "thumbs.txt")).read().split("\n") if l]
    nthumbs = len(ids) + 1
    json.dump(params(nthumbs), open(os.path.join(HERE, "params.json"), "w"), indent=2)
    out = [HEAD.rstrip("\n")]
    for c, title in CATS:
        out += tab(c, title, nthumbs)
    open(os.path.join(HERE, "layout.conf"), "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
