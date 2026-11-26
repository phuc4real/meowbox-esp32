#!/usr/bin/env python3
"""MeowBox cat sprites -- the single source of truth.

Every sprite below is a list of text rows, one character per pixel:

    .  transparent (index 0)
    O  outline            (fur-dependent; light rim for the black cat)
    F  fur main           (fur-dependent)
    S  fur shade          (fur-dependent)
    T  tabby stripe       (fur-dependent; invisible = fur on white / calico)
    P  patch 1            (fur-dependent; orange on calico, = fur elsewhere)
    Q  patch 2            (fur-dependent; black on calico, = fur elsewhere)
    L  belly / muzzle light (fur-dependent)
    E  eye                (fur-dependent: amber on black, blue on white, ...)
    W  white              (fixed)  eye highlight, teeth, laptop, speech bubbles
    K  pink               (fixed)  inner ear, nose, blush, tongue, laptop logo
    D  dark               (fixed)  pupils, prop and laptop outline
    B  blue               (fixed)  Zzz
    R  red                (fixed)  question mark
    Y  yellow             (fixed)  sparkle
    N  brown              (fixed)  (free)

Props may only use the fixed roles (W K D B R Y N) so they look identical with
every fur palette.

Usage:
    python sprites.py            # validate + write C files + previews
    python sprites.py gen        # validate + write C files only
    python sprites.py preview    # previews only (needs Pillow)
    python sprites.py check      # validate only
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.normpath(os.path.join(HERE, "..", ".."))
COMP_DIR = os.path.join(FW, "components", "cat_sprites")
PREVIEW_DIR = os.path.join(HERE, "preview")

CHARS = ".OFSTPQLEWKDBRYN"          # index = palette slot
FIXED = "WKDBRYN"                  # slots shared by every fur palette
ROLE_NAMES = ["transparent", "outline", "fur", "fur_shade", "stripe", "patch", "patch2",
              "light", "eye", "white", "pink", "dark", "blue", "red", "yellow", "brown"]

POSES = ["SIT", "BLINK", "SLEEP1", "SLEEP2", "HAPPY", "SAD", "ANGRY", "CODE1", "CODE2", "THINK", "WAIT"]
PROP_NAMES = ["ZZZ", "SPARKLE", "QUESTION", "DOTS"]
FURS = ["tabby", "orange", "black", "white", "calico", "gray"]

_FIXED_COLS = ["#FFFFFF", "#F49AB0", "#2A1C22", "#5AA8E6", "#E8434F", "#FFD23F", "#8A5A3C"]
#          outline    fur        shade      stripe     patch      patch2     light      eye
PALETTES = {
    # "meo muop": grey-brown tabby with dark stripes
    "tabby":  ["#241C18", "#9A8A74", "#776A58", "#3F352C", "#9A8A74", "#9A8A74", "#E9E0CF", "#1D1922"],
    "orange": ["#3A1F12", "#F2A65A", "#D27F38", "#B85C22", "#F2A65A", "#F2A65A", "#FFF1DC", "#1D1922"],
    "black":  ["#7F7A99", "#2E2B39", "#211F29", "#26232F", "#2E2B39", "#2E2B39", "#4D4859", "#F2C744"],
    "white":  ["#3B3444", "#F5F2ED", "#CFCAD8", "#F5F2ED", "#F5F2ED", "#F5F2ED", "#FFFFFF", "#27458F"],
    "calico": ["#2E2228", "#F6F2EA", "#D6CFC8", "#F6F2EA", "#E8913A", "#3A3238", "#FFFFFF", "#1D1922"],
    "gray":   ["#22252E", "#A0A7B4", "#7C8392", "#7A8190", "#A0A7B4", "#A0A7B4", "#DDE0E7", "#23503A"],
}
for _k in PALETTES:
    PALETTES[_k] = ["#000000"] + PALETTES[_k] + _FIXED_COLS

# Theme palettes (full 16 entries: index 0 unused, then the 15 roles). Used by MeowBox UI themes.
THEME_PALETTES = {
    "neon": ["#000000", "#00E5FF", "#151A2C", "#0E1120", "#FF2BD6", "#151A2C", "#151A2C", "#2B3558", "#F5F032",
             "#E8F6FF", "#FF2BD6", "#05060A", "#00E5FF", "#FF3B3B", "#F5F032", "#6B3FA0"],
    "mono": ["#000000", "#000000", "#111111", "#000000", "#000000", "#111111", "#111111", "#3A3A3A", "#F2F0EA",
             "#F2F0EA", "#D7141A", "#000000", "#0047BB", "#D7141A", "#FFCC00", "#3A3A3A"],
    "sumi": ["#000000", "#24211D", "#77716A", "#5A5550", "#3E3A35", "#77716A", "#77716A", "#C7BFB2", "#3E4A6B",
             "#F3EDE1", "#D98C98", "#24211D", "#3E4A6B", "#A8322A", "#C9A54A", "#74664F"],
}


# ============================================================================ DATA
SPRITES = {  # the adult cat (the kitten, junior and senior ages of the virtual pet are in git history)
    "SIT": [
        ".....OPO................OQO.....",
        ".....OPPO..............OQQO.....",
        ".....OPKPOOOOOOOOOOOOOOQKQO.....",
        ".....OPKKPPFFFFTTFFQQQQKKQO.....",
        ".....OPKKPPFFTFTTFTQQQQKKQO.....",
        "......OPPFFFFTFFFFTFFFFQQO......",
        ".....OFFFFFFFFFFFFFFFFFFFFO.....",
        ".....OFFFWEEFFFFFFFFWEEFFFO.....",
        "....OTTTFEDEFFFFFFFFEDEFTTTO....",
        "....OFFFFEDEFFFFFFFFEDEFFFFO....",
        "....OTTKKFFFFLLKKLLFFFFKKTTO....",
        ".....OFFFFFFFLOLLOLFFFFFFFO.....",
        "......OFFFFFFFLOOLFFFFFFFO......",
        ".......OOFFFFFFFFFFFFFFOO.......",
        ".........OOOOOOOOOOOOOO.........",
        "........OPPPPPFLLFFFFFFO........",
        "........OTTTPPLLLLFFTTTO........",
        ".......OPPPPPLLLLLLFFFFFO.......",
        ".......OPPPPPLLLLLLFFFFFO...OO..",
        "......OTTTPPPLLLLLLFFFTTTO.OFFO.",
        "......OPPPPPPLLLLLLFFFFFFOOFFFO.",
        ".....OSPPPPPPLLLLLLFFFFFFSOTTO..",
        ".....OTTTFFFFLLLLLLQQQQTTTOFFO..",
        ".....OSSFFFFFLLLLLLQQQQQSSOFO...",
        ".....OSSFFFFFLLLLLLQQQQQSSOTO...",
        ".....OTTTFFFFFLLLLFQQQQTTTOFO...",
        ".....OSSSFFFFFFLLFFQQQQSSSOFO...",
        "......OSSFFOOFFFFFFOOQQSSOTO....",
        ".......OSSOLLOFFFFOLLOSSOFO.....",
        "........OOLLLLOFFOLLLLOOOO......",
        ".........OLLLLOOOOLLLLO.........",
        "..........OOOO....OOOO..........",
    ],
    "BLINK": [
        ".....OPO................OQO.....",
        ".....OPPO..............OQQO.....",
        ".....OPKPOOOOOOOOOOOOOOQKQO.....",
        ".....OPKKPPFFFFTTFFQQQQKKQO.....",
        ".....OPKKPPFFTFTTFTQQQQKKQO.....",
        "......OPPFFFFTFFFFTFFFFQQO......",
        ".....OFFFFFFFFFFFFFFFFFFFFO.....",
        ".....OFFFFFFFFFFFFFFFFFFFFO.....",
        "....OTTTFOFOFFFFFFFFOFOFTTTO....",
        "....OFFFFFOFFFFFFFFFFOFFFFFO....",
        "....OTTKKFFFFLLKKLLFFFFKKTTO....",
        ".....OFFFFFFFLOLLOLFFFFFFFO.....",
        "......OFFFFFFFLOOLFFFFFFFO......",
        ".......OOFFFFFFFFFFFFFFOO.......",
        ".........OOOOOOOOOOOOOO.........",
        "........OPPPPPFLLFFFFFFO........",
        "........OTTTPPLLLLFFTTTO........",
        ".......OPPPPPLLLLLLFFFFFO.......",
        ".......OPPPPPLLLLLLFFFFFO...OO..",
        "......OTTTPPPLLLLLLFFFTTTO.OFFO.",
        "......OPPPPPPLLLLLLFFFFFFOOFFFO.",
        ".....OSPPPPPPLLLLLLFFFFFFSOTTO..",
        ".....OTTTFFFFLLLLLLQQQQTTTOFFO..",
        ".....OSSFFFFFLLLLLLQQQQQSSOFO...",
        ".....OSSFFFFFLLLLLLQQQQQSSOTO...",
        ".....OTTTFFFFFLLLLFQQQQTTTOFO...",
        ".....OSSSFFFFFFLLFFQQQQSSSOFO...",
        "......OSSFFOOFFFFFFOOQQSSOTO....",
        ".......OSSOLLOFFFFOLLOSSOFO.....",
        "........OOLLLLOFFOLLLLOOOO......",
        ".........OLLLLOOOOLLLLO.........",
        "..........OOOO....OOOO..........",
    ],
    "SLEEP1": [
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        ".........OPO................OQO.",
        ".........OPPO..............OQQO.",
        ".........OPKPOOOOOOOOOOOOOOQKQO.",
        ".........OPKKPPFFFFTTFFQQQQKKQO.",
        ".........OPKKPPFFTFTTFTQQQQKKQO.",
        "..........OPPFFFFTFFFFTFFFFQQO..",
        "........OOFFFFFFFFFFFFFFFFFFFFO.",
        ".....OOOPOFFFFFFFFFFFFFFFFFFFFO.",
        "....OQQTOTTTFOFOFFFFFFFFOFOFTTTO",
        "...OTQQTOFFFFFOFFFFFFFFFFOFFFFFO",
        "..OQTQQTOTTKKFFFFLLKKLLFFFFKKTTO",
        ".OQQTQQQFOFFFFFFFLLLLLLFFFFFFFO.",
        ".OQQQQQQFFOFFFFFFFLLLLFFFFFFFO..",
        "OFFFFFFFFFFOOFFFFFFFFFFFFFFOO...",
        ".OOOFFFFFFFFFOOOOOOOOOOOOOO.....",
        ".OFFOFFFFFFFFFFFFFFFFFFFFFO.....",
        ".OTTTOOOFFFFFFFFFFFOOOFFFO......",
        "..OFFFFFOOOOOOOOOOOFFFOSO.......",
        "...OFFFFFFFFFFFFFFFFFFOO........",
        "....OOOTTTTTTTTTTTTTOOO.........",
        ".......OOOOOOOOOOOOO............",
    ],
    "SLEEP2": [
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        ".........OPO................OQO.",
        ".........OPPO..............OQQO.",
        ".........OPKPOOOOOOOOOOOOOOQKQO.",
        ".........OPKKPPFFFFTTFFQQQQKKQO.",
        ".........OPKKPPFFTFTTFTQQQQKKQO.",
        "..........OPPFFFFTFFFFTFFFFQQO..",
        "........OOFFFFFFFFFFFFFFFFFFFFO.",
        "......OOPOFFFFFFFFFFFFFFFFFFFFO.",
        "....OOQTOTTTFOFOFFFFFFFFOFOFTTTO",
        "...OTQQTOFFFFFOFFFFFFFFFFOFFFFFO",
        "..OQTQQTOTTKKFFFFLLKKLLFFFFKKTTO",
        ".OQQTQQQPOFFFFFFFLLLLLLFFFFFFFO.",
        ".OQQQQQQFFOFFFFFFFLLLLFFFFFFFO..",
        ".OQQQQQQFFFOOFFFFFFFFFFFFFFOO...",
        ".OFFFFFFFFFFFOOOOOOOOOOOOOO.....",
        ".OOOFFFFFFFFFFFFFFFFFFFFFFO.....",
        ".OFFOFFFFFFFFFFFFFFFFFFFFFO.....",
        ".OTTTOOOFFFFFFFFFFFOOOFFFO......",
        "..OFFFFFOOOOOOOOOOOFFFOSO.......",
        "...OFFFFFFFFFFFFFFFFFFOO........",
        "....OOOTTTTTTTTTTTTTOO..........",
        ".......OOOOOOOOOOOOO............",
    ],
    "HAPPY": [
        ".....OPO................OQO.....",
        ".....OPPO..............OQQO.....",
        ".....OPKPOOOOOOOOOOOOOOQKQO.....",
        ".....OPKKPPFFFFTTFFQQQQKKQO.....",
        ".....OPKKPPFFTFTTFTQQQQKKQO.....",
        "......OPPFFFFTFFFFTFFFFQQO......",
        ".....OFFFFFFFFFFFFFFFFFFFFO.....",
        ".....OFFFFFFFFFFFFFFFFFFFFO.....",
        "....OTTTFFOFFFFFFFFFFOFFTTTO....",
        "....OFFFFOFOFFFFFFFFOFOFFFFO....",
        "....OTTKKFFFFLLKKLLFFFFKKTTO....",
        ".....OFFFFFFFLOOOOLFFFFFFFO.....",
        "......OFFFFFFFOKKOFFFFFFFOOO....",
        ".......OOFFFFFFOOFFFFFFOOOFFO...",
        ".........OOOOOOOOOOOOOO.OFFFFO..",
        "........OPPPPPFLLFFFFFFOOTTOO...",
        "........OTTTPPLLLLFFTTTOOFFO....",
        ".......OPPPPPLLLLLLFFFFFOFFO....",
        ".......OPPPPPLLLLLLFFFFFOTTO....",
        "......OTTTPPPLLLLLLFFFTTTOFO....",
        "......OPPPPPPLLLLLLFFFFFFOFO....",
        ".....OSPPPPPPLLLLLLFFFFFFSOTO...",
        ".....OTTTFFFFLLLLLLQQQQTTTOFFO..",
        ".....OSSFFFFFLLLLLLQQQQQSSOFFO..",
        ".....OSSFFFFFLLLLLLQQQQQSSOOO...",
        ".....OTTTFFFFFLLLLFQQQQTTTO.....",
        ".....OSSSFFFFFFLLFFQQQQSSSO.....",
        "......OSSFFOOFFFFFFOOQQSSO......",
        ".......OSSOLLOFFFFOLLOSSO.......",
        "........OOLLLLOFFOLLLLOO........",
        ".........OLLLLOOOOLLLLO.........",
        "..........OOOO....OOOO..........",
    ],
    "SAD": [
        "................................",
        "................................",
        "........OOO..........OOO........",
        "......OOPPPOOOOOOOOOOQQQOO......",
        ".....OPPKKPFFFFTTFFQQQKKQQO.....",
        "....OPPKKPPFFTFTTFTQQQQKKQQO....",
        "....PPKPPFFFFTFFFFTFFFFQQKQQ....",
        "....FFFFFFOFFFFFFFFFFOFFFFFF....",
        "....OOFFFOFFFFFFFFFFFFOFFFOO....",
        "....OTTTFWEEFFFFFFFFEEWFTTTO....",
        "....OFFFFEDEFFFFFFFFEDEFFFFO....",
        "....OTTTFBFFFLLKKLLFFFFFTTTO....",
        ".....OFFFBFFFLLOOLLFFFFFFFO.....",
        "......OFFFFFFFOLLOFFFFFFFO......",
        ".......OOFFFFFFFFFFFFFFOO.......",
        "........OOOOOOOOOOOOOOOO........",
        "........OTTTPPLLLLFFTTTO........",
        ".......OPPPPPLLLLLLFFFFFO.......",
        ".......OPPPPPLLLLLLFFFFFO.......",
        "......OTTTPPPLLLLLLFFFTTTO......",
        "......OPPPPPPLLLLLLFFFFFFO......",
        ".....OSPPPPPPLLLLLLFFFFFFSO.....",
        ".....OTTTFFFFLLLLLLQQQQTTTO.....",
        ".....OSSFFFFFLLLLLLQQQQQSSO.....",
        ".....OSSFFFFFLLLLLLQQQQQSSO.....",
        ".....OTTTFFFFFLLLLFQQQQTTTO.....",
        ".....OSSSFFFFFFLLFFQQQQSSSO.....",
        "......OSSFFOOFFFFFFOOQQSSOOOOOO.",
        ".......OSSOLLOFFFFOLLOSSOOFFFFFO",
        "........OOLLLLOFFOLLLLOOFFFFFFO.",
        ".........OLLLLOOOOLLLLOOOOOOOO..",
        "..........OOOO....OOOO..........",
    ],
    "ANGRY": [
        "................................",
        "................................",
        "................................",
        "................................",
        "........OOOO..OOOOOOOOOOOO..OOOO",
        "..OOOO..PPPPOOPFFFFTTFFQQQOOQQQQ",
        ".OFTTTO.OPPKKPPFFTFTTFTQQQQKKQQO",
        ".OOFFFO..OPPPFFFFTFFFFTFFFFQQQO.",
        "OFFFFFO..OFFFFFFFFFFFFFFFFFFFFO.",
        ".OTTTTO..OFFFEOFFFFFFFFFFOEFFFO.",
        "OFFFFO..OTTTFEDOOFFFFFFOODEFTTTO",
        ".OFFFOOOOFFFFEDEFFFFFFFFEDEFFFFO",
        "OFTTTOTOOTTTFFFFFLLKKLLFFFFFTTTO",
        ".OFFOOTFFOFFFFFFFOOOOOOFFFFFFFO.",
        "OFFOQQTFFTOFFFFFFOWKKWOFFFFFFO..",
        ".OTOQQTFFFFOOFFFFFOOOOFFFFFOO...",
        "OFOQQQQFFFFFPOOOOOOOOOOOOOO.....",
        ".OOQQQQFFFFFPPPPPPPPPPPO........",
        "OFOQQQQFFFFFPPPPPPPPPPPO........",
        ".OOQQQQFFFFFPPPPPPPPPPPO........",
        "OFOQQQQFFFFFFFFFFFFFFFFFO.......",
        ".OOQQQQFFFFFFFFFFFFFFFFFO.......",
        "...OFFFFFFFLLLLLLLLLLFFFO.......",
        "...OOFFFFFFLLLLLLLLLLFFFO.......",
        "...OSOFFFFFLLLLLLLLLOFFFO.......",
        "...OSSOOFFFLLLLLLLOOOFFFO.......",
        "...OSSSOFFFOOOOOOOSSOFFFO.......",
        "...OSSSOFFFO....OSSSOFFFO.......",
        "...OSSSOFFFO....OSSSOFFFO.......",
        "...OLLLOLLLO....OLLLOLLLO.......",
        "...OLLLOLLLO....OLLLOLLLO.......",
        "....OOO.OOO......OOO.OOO........",
    ],
}

PROPS = {
    "ZZZ": [
        "...........DBBBB",
        "............DDBD",
        "............DBDD",
        ".......DDDDDBBBB",
        "......DBBBBBDDDD",
        ".......DDDBBD...",
        "........DBBD....",
        ".......DBBDD....",
        ".DDDDDDBBBBBD...",
        "DBBBBBBDDDDD....",
        ".DDDDBBD........",
        "...DBBD.........",
        "..DBBD..........",
        ".DBBDDD.........",
        "DBBBBBBD........",
        ".DDDDDD.........",
    ],
    "SPARKLE": [
        ".............D..",
        "............DYD.",
        "...........DDYDD",
        "......D...DYYWYY",
        ".....DYD...DDYDD",
        ".....DYD....DYD.",
        ".....DYD.....D..",
        "....DYYYD.......",
        ".DDDYWYYYDDD....",
        "DYYYYYWYYYYYD...",
        ".DDDYYYYYDDD....",
        "....DYYYD.......",
        ".....DYD........",
        ".....DYD........",
        ".....DYD........",
        "......D.........",
    ],
}
# ---------------------------------------------------------------------------- Meow Coding
# Speech bubble with a question mark: an agent waits for you (a permission prompt or a question).
PROPS["QUESTION"] = [
    "................",
    "..DDDDDDDDDDDD..",
    ".DWWWWWWWWWWWWD.",
    ".DWWWWRRRRWWWWD.",
    ".DWWWRRWWRRWWWD.",
    ".DWWWWWWWRRWWWD.",
    ".DWWWWWWRRWWWWD.",
    ".DWWWWWRRWWWWWD.",
    ".DWWWWWRRWWWWWD.",
    ".DWWWWWWWWWWWWD.",
    ".DWWWWWRRWWWWWD.",
    ".DWWWWWWWWWWWWD.",
    "..DDDWWDDDDDDD..",
    "....DWD.........",
    "....DD..........",
    "................",
]
# Thought bubble "...": thinking between tool calls.
PROPS["DOTS"] = [
    "................",
    "...DDDDDDDDDD...",
    "..DWWWWWWWWWWD..",
    ".DWWWWWWWWWWWWD.",
    ".DWWWWWWWWWWWWD.",
    ".DWDDWWDDWWDDWD.",
    ".DWDDWWDDWWDDWD.",
    ".DWWWWWWWWWWWWD.",
    "..DWWWWWWWWWWD..",
    "...DDDDDDDDDD...",
    ".....DWWD.......",
    ".....DDDD.......",
    "...DWD..........",
    "...DD...........",
    "................",
    "................",
]

# The coding poses are the sitting cat (SIT, BLINK) with a laptop in front of it and its paws on the
# keyboard edge. The overlays are drawn here as text too; "." keeps the cat pixel underneath.
LAPTOP = [
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "...DDDDDDDDDDDDDDDDDDDDDDDDDD...",
    "...DWWWWWWWWWWWWWWWWWWWWWWWWD...",
    "...DWWWWWWWWWWWWWWWWWWWWWWWWD...",
    "...DWWWWWWWWWWKWWKWWWWWWWWWWD...",
    "...DWWWWWWWWWWKKKKWWWWWWWWWWD...",
    "...DWWWWWWWWWWKKKKWWWWWWWWWWD...",
    "...DWWWWWWWWWWWKKWWWWWWWWWWWD...",
    "...DWWWWWWWWWWWWWWWWWWWWWWWWD...",
    "...DWWWWWWWWWWWWWWWWWWWWWWWWD...",
    "...DWWWWWWWWWWWWWWWWWWWWWWWWD...",
    "...DDDDDDDDDDDDDDDDDDDDDDDDDD...",
    "..DWWWWWWWWWWWWWWWWWWWWWWWWWWD..",
    "..DDDDDDDDDDDDDDDDDDDDDDDDDDDD..",
]
PAW = [".OO.", "OLLO", "OLLO"]  # a front paw seen from above the lid


def _overlay(base, over):
    return ["".join(o if o != "." else b for b, o in zip(br, orow)) for br, orow in zip(base, over)]


def _paws(rows, left_up, right_up):
    """Paws resting on the lid edge (row 19) or lifted 2 px (typing)."""
    rows = [list(r) for r in rows]
    for x, up in ((8, left_up), (20, right_up)):
        y0 = 15 if up else 17
        for dy, line in enumerate(PAW):
            for dx, c in enumerate(line):
                if c != ".":
                    rows[y0 + dy][x + dx] = c
    return ["".join(r) for r in rows]


_desk = _overlay(SPRITES["SIT"], LAPTOP)
SPRITES["CODE1"] = _paws(_desk, True, False)
SPRITES["CODE2"] = _paws(_desk, False, True)
SPRITES["THINK"] = _paws(_overlay(SPRITES["BLINK"], LAPTOP), False, False)  # eyes closed, paws still
SPRITES["WAIT"] = _paws(_desk, True, True)  # both paws up, looking at you
# ============================================================================ /DATA


def validate():
    errs = []
    for p in POSES:
        rows = SPRITES.get(p)
        if rows is None:
            errs.append(f"missing sprite {p}")
            continue
        if len(rows) != 32 or any(len(r) != 32 for r in rows):
            errs.append(f"{p}: must be 32x32")
        bad = {c for r in rows for c in r if c not in CHARS}
        if bad:
            errs.append(f"{p}: unknown chars {bad}")
    for n in PROP_NAMES:
        rows = PROPS.get(n)
        if rows is None:
            errs.append(f"missing prop {n}")
            continue
        if len(rows) != 16 or any(len(r) != 16 for r in rows):
            errs.append(f"prop {n}: must be 16x16")
        bad = {c for r in rows for c in r if c != "." and c not in FIXED}
        if bad:
            errs.append(f"prop {n}: uses fur-dependent roles {bad}")
    for f in FURS:
        if len(PALETTES[f]) != 16:
            errs.append(f"palette {f}: needs 16 entries")
        for c in FIXED:
            i = CHARS.index(c)
            if PALETTES[f][i] != PALETTES[FURS[0]][i]:
                errs.append(f"palette {f}: fixed slot {i} ({c}) differs")
    if errs:
        raise SystemExit("sprite validation failed:\n  " + "\n  ".join(errs))


def pack(rows):
    out = []
    for r in rows:
        idx = [CHARS.index(c) for c in r]
        for i in range(0, len(idx), 2):
            out.append((idx[i] << 4) | idx[i + 1])
    return out


def rgb565(hexcol):
    r, g, b = (int(hexcol[i:i + 2], 16) for i in (1, 3, 5))
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def _c_bytes(data, indent="        "):
    lines = []
    for i in range(0, len(data), 16):
        lines.append(indent + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    return "\n".join(lines)


HEADER = r'''/* AUTO-GENERATED by firmware/tools/cat_sprites/sprites.py -- do not edit by hand. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

@POSE_ENUM@
@PROP_ENUM@
typedef enum { CAT_FUR_TABBY, CAT_FUR_ORANGE, CAT_FUR_BLACK, CAT_FUR_WHITE, CAT_FUR_CALICO, CAT_FUR_GRAY, CAT_FUR_COUNT } cat_fur_t;

#define CAT_SPRITE_W 32
#define CAT_SPRITE_H 32
#define CAT_PROP_W 16
#define CAT_PROP_H 16

/* Palette slots (same meaning for every fur). 0 = transparent.
 * 1..8 depend on the fur, 9..15 are identical in all palettes (props use only these). */
enum {
    CAT_PAL_TRANSPARENT = 0, CAT_PAL_OUTLINE, CAT_PAL_FUR, CAT_PAL_FUR_SHADE, CAT_PAL_STRIPE,
    CAT_PAL_PATCH, CAT_PAL_PATCH2, CAT_PAL_LIGHT, CAT_PAL_EYE,
    CAT_PAL_WHITE, CAT_PAL_PINK, CAT_PAL_DARK, CAT_PAL_BLUE, CAT_PAL_RED, CAT_PAL_YELLOW, CAT_PAL_BROWN,
};

/* 4-bit palette indices, packed 2 pixels per byte, high nibble = left pixel, row-major. 0 = transparent.
 * Out-of-range arguments are clamped (never returns NULL).
 *
 * Expanding into an LVGL 9 RGB565A8 image (colour plane w*h*2 bytes, then alpha plane w*h bytes),
 * optionally scaled by an integer factor with nearest-neighbour:
 *
 *   static void cat_expand(const uint8_t *src, int w, int h, const uint16_t *pal,
 *                          int scale, uint8_t *dst)            // dst: (w*s)*(h*s)*3 bytes
 *   {
 *       int dw = w * scale, dh = h * scale;
 *       uint16_t *rgb = (uint16_t *)dst;
 *       uint8_t  *a   = dst + dw * dh * 2;
 *       for (int y = 0; y < dh; y++)
 *           for (int x = 0; x < dw; x++) {
 *               int sx = x / scale, sy = y / scale, i = sy * w + sx;
 *               uint8_t v = src[i >> 1];
 *               uint8_t idx = (i & 1) ? (v & 0x0F) : (v >> 4);
 *               rgb[y * dw + x] = idx ? pal[idx] : 0;
 *               a[y * dw + x]   = idx ? 0xFF : 0x00;
 *           }
 *   }
 *
 *   // 32x32 cat at x3 -> 96x96:  static uint8_t buf[96*96*3];
 *   cat_expand(cat_sprite(CAT_POSE_SIT), CAT_SPRITE_W, CAT_SPRITE_H,
 *              cat_palette(CAT_FUR_TABBY), 3, buf);
 *   lv_image_dsc_t dsc = { .header.magic = LV_IMAGE_HEADER_MAGIC, .header.cf = LV_COLOR_FORMAT_RGB565A8,
 *                          .header.w = 96, .header.h = 96, .header.stride = 96 * 2,
 *                          .data = buf, .data_size = sizeof(buf) };
 *   lv_image_set_src(img, &dsc);   // call lv_image_cache_drop(&dsc) / lv_obj_invalidate after re-expanding
 *
 * If your panel needs byte-swapped RGB565, swap in the flush callback (as usual), not here.
 */
const uint8_t *cat_sprite(cat_pose_t pose);                   /* 32*32/2 = 512 bytes */
const uint8_t *cat_prop(cat_prop_t prop);                     /* 16*16/2 = 128 bytes */
const uint16_t *cat_palette(cat_fur_t fur);                   /* 16 RGB565 colours (index 0 unused) */
cat_fur_t cat_fur_from_name(const char *name);                /* "tabby","orange","black","white","calico","gray"; unknown -> tabby */
const uint16_t *cat_theme_palette(const char *name);          /* "neon","mono","sumi"; NULL if unknown (use the fur palette) */

#ifdef __cplusplus
}
#endif
'''


def gen_c():
    os.makedirs(os.path.join(COMP_DIR, "include"), exist_ok=True)
    with open(os.path.join(COMP_DIR, "CMakeLists.txt"), "w", newline="\n") as f:
        f.write('idf_component_register(SRCS "cat_sprites.c" INCLUDE_DIRS "include")\n')
    with open(os.path.join(COMP_DIR, "include", "cat_sprites.h"), "w", newline="\n") as f:
        def enum(kind, names):
            return ("typedef enum { " + ", ".join(f"CAT_{kind}_{n}" for n in [*names, "COUNT"]) +
                    " } cat_" + kind.lower() + "_t;")
        f.write(HEADER.replace("@POSE_ENUM@", enum("POSE", POSES)).replace("@PROP_ENUM@", enum("PROP", PROP_NAMES)))
    o = []
    o.append("/* AUTO-GENERATED by firmware/tools/cat_sprites/sprites.py -- do not edit by hand. */")
    o.append('#include "cat_sprites.h"')
    o.append("#include <string.h>")
    o.append("")
    o.append("static const uint8_t s_sprites[CAT_POSE_COUNT][CAT_SPRITE_W * CAT_SPRITE_H / 2] = {")
    for p in POSES:
        o.append(f"    [CAT_POSE_{p}] = {{")
        o.append(_c_bytes(pack(SPRITES[p]), "        "))
        o.append("    },")
    o.append("};")
    o.append("")
    o.append("static const uint8_t s_props[CAT_PROP_COUNT][CAT_PROP_W * CAT_PROP_H / 2] = {")
    for n in PROP_NAMES:
        o.append(f"    [CAT_PROP_{n}] = {{")
        o.append(_c_bytes(pack(PROPS[n]), "        "))
        o.append("    },")
    o.append("};")
    o.append("")
    o.append("static const uint16_t s_palettes[CAT_FUR_COUNT][16] = {")
    for f in FURS:
        cols = ", ".join(f"0x{rgb565(c):04X}" for c in PALETTES[f])
        o.append(f"    [CAT_FUR_{f.upper()}] = {{ {cols} }},")
    o.append("};")
    o.append("")
    o.append("static const struct { const char *name; uint16_t pal[16]; } s_theme_pals[] = {")
    for name, cols in THEME_PALETTES.items():
        o.append(f'    {{"{name}", {{ ' + ", ".join(f"0x{rgb565(c):04X}" for c in cols) + " }},")
    o.append("};")
    o.append("")
    o.append("static const char *const s_fur_names[CAT_FUR_COUNT] = {")
    o.append("    " + ", ".join(f'"{f}"' for f in FURS) + ",")
    o.append("};")
    o.append("""
