#!/usr/bin/env python3
"""Generate the per-theme MeowBox fonts (LVGL C fonts + web woff2 subsets).

    cd firmware/tools/fonts
    npm install                           # lv_font_conv 1.5.3 (package.json)
    python -m pip install --user fonttools brotli
    python gen_theme_fonts.py             # everything
    python gen_theme_fonts.py --lvgl      # only components/fonts/mb_<theme>_*.c
    python gen_theme_fonts.py --web       # only web/assets/fonts/*.woff2 + fonts.json
    python gen_theme_fonts.py --agent     # only pc-agent/assets/fonts/*.ttf (the agent window's display fonts)

Called from gen_fonts.ps1 as well. Sources are downloaded from the google/fonts
GitHub repo into ./src/<family>/ (with their OFL.txt) if they are missing.
Variable fonts are pinned to static instances in ./src/static/ (generated,
git-ignored) with fontTools.varLib.instancer, because lv_font_conv only renders
the default instance of a variable font.

Every LVGL text font is checked (fontTools cmap) against the Vietnamese text
range; code points the theme font lacks are merged from Be Vietnam Pro at the
same size (lv_font_conv: a later --font/-r pair overrides earlier ones) so the
firmware never shows tofu. The script prints what it had to merge.
"""
import argparse
import json
import os
import subprocess
import sys
import urllib.request

from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_C = os.path.normpath(os.path.join(HERE, '..', '..', 'components', 'fonts'))
OUT_WEB = os.path.normpath(os.path.join(HERE, '..', '..', 'web', 'assets', 'fonts'))
OUT_AGENT = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'pc-agent', 'assets', 'fonts'))
GF = 'https://raw.githubusercontent.com/google/fonts/main/'

# local path (relative to HERE) -> download URL
SOURCES = {
    'src/archivo/Archivo[wdth,wght].ttf': GF + 'ofl/archivo/Archivo%5Bwdth,wght%5D.ttf',
    'src/archivo/OFL.txt': GF + 'ofl/archivo/OFL.txt',
    'src/chakrapetch/ChakraPetch-Regular.ttf': GF + 'ofl/chakrapetch/ChakraPetch-Regular.ttf',
    'src/chakrapetch/ChakraPetch-Medium.ttf': GF + 'ofl/chakrapetch/ChakraPetch-Medium.ttf',
    'src/chakrapetch/ChakraPetch-SemiBold.ttf': GF + 'ofl/chakrapetch/ChakraPetch-SemiBold.ttf',
    'src/chakrapetch/ChakraPetch-Bold.ttf': GF + 'ofl/chakrapetch/ChakraPetch-Bold.ttf',
    'src/chakrapetch/OFL.txt': GF + 'ofl/chakrapetch/OFL.txt',
    'src/vt323/VT323-Regular.ttf': GF + 'ofl/vt323/VT323-Regular.ttf',
    'src/vt323/OFL.txt': GF + 'ofl/vt323/OFL.txt',
    'src/BeVietnamPro-Light.ttf': GF + 'ofl/bevietnampro/BeVietnamPro-Light.ttf',
    'src/BeVietnamPro-Regular.ttf': GF + 'ofl/bevietnampro/BeVietnamPro-Regular.ttf',
    'src/BeVietnamPro-SemiBold.ttf': GF + 'ofl/bevietnampro/BeVietnamPro-SemiBold.ttf',
    'src/BeVietnamPro-Bold.ttf': GF + 'ofl/bevietnampro/BeVietnamPro-Bold.ttf',
    'src/OFL.txt': GF + 'ofl/bevietnampro/OFL.txt',
    'src/cormorantgaramond/CormorantGaramond[wght].ttf': GF + 'ofl/cormorantgaramond/CormorantGaramond%5Bwght%5D.ttf',
    'src/cormorantgaramond/CormorantGaramond-Italic[wght].ttf': GF + 'ofl/cormorantgaramond/CormorantGaramond-Italic%5Bwght%5D.ttf',
    'src/cormorantgaramond/OFL.txt': GF + 'ofl/cormorantgaramond/OFL.txt',
    'src/notoserifjp/NotoSerifJP[wght].ttf': GF + 'ofl/notoserifjp/NotoSerifJP%5Bwght%5D.ttf',
    'src/notoserifjp/OFL.txt': GF + 'ofl/notoserifjp/OFL.txt',
    'src/ibmplexmono/IBMPlexMono-Regular.ttf': GF + 'ofl/ibmplexmono/IBMPlexMono-Regular.ttf',
    'src/ibmplexmono/OFL.txt': GF + 'ofl/ibmplexmono/OFL.txt',
    # Tinos moved from apache/ to ofl/ in google/fonts (2026, v1.340, now SIL OFL 1.1)
    'src/tinos/Tinos-Regular.ttf': GF + 'ofl/tinos/Tinos-Regular.ttf',
    'src/tinos/Tinos-Bold.ttf': GF + 'ofl/tinos/Tinos-Bold.ttf',
    'src/tinos/OFL.txt': 'https://raw.githubusercontent.com/googlefonts/tinos/main/OFL.txt',
    'src/jetbrainsmono/JetBrainsMono[wght].ttf': GF + 'ofl/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf',
    'src/jetbrainsmono/OFL.txt': GF + 'ofl/jetbrainsmono/OFL.txt',
}

