cmake_minimum_required(VERSION 3.22)
project(doom C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# DOOM engine: every .c in doom/ (the network layer, sound server, X11
# backend, French strings, DOS/asm code etc. have been removed) plus the
# small Android platform layer in platform/.
file(GLOB DOOM_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/doom/*.c)
file(GLOB PLAT_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/*.c)

# native_app_glue lives in the NDK; compile it straight into the library.
set(GLUE_DIR ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(doom SHARED ${DOOM_SRC} ${PLAT_SRC} ${GLUE_DIR}/android_native_app_glue.c)
target_include_directories(doom PRIVATE doom platform ${GLUE_DIR})

# -O2 keeps the software renderer fast; thin-LTO + section GC drop whatever is
# unreferenced. The engine is 1990s C, so avoid strict-aliasing / signed
# overflow assumptions.
target_compile_options(doom PRIVATE
    -O2 -fvisibility=hidden -ffunction-sections -fdata-sections
    -fno-strict-aliasing -fwrapv -flto=thin
    # DOOM assumes plain 'char' is signed (x86). On ARM/ARM64 Android it is
    # unsigned, which breaks negative forwardmove/sidemove (inverted, jerky
    # movement). Force the original behaviour everywhere.
    -fsigned-char
    -Wno-unused-parameter -Wno-unused-variable -Wno-parentheses -Wno-pointer-sign)
target_link_options(doom PRIVATE
    -flto=thin -Wl,--gc-sections -Wl,--build-id=none -Wl,-z,max-page-size=16384)

target_link_libraries(doom PRIVATE android log EGL GLESv2 aaudio m)
