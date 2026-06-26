cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Inject our lwIP forwarding hook (per-client internet gate) into the lwip build.
# The -I makes the hook header visible when lwip compiles ip4.c.
idf_build_set_property(COMPILE_OPTIONS
    "-DESP_IDF_LWIP_HOOK_FILENAME=\"wifitree_lwip_hooks.h\"" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_LIST_DIR}/src" APPEND)

project(wifitree-esp32)
