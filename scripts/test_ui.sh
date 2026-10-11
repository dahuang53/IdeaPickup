#!/bin/sh
# Headless rendering of production ui.c with host-only app_snapshot/command stubs.
set -eu
cd "$(dirname "$0")/.."
project_root=$(pwd)
lvgl_source="$project_root/managed_components/lvgl__lvgl"
if [ ! -f "$lvgl_source/lvgl.h" ]; then
    echo 'First run the documented IDF build to obtain locked LVGL 9.3.0.' >&2
    exit 1
fi
cmake_tool=${CMAKE:-.venv/bin/cmake}
if [ ! -x "$cmake_tool" ]; then cmake_tool=cmake; fi
ui_output=${MEMO_UI_OUTPUT:-evidence}
ui_build=${MEMO_UI_BUILD:-.tools/ui-preview-build}
mkdir -p .tools/ui-preview-project "$ui_output"
export MEMO_UI_OUTPUT="$ui_output"
cat > .tools/ui-preview-project/lv_conf.h <<'EOF'
#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 32
#define LV_MEM_SIZE (1024 * 1024)
#define LV_USE_OS LV_OS_NONE
#define LV_USE_LOG 0
#define LV_USE_THORVG_INTERNAL 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
#endif
EOF
cat > .tools/ui-preview-project/CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(voicememo_ui_preview C)
set(CMAKE_C_STANDARD 11)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_SOURCE}" lvgl)
function(add_ui_preview target width)
    add_executable(${target} "${PROJECT_ROOT}/tests/ui_preview.c"
        "${PROJECT_ROOT}/main/ui.c" "${PROJECT_ROOT}/main/cat_anim.c"
        "${PROJECT_ROOT}/main/memo_font.c" "${PROJECT_ROOT}/main/memo_font20.c"
        "${PROJECT_ROOT}/main/memo_font22.c" "${PROJECT_ROOT}/main/memo_font24.c"
        "${PROJECT_ROOT}/main/memo_icons.c")
    target_include_directories(${target} PRIVATE "${PROJECT_ROOT}/main")
    target_compile_definitions(${target} PRIVATE CAT_ANIM_TEST=1 MEMO_UI_WIDTH=${width} MEMO_UI_HEIGHT=${width})
    target_link_libraries(${target} PRIVATE lvgl)
endfunction()
add_ui_preview(ui_preview_360 360)
add_ui_preview(ui_preview_240 240)
add_ui_preview(ui_preview_vocat16 360)
target_compile_definitions(ui_preview_vocat16 PRIVATE CONFIG_MEMO_VOCAT16=1 MEMO_UI_VOCAT16_PREVIEW=1)
EOF
"$cmake_tool" -S .tools/ui-preview-project -B "$ui_build" \
    -DPROJECT_ROOT="$project_root" -DLVGL_SOURCE="$lvgl_source" \
    -DCMAKE_BUILD_TYPE=Debug > "$ui_output/ui-host-build.log" 2>&1
"$cmake_tool" --build "$ui_build" -j 4 >> "$ui_output/ui-host-build.log" 2>&1
"$ui_build"/ui_preview_360 > "$ui_output/ui-host-test.log" 2>&1
"$ui_build"/ui_preview_240 >> "$ui_output/ui-host-test.log" 2>&1
"$ui_build"/ui_preview_vocat16 >> "$ui_output/ui-host-test.log" 2>&1
cat "$ui_output/ui-host-test.log"
# PNG conversion uses only the Python standard library; no image tool install.
python3 - <<'PY'
from pathlib import Path
import struct, zlib, os
for ppm in Path(os.environ['MEMO_UI_OUTPUT']).glob('ui-*.ppm'):
    header, size, depth, pixels = ppm.read_bytes().split(b'\n', 3)
    assert header == b'P6' and depth == b'255'
    w, h = map(int, size.split())
    assert len(pixels) == w * h * 3
    def chunk(kind, body):
        return struct.pack('!I', len(body)) + kind + body + struct.pack('!I', zlib.crc32(kind + body))
    scan = b''.join(b'\x00' + pixels[y*w*3:(y+1)*w*3] for y in range(h))
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('!2I5B', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(scan)) + chunk(b'IEND', b'')
    ppm.with_suffix('.png').write_bytes(png)
    ppm.unlink()
PY