const uint8_t *cat_sprite(cat_pose_t pose)
{
    if ((unsigned)pose >= CAT_POSE_COUNT) pose = CAT_POSE_SIT;
    return s_sprites[pose];
}

const uint8_t *cat_prop(cat_prop_t prop)
{
    if ((unsigned)prop >= CAT_PROP_COUNT) prop = CAT_PROP_ZZZ;
    return s_props[prop];
}

const uint16_t *cat_palette(cat_fur_t fur)
{
    if ((unsigned)fur >= CAT_FUR_COUNT) fur = CAT_FUR_TABBY;
    return s_palettes[fur];
}

const uint16_t *cat_theme_palette(const char *name)
{
    for (size_t i = 0; name && i < sizeof(s_theme_pals) / sizeof(s_theme_pals[0]); i++)
        if (strcmp(name, s_theme_pals[i].name) == 0) return s_theme_pals[i].pal;
    return NULL;
}

cat_fur_t cat_fur_from_name(const char *name)
{
    if (name) {
        for (int i = 0; i < CAT_FUR_COUNT; i++) {
            if (strcmp(name, s_fur_names[i]) == 0) return (cat_fur_t)i;
        }
    }
    return CAT_FUR_TABBY;
}
""")
    with open(os.path.join(COMP_DIR, "cat_sprites.c"), "w", newline="\n") as f:
        f.write("\n".join(o))
    total = len(POSES) * 512 + len(PROP_NAMES) * 128 + len(FURS) * 32
    print(f"wrote {COMP_DIR} (const data ~{total} bytes)")


# ---------------------------------------------------------------------- preview
BG = (0x0E, 0x11, 0x16)


def _rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


def render(rows, fur, scale):
    from PIL import Image
    pal = [_rgb(c) for c in PALETTES[fur]]
    im = Image.new("RGB", (len(rows[0]), len(rows)), BG)
    px = im.load()
    for y, r in enumerate(rows):
        for x, c in enumerate(r):
            if c != ".":
                px[x, y] = pal[CHARS.index(c)]
    return im.resize((im.width * scale, im.height * scale), Image.NEAREST)


def preview(scale=4):
    from PIL import Image, ImageDraw
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    pad, lab = 6, 14
    cw = 32 * scale + pad
    label_w = 60

    def fur_sheet(fur):
        im = Image.new("RGB", (label_w + cw * len(POSES), lab + cw), BG)
        d = ImageDraw.Draw(im)
        d.text((4, lab + cw // 2 - 6), fur, fill=(150, 160, 175))
        for pi, p in enumerate(POSES):
            d.text((label_w + pi * cw + pad, 2), p, fill=(150, 160, 175))
            im.paste(render(SPRITES[p], fur, scale), (label_w + pi * cw + pad // 2, lab + pad // 2))
        return im

    sheets = []
    for fur in FURS:
        s = fur_sheet(fur)
        s.save(os.path.join(PREVIEW_DIR, f"cat_{fur}.png"))
        sheets.append((fur, s))
    # everything in one image
    hdr = 16
    big = Image.new("RGB", (sheets[0][1].width, sum(s.height + hdr for _, s in sheets)), BG)
    d = ImageDraw.Draw(big)
    y = 0
    for fur, s in sheets:
        d.text((4, y + 2), fur.upper(), fill=(230, 200, 120))
        big.paste(s, (0, y + hdr))
        y += s.height + hdr
    big.save(os.path.join(PREVIEW_DIR, "all.png"))
    # props
    pw = 16 * scale * 2 + pad
    im = Image.new("RGB", (pw * len(PROP_NAMES), 16 * scale * 2 + pad + lab), BG)
    d = ImageDraw.Draw(im)
    for i, n in enumerate(PROP_NAMES):
        d.text((i * pw + pad, 2), n, fill=(150, 160, 175))
        im.paste(render(PROPS[n], "tabby", scale * 2), (i * pw + pad // 2, lab + pad // 2))
    im.save(os.path.join(PREVIEW_DIR, "props.png"))
    # on-device look: 296x240 screen, cat x3 + a few props x2
    scr = Image.new("RGB", (296 * 2, 240 * 2), BG)
    demo = [("CODE1", "tabby"), ("WAIT", "calico"), ("THINK", "black")]
    for i, (p, f) in enumerate(demo):
        scr.paste(render(SPRITES[p], f, 6), (8 + i * 196, 120))
    for i, n in enumerate(PROP_NAMES):
        scr.paste(render(PROPS[n], "tabby", 4), (20 + i * 92, 380))
    scr.save(os.path.join(PREVIEW_DIR, "screen_mock.png"))
    print(f"wrote previews to {PREVIEW_DIR}")


def dump(pose):
    for r in SPRITES[pose.upper()]:
        print(f'        "{r}",')


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "all"
    validate()
    if cmd in ("all", "gen"):
        gen_c()
    if cmd in ("all", "preview"):
        preview()
    if cmd == "dump":
        dump(sys.argv[2])
    if cmd == "check":
        print("ok")
