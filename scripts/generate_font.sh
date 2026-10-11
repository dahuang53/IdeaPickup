#!/bin/sh
# Optional asset regeneration; normal ESP-IDF builds use checked-in memo_font.c.
set -eu
cd "$(dirname "$0")/.."
mkdir -p .tools/ui-font
font=.tools/ui-font/NotoSansCJKsc-Regular.otf
expected=2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b
if [ ! -f "$font" ]; then
    curl -fL --retry 2 \
        'https://raw.githubusercontent.com/notofonts/noto-cjk/Serif2.003/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf' \
        -o "$font"
fi
actual=$(shasum -a 256 "$font" | cut -d ' ' -f 1)
if [ "$actual" != "$expected" ]; then
    echo "Noto font SHA256 mismatch; refusing to regenerate" >&2
    exit 1
fi
npm ci --prefix assets/fonts --cache .tools/npm-cache --ignore-scripts --no-audit --no-fund
symbols=$(python3 -c 'from pathlib import Path; import re; paths=[Path("main/ui.c"),Path("main/app.c"),Path("main/memo_cardless.cpp")]; text="".join("".join(re.findall(r"\"((?:[^\"\\]|\\.)*)\"",p.read_text())) for p in paths if p.exists()); print("".join(sorted(set(c for c in text if ord(c)>127))))')
node assets/fonts/node_modules/lv_font_conv/lv_font_conv.js \
    --font "$font" --range 0x20-0x7e --symbols "$symbols" \
    --size 18 --bpp 4 --format lvgl --no-compress --no-prefilter \
    --lv-font-name memo_font --lv-include lvgl.h --output main/memo_font.c
node assets/fonts/node_modules/lv_font_conv/lv_font_conv.js \
    --font "$font" --range 0x20-0x7e --symbols "$symbols" \
    --size 20 --bpp 4 --format lvgl --no-compress --no-prefilter \
    --lv-font-name memo_font20 --lv-include lvgl.h --output main/memo_font20.c
node assets/fonts/node_modules/lv_font_conv/lv_font_conv.js \
    --font "$font" --range 0x20-0x7e --symbols '播放录音暂停继续静麦克风取消' \
    --size 22 --bpp 4 --format lvgl --no-compress --no-prefilter \
    --lv-font-name memo_font22 --lv-include lvgl.h --output main/memo_font22.c
node assets/fonts/node_modules/lv_font_conv/lv_font_conv.js \
    --font "$font" --range 0x20-0x7e --symbols '录音无卡' \
    --size 24 --bpp 4 --format lvgl --no-compress --no-prefilter \
    --lv-font-name memo_font24 --lv-include lvgl.h --output main/memo_font24.c
shasum -a 256 main/memo_font.c main/memo_font20.c main/memo_font22.c main/memo_font24.c
