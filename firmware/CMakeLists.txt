cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.2.5")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(meowbox)

# Tile swipes (and every other animated scroll) take at most 250 ms instead of LVGL's 400 ms: a 296 px tile always
# hit the 400 ms ceiling, and each frame of it is a full-screen redraw (docs/perf/results-0.2.3.md, swipe FPS).
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_definitions(${lvgl_lib} PRIVATE SCROLL_ANIM_TIME_MAX=250)