# static instance (relative to HERE) -> (variable source, axis location, optional code point subset)
INSTANCES = {
    'src/static/Archivo-Regular.ttf':   ('src/archivo/Archivo[wdth,wght].ttf', {'wdth': 100, 'wght': 400}, None),
    'src/static/Archivo-Bold.ttf':      ('src/archivo/Archivo[wdth,wght].ttf', {'wdth': 100, 'wght': 700}, None),
    'src/static/Archivo-ExtraBold-wdth72.ttf': ('src/archivo/Archivo[wdth,wght].ttf', {'wdth': 72, 'wght': 800}, None),
    'src/static/CormorantGaramond-Light.ttf':  ('src/cormorantgaramond/CormorantGaramond[wght].ttf', {'wght': 300}, None),
    'src/static/CormorantGaramond-Medium.ttf': ('src/cormorantgaramond/CormorantGaramond[wght].ttf', {'wght': 500}, None),
    'src/static/CormorantGaramond-SemiBoldItalic.ttf': ('src/cormorantgaramond/CormorantGaramond-Italic[wght].ttf', {'wght': 600}, None),
    'src/static/CormorantGaramond-SemiBold.ttf': ('src/cormorantgaramond/CormorantGaramond[wght].ttf', {'wght': 600}, None),
    # only U+732B (猫) is needed; subset first, the full CJK font is 13 MB
    'src/static/NotoSerifJP-SemiBold-732B.ttf': ('src/notoserifjp/NotoSerifJP[wght].ttf', {'wght': 600}, [0x732B]),
    'src/static/JetBrainsMono-Regular.ttf': ('src/jetbrainsmono/JetBrainsMono[wght].ttf', {'wght': 400}, None),
}

FA = 'src/FontAwesome5-Solid+Brands+Regular.woff'
BVP_FALLBACK = {'light': 'src/BeVietnamPro-Light.ttf', 'regular': 'src/BeVietnamPro-Regular.ttf',
                'medium': 'src/BeVietnamPro-Regular.ttf', 'bold': 'src/BeVietnamPro-Bold.ttf'}

# Same ranges as mb_font_14 (gen_fonts.ps1)
TEXT_RANGE = ('0x20-0x7E,0xA0-0xFF,0x102-0x103,0x110-0x111,0x128-0x129,0x168-0x169,0x1A0-0x1A1,'
              '0x1AF-0x1B0,0x1EA0-0x1EF9,0x2013-0x2014,0x2018-0x2019,0x201C-0x201D,0x2022,0x2026,0x20AC')
# LV_SYMBOL_* code points, same list as lvgl v9.5.0 scripts/built_in_font/built_in_font_gen.py
SYM_RANGE = ('61441,61448,61451,61452,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,'
             '61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,'
             '61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,'
             '61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650')
CLOCK_SYMBOLS = '0123456789: -°'

