# Regenerates the MeowBox LVGL fonts into firmware/components/fonts/.
#
#   cd firmware/tools/fonts
#   npm install            # installs lv_font_conv 1.5.3 locally (package.json)
#   python -m pip install --user fonttools brotli   # for the theme fonts
#   ./gen_fonts.ps1             # default fonts + all theme fonts (LVGL + web)
#   ./gen_fonts.ps1 -NoTheme    # only the default mb_font_* set
#
# Sources (in ./src):
#   BeVietnamPro-*.ttf                  - google/fonts ofl/bevietnampro, SIL OFL 1.1 (OFL.txt)
#   FontAwesome5-Solid+Brands+Regular.woff - lvgl/lvgl v9.5.0 scripts/built_in_font (LV_SYMBOL_* glyphs)
#
# Theme fonts (mb_sw_*, mb_cy_*, mb_zen_*, mb_br_* and storage/web/fonts/*.woff2 + fonts.json)
# are produced by gen_theme_fonts.py (sources/licences in src/<family>/, downloaded on demand).
#
# Fonts are generated uncompressed (--no-compress) so they work without LV_USE_FONT_COMPRESSED.
param([switch]$NoTheme)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
Push-Location $here   # relative paths keep the "Opts:" header in the .c files machine-independent
$out  = '../../components/fonts'
$conv = Join-Path $here 'node_modules\.bin\lv_font_conv.cmd'
if (-not (Test-Path $conv)) { npm install }

$regular = 'src/BeVietnamPro-Regular.ttf'
$bold    = 'src/BeVietnamPro-Bold.ttf'
$fa      = 'src/FontAwesome5-Solid+Brands+Regular.woff'

# Basic Latin, Latin-1, Vietnamese letters outside Latin-1 (Ăă Đđ Ĩĩ Ũũ Ơơ Ưư),
# Latin Extended Additional (Vietnamese tone-marked letters), and punctuation:
# ‘ ’ “ ” – — … • €   (° is U+00B0, already in Latin-1)
$textRange = '0x20-0x7E,0xA0-0xFF,0x102-0x103,0x110-0x111,0x128-0x129,0x168-0x169,0x1A0-0x1A1,0x1AF-0x1B0,0x1EA0-0x1EF9,0x2013-0x2014,0x2018-0x2019,0x201C-0x201D,0x2022,0x2026,0x20AC'

# LV_SYMBOL_* code points, same list as lvgl v9.5.0 scripts/built_in_font/built_in_font_gen.py
$symRange = '61441,61448,61451,61452,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650'

function New-Font([string]$name, [int]$size, [string[]]$fontArgs) {
    $file = "$out/$name.c"
    $argList = @('--bpp', '4', '--size', "$size", '--format', 'lvgl', '--no-compress',
                 '--lv-include', 'lvgl.h', '--lv-font-name', $name, '-o', $file) + $fontArgs
    & $conv @argList
    if ($LASTEXITCODE -ne 0) { throw "lv_font_conv failed for $name" }
    Write-Host ("{0,-22} {1,8:N0} bytes .c" -f "$name.c", (Get-Item $file).Length)
}

New-Item -ItemType Directory -Force $out | Out-Null

New-Font 'mb_font_14'      14 @('--font', $regular, '-r', $textRange, '--font', $fa, '-r', $symRange)
New-Font 'mb_font_20'      20 @('--font', $regular, '-r', $textRange, '--font', $fa, '-r', $symRange)
New-Font 'mb_font_20_bold' 20 @('--font', $bold,    '-r', $textRange)
# Clock digits: 0-9 : space - °
New-Font 'mb_font_clock_64' 64 @('--font', $bold, '--symbols', '0123456789: -', '-r', '0xB0')

if (-not $NoTheme) {
    python -P gen_theme_fonts.py
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw 'gen_theme_fonts.py failed' }
}

Pop-Location
