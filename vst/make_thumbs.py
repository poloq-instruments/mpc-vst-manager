#!/usr/bin/env python3
"""Bakes the list's plugin pictures: one 140x69 frame per catalog entry (the plugin screen's own shape, about 2:1),
its whole screenshot, or its initials when it has none. Frame 0 is the generic picture for entries newer than this build.
Writes images/thumbs.png (frames stacked down), thumbs.txt (the ids in frame order, read by make_layout.py) and
../src/thumbs.h (the same ids for the engine). Needs Pillow and the network:  python3 vst/make_thumbs.py"""
import io
import json
import os
import sys
import urllib.request

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
CATALOG = "https://sd88me.github.io/mpc-vst-plugins/catalog.json"
APPS = "https://sd88me.github.io/mpc-vst-plugins/apps.json"   # the companion apps: their own MPC tab
W, H = 140, 69
BW, BH = 560, 275   # the info overlay's picture (half its width)
BIG_PER = 12288 // BH   # frames per big strip: a strip may be at most 12288 px tall (shadow_skin.MAX_STRIP)
MAX = 127   # the meter's filmstrip holds at most 128 frames (frame 0 + 127 plugins)
BG, EDGE, INK = (36, 38, 42), (52, 54, 59), (236, 236, 237)


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "plugin-manager-thumbs"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def font(size):
    for f in ("/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default(size)


def tile(text):
    im = Image.new("RGB", (W, H), (12, 12, 13))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, W - 1, H - 1), 6, fill=BG, outline=EDGE)
    d.text((W / 2, H / 2), text, font=font(26), fill=INK, anchor="mm")
    return im


def initials(name):
    words = [w for w in name.replace("-", " ").split() if w[:1].isalpha()]
    s = "".join(w[0] for w in words[:2]).upper()
    return s if len(s) > 1 or not name[1:2] else (name[:2]).upper()


def fit(src, w, h, bg, radius):
    im = src.copy()
    im.thumbnail((w, h), Image.LANCZOS)   # the whole screen, its shape kept, centred in the frame
    pic = Image.new("RGB", (w, h), bg)
    pic.paste(im, ((w - im.size[0]) // 2, (h - im.size[1]) // 2))
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1), radius, fill=255)
    out = Image.new("RGB", (w, h), bg)
    out.paste(pic, (0, 0), mask)
    return out


def shot(url):
    return Image.open(io.BytesIO(fetch(url))).convert("RGB")


def main():
    plugins = json.loads(fetch(CATALOG))["plugins"]
    plugins = [p for p in plugins if any(v.get("url") for v in p.get("versions", []))]
    plugins += [dict(a, name=a["title"]) for a in json.loads(fetch(APPS))["apps"]]
    plugins = plugins[:MAX]
    frames, big, ids = [tile("?")], [tile("?").resize((BW, BH))], [""]
    for p in plugins:
        pic = None
        if p.get("screenshot"):
            try:
                pic = shot(p["screenshot"])
            except Exception as e:   # a broken link: the initials instead
                print("  %s: %s" % (p["id"], e), file=sys.stderr)
        if pic:
            frames.append(fit(pic, W, H, (12, 12, 13), 5))
            big.append(fit(pic, BW, BH, (20, 20, 22), 10))
        else:
            frames.append(tile(initials(p["name"])))
            big.append(tile(initials(p["name"])).resize((BW, BH)))
        ids.append(p["id"])
    nstrips = -(-len(big) // BIG_PER)   # every big strip has BIG_PER frames (the last padded), as its meter says
    for s in range(nstrips):
        strip = Image.new("RGB", (BW, BH * BIG_PER), (20, 20, 22))
        for f, im in enumerate(big[s * BIG_PER:(s + 1) * BIG_PER]):
            strip.paste(im, (0, f * BH))
        strip.save(os.path.join(HERE, "images", "thumbs_big_%d.png" % s), optimize=True)
    strip = Image.new("RGB", (W, H * len(frames)))
    for i, f in enumerate(frames):
        strip.paste(f, (0, i * H))
    strip.save(os.path.join(HERE, "images", "thumbs.png"), optimize=True)
    open(os.path.join(HERE, "thumbs.txt"), "w").write("\n".join(ids[1:]) + "\n")
    with open(os.path.join(HERE, "..", "src", "thumbs.h"), "w") as f:
        f.write("/* written by vst/make_thumbs.py: frame i of images/thumbs.png is THUMB_IDS[i] (0: no picture) */\n")
        f.write("static const char *const THUMB_IDS[] = {\n" + "".join('    "%s",\n' % i for i in ids) + "};\n")
        f.write("#define NTHUMBS %d\n" % len(ids))
        f.write("#define THUMB_BIG_PER %d   /* frames per images/thumbs_big_<n>.png (the info overlay's picture) */\n" % BIG_PER)
    print("%d frames" % len(frames))


if __name__ == "__main__":
    main()