# name, size, source, fallback weight, kind ('text' | 'text+sym' | 'clock'), extra [(font, range)]
LVGL_FONTS = [
    ('mb_sw_14',     14, 'src/static/Archivo-Regular.ttf',              'regular', 'text+sym', []),
    ('mb_sw_20',     20, 'src/static/Archivo-Regular.ttf',              'regular', 'text+sym', []),
    ('mb_sw_20b',    20, 'src/static/Archivo-Bold.ttf',                 'bold',    'text',     []),
    ('mb_sw_clock',  96, 'src/static/Archivo-ExtraBold-wdth72.ttf',     'bold',    'clock',    []),
    ('mb_cy_14',     14, 'src/chakrapetch/ChakraPetch-Regular.ttf',     'regular', 'text+sym', []),
    ('mb_cy_20',     20, 'src/chakrapetch/ChakraPetch-Medium.ttf',      'medium',  'text+sym', []),
    ('mb_cy_20b',    20, 'src/chakrapetch/ChakraPetch-Bold.ttf',        'bold',    'text',     []),
    ('mb_cy_clock',  92, 'src/vt323/VT323-Regular.ttf',                 'regular', 'clock',    []),
    ('mb_zen_14',    14, 'src/BeVietnamPro-Light.ttf',                  'light',   'text+sym', []),
    ('mb_zen_20',    20, 'src/static/CormorantGaramond-Medium.ttf',     'medium',  'text+sym',
        [('src/static/NotoSerifJP-SemiBold-732B.ttf', '0x732B')]),
    ('mb_zen_20b',   22, 'src/static/CormorantGaramond-SemiBoldItalic.ttf', 'bold', 'text',    []),
    ('mb_zen_clock', 56, 'src/static/CormorantGaramond-Light.ttf',      'light',   'clock',    []),
    ('mb_br_14',     14, 'src/ibmplexmono/IBMPlexMono-Regular.ttf',     'regular', 'text+sym', []),
    ('mb_br_20',     20, 'src/tinos/Tinos-Regular.ttf',                 'regular', 'text+sym', []),
    ('mb_br_20b',    20, 'src/tinos/Tinos-Bold.ttf',                    'bold',    'text',     []),
    ('mb_br_clock',  84, 'src/tinos/Tinos-Bold.ttf',                    'bold',    'clock',    []),
]

# Web subsets (pyftsubset-equivalent, woff2)
WEB_UNICODES = ('U+0020-007E,U+00A0-00FF,U+0102-0103,U+0110-0111,U+0128-0129,U+0168-0169,U+01A0-01A1,'
                'U+01AF-01B0,U+1EA0-1EF9,U+2013-2014,U+2018-201D,U+2026,U+00B0')
WEB_DIGITS = 'U+0020-007E,U+00B0'
# file -> (source, unicodes, fonts.json entry); variable axis limits per file in WEB_AXES
WEB_AXES = {
    'archivo-vi.woff2': {'wdth': (62, 100), 'wght': (400, 800)},
    'jetbrains-mono-vi.woff2': {'wght': (400, 700)},
}
WEB_FONTS = {
    # variable, axes limited to what the Swiss theme uses (body wdth 100, display wdth 72; 400..800)
    'archivo-vi.woff2': ('src/archivo/Archivo[wdth,wght].ttf', WEB_UNICODES,
                         {'family': 'Archivo', 'weight': '400 800', 'style': 'normal', 'stretch': '62% 100%'}),
    'chakra-petch-400-vi.woff2': ('src/chakrapetch/ChakraPetch-Regular.ttf', WEB_UNICODES,
                                  {'family': 'Chakra Petch', 'weight': '400', 'style': 'normal'}),
    'chakra-petch-600-vi.woff2': ('src/chakrapetch/ChakraPetch-SemiBold.ttf', WEB_UNICODES,
                                  {'family': 'Chakra Petch', 'weight': '600', 'style': 'normal'}),
    'chakra-petch-700-vi.woff2': ('src/chakrapetch/ChakraPetch-Bold.ttf', WEB_UNICODES,
                                  {'family': 'Chakra Petch', 'weight': '700', 'style': 'normal'}),
    'jetbrains-mono-vi.woff2': ('src/jetbrainsmono/JetBrainsMono[wght].ttf', WEB_UNICODES,
                                {'family': 'JetBrains Mono', 'weight': '400 700', 'style': 'normal'}),
    'vt323-digits.woff2': ('src/vt323/VT323-Regular.ttf', WEB_DIGITS,
                           {'family': 'VT323', 'weight': '400', 'style': 'normal'}),
    'cormorant-garamond-300-vi.woff2': ('src/static/CormorantGaramond-Light.ttf', WEB_UNICODES,
                                        {'family': 'Cormorant Garamond', 'weight': '300', 'style': 'normal'}),
    'cormorant-garamond-500-vi.woff2': ('src/static/CormorantGaramond-Medium.ttf', WEB_UNICODES,
                                        {'family': 'Cormorant Garamond', 'weight': '500', 'style': 'normal'}),
    'cormorant-garamond-600-italic-vi.woff2': ('src/static/CormorantGaramond-SemiBoldItalic.ttf', WEB_UNICODES,
                                               {'family': 'Cormorant Garamond', 'weight': '600', 'style': 'italic'}),
    'be-vietnam-pro-light-vi.woff2': ('src/BeVietnamPro-Light.ttf', WEB_UNICODES,
                                      {'family': 'Be Vietnam Pro', 'weight': '300', 'style': 'normal'}),
}


