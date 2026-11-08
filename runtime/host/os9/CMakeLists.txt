# GPM2 for Mac OS 9 (PowerPC, CarbonLib) with Retro68. Usage (in CI, inside the ghcr.io/autc04/retro68 image):
#   cmake -S runtime/host/os9 -B build-os9 -DCMAKE_TOOLCHAIN_FILE=/Retro68-build/toolchain/powerpc-apple-macos/cmake/retrocarbon.toolchain.cmake
cmake_minimum_required(VERSION 3.10)
project(GPM2 C)

get_filename_component(RT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)       # runtime/
get_filename_component(GEN ${RT}/../gen ABSOLUTE)

file(GLOB WIN32_SRC ${RT}/win32/*.c)
file(GLOB GEN_SRC ${GEN}/*.c)
list(FILTER GEN_SRC EXCLUDE REGEX "insn_tests\\.c$")

set(SRC
    ${RT}/machine.c ${RT}/image.c ${RT}/dispatch.c ${RT}/hooks/hooks.c ${RT}/gpm2run.c
    ${WIN32_SRC} ${GEN_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}/host_os9.c ${CMAKE_CURRENT_SOURCE_DIR}/os9_posix.c ${CMAKE_CURRENT_SOURCE_DIR}/os9_net.c ${RT}/host/sharpscale.c)

add_application(GPM2 ${SRC} ${CMAKE_CURRENT_SOURCE_DIR}/gpm2.r ${CMAKE_CURRENT_SOURCE_DIR}/icons_gen.r
    TYPE "APPL" CREATOR "GPM2")

target_include_directories(GPM2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim ${RT} ${GEN})
target_compile_definitions(GPM2 PRIVATE GPM2_OS9)
target_compile_options(GPM2 PRIVATE -O1 -g0 -fno-strict-aliasing -ffunction-sections -include ${CMAKE_CURRENT_SOURCE_DIR}/os9_compat.h)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/os9_net.c PROPERTIES COMPILE_DEFINITIONS OS9_NO_REDIRECT)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/os9_posix.c PROPERTIES COMPILE_DEFINITIONS OS9_NO_REDIRECT COMPILE_FLAGS -fno-builtin)
set_target_properties(GPM2 PROPERTIES LINK_FLAGS "-Wl,-gc-sections")