# PC agent window (docs/enhance_agent.md, B1): each theme's display font for headings and the speaker mirror's big
# numbers, as static TTF (egui reads TrueType outlines only), Vietnamese subset, embedded in MeowBoxAgent.exe.
# File names = "agent.display_font" in themes/<id>.json; Brutalism uses Windows' own Times New Roman Bold.
AGENT_FONTS = {
    'archivo-display.ttf': 'src/static/Archivo-ExtraBold-wdth72.ttf',
    'chakra-petch-display.ttf': 'src/chakrapetch/ChakraPetch-Bold.ttf',
    'cormorant-display.ttf': 'src/static/CormorantGaramond-SemiBold.ttf',
}
AGENT_LICENSES = {'Archivo': 'src/archivo/OFL.txt', 'ChakraPetch': 'src/chakrapetch/OFL.txt',
                  'CormorantGaramond': 'src/cormorantgaramond/OFL.txt'}
AGENT_BUDGET = 150 * 1024


def p(rel):
    return os.path.join(HERE, rel)


def parse_ranges(spec):
    cps = []
    for part in spec.replace('U+', '0x').split(','):
        if '-' in part:
            a, b = part.split('-')
            if not b.startswith('0x'):
                b = '0x' + b
            cps.extend(range(int(a, 0), int(b, 0) + 1))
        else:
            cps.append(int(part, 0))
    return cps


def fmt_ranges(cps):
    cps = sorted(set(cps))
    out, i = [], 0
    while i < len(cps):
        j = i
        while j + 1 < len(cps) and cps[j + 1] == cps[j] + 1:
            j += 1
        out.append('0x%X' % cps[i] if i == j else '0x%X-0x%X' % (cps[i], cps[j]))
        i = j + 1
    return ','.join(out)


def ensure_sources():
    for rel, url in SOURCES.items():
        if not os.path.exists(p(rel)):
            os.makedirs(os.path.dirname(p(rel)), exist_ok=True)
            print('download', rel)
            urllib.request.urlretrieve(url, p(rel))


def ensure_instances():
    os.makedirs(p('src/static'), exist_ok=True)
    for rel, (src, loc, only) in INSTANCES.items():
        if os.path.exists(p(rel)) and os.path.getmtime(p(rel)) >= os.path.getmtime(p(src)):
            continue
        print('instance', rel, loc)
        f = TTFont(p(src))
        if only:
            opts = subset.Options()
            opts.layout_features = []
            opts.name_IDs = ['*']
            opts.notdef_outline = True
            s = subset.Subsetter(opts)
            s.populate(unicodes=only)
            s.subset(f)
        f = instancer.instantiateVariableFont(f, loc)
        f.save(p(rel))


_cmaps = {}


def cmap(rel):
    if rel not in _cmaps:
        _cmaps[rel] = set(TTFont(p(rel)).getBestCmap().keys())
    return _cmaps[rel]


def gen_lvgl():
    if sys.platform == 'win32':
        conv = p('node_modules/.bin/lv_font_conv.cmd')
    else:
        conv = p('node_modules/.bin/lv_font_conv')
    if not os.path.exists(conv):
        subprocess.check_call('npm install', shell=True, cwd=HERE)
    out_rel = '../../components/fonts'   # relative: keeps the "Opts:" header machine-independent
    report = []
    for name, size, src, fb_weight, kind, extra in LVGL_FONTS:
        if kind == 'clock':
            need = [ord(c) for c in CLOCK_SYMBOLS]
            font_args = ['--font', src, '--symbols', CLOCK_SYMBOLS.replace('°', ''), '-r', '0xB0']
        else:
            need = parse_ranges(TEXT_RANGE)
            font_args = ['--font', src, '-r', TEXT_RANGE]
        missing = [c for c in need if c not in cmap(src)]
        if missing:
            fb = BVP_FALLBACK[fb_weight]
            still = [c for c in missing if c not in cmap(fb)]
            if still:
                raise SystemExit('%s: %s missing from both %s and %s' % (name, fmt_ranges(still), src, fb))
            font_args += ['--font', fb, '-r', fmt_ranges(missing)]
            report.append('%s: %d code points missing in %s, merged from %s: %s'
                          % (name, len(missing), src, fb, fmt_ranges(missing)))
        for xf, xr in extra:
            miss = [c for c in parse_ranges(xr) if c not in cmap(xf)]
            if miss:
                raise SystemExit('%s: %s missing from %s' % (name, fmt_ranges(miss), xf))
            font_args += ['--font', xf, '-r', xr]
        if kind == 'text+sym':
            font_args += ['--font', FA, '-r', SYM_RANGE]
        args = [conv, '--bpp', '4', '--size', str(size), '--format', 'lvgl', '--no-compress',
                '--lv-include', 'lvgl.h', '--lv-font-name', name, '-o', '%s/%s.c' % (out_rel, name)] + font_args
        subprocess.check_call(args, cwd=HERE, shell=False)
        print('%-16s %9d bytes .c' % (name + '.c', os.path.getsize(os.path.join(OUT_C, name + '.c'))))
    print('\nGlyph coverage:')
    print('\n'.join(report) if report else '  all requested code points present in the theme fonts')


def gen_web():
    os.makedirs(OUT_WEB, exist_ok=True)
    meta = {}
    total = 0
    for fname, (src, uni, entry) in WEB_FONTS.items():
        want = parse_ranges(uni)
        missing = [c for c in want if c not in cmap(src)]
        if missing:
            print('  note: %s lacks %s (browser falls back)' % (fname, fmt_ranges(missing)))
        opts = subset.Options()
        opts.flavor = 'woff2'
        opts.name_IDs = ['*']
        opts.name_languages = ['*']
        opts.notdef_outline = True
        opts.hinting = False
        opts.desubroutinize = True
        f = TTFont(p(src))
        s = subset.Subsetter(opts)
        s.populate(unicodes=want)
        s.subset(f)
        if fname in WEB_AXES:   # subset first, then limit the axes (L3 instancing)
            f = instancer.instantiateVariableFont(f, WEB_AXES[fname])
        out = os.path.join(OUT_WEB, fname)
        subset.save_font(f, out, opts)
        size = os.path.getsize(out)
        total += size
        meta[fname] = entry
        print('%-40s %8d bytes' % (fname, size))
    print('%-40s %8d bytes' % ('total', total))
    with open(os.path.join(OUT_WEB, 'fonts.json'), 'w', encoding='utf-8', newline='\n') as fp:
        json.dump(meta, fp, indent=2, ensure_ascii=False)
        fp.write('\n')


def gen_agent():
    os.makedirs(OUT_AGENT, exist_ok=True)
    want = parse_ranges(WEB_UNICODES)
    for fname, src in AGENT_FONTS.items():
        missing = [c for c in want if c not in cmap(src)]
        if missing:
            print('  note: %s lacks %s (Segoe UI fills in)' % (fname, fmt_ranges(missing)))
        opts = subset.Options()
        opts.name_IDs = ['*']
        opts.notdef_outline = True
        opts.hinting = False
        opts.layout_features = ['kern', 'liga', 'ccmp', 'mark', 'mkmk']
        f = TTFont(p(src))
        s = subset.Subsetter(opts)
        s.populate(unicodes=want)
        s.subset(f)
        out = os.path.join(OUT_AGENT, fname)
        subset.save_font(f, out, opts)
        size = os.path.getsize(out)
        print('%-40s %8d bytes' % (fname, size))
        if size > AGENT_BUDGET:
            raise SystemExit('%s is over the %d KB budget' % (fname, AGENT_BUDGET // 1024))
    # the SIL OFL travels with the fonts
    for family, lic in AGENT_LICENSES.items():
        with open(p(lic), 'rb') as src, open(os.path.join(OUT_AGENT, 'OFL-%s.txt' % family), 'wb') as dst:
            dst.write(src.read().replace(b'\r\n', b'\n'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--lvgl', action='store_true')
    ap.add_argument('--web', action='store_true')
    ap.add_argument('--agent', action='store_true')
    a = ap.parse_args()
    both = not (a.lvgl or a.web or a.agent)
    ensure_sources()
    ensure_instances()
    if a.lvgl or both:
        gen_lvgl()
    if a.web or both:
        gen_web()
    if a.agent or both:
        gen_agent()


if __name__ == '__main__':
    main()
