cmake_minimum_required(VERSION 3.20)
project(winmm_mt7922_hci LANGUAGES CXX)

# The pass-through stubs are MASM; every other translation unit is plain C++17.
enable_language(ASM_MASM)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "the shim is x64 only; configure with -A x64")
endif()

# ---------------------------------------------------------------------------
# Pass-through stub generation.
#
# Our winmm.dll is loaded from the PC Manager install dir (app-dir precedence)
# by HiConnectivityService.exe, so it must export exactly the same entries as the
# system winmm.dll it forwards to.  The export table is therefore read from this
# build machine's winmm.dll -- there is no snapshot to keep in sync, and the DEF
# always describes the machine the shim was built on.  Build on the same Windows
# generation you intend to run on.
# ---------------------------------------------------------------------------
set(WINMM_DLL "$ENV{SystemRoot}/System32/winmm.dll")
if(NOT EXISTS "${WINMM_DLL}")
  message(FATAL_ERROR "system winmm.dll not found at ${WINMM_DLL}")
endif()

set(GEN_DIR "${CMAKE_BINARY_DIR}/generated")
set(GEN_SCRIPT "${CMAKE_SOURCE_DIR}/generator/gen_winmm_shim.py")

set(GEN_DEF  "${GEN_DIR}/winmm.def")
set(GEN_ASM  "${GEN_DIR}/winmm_thunks.asm")
set(GEN_PTRS "${GEN_DIR}/winmm_ptrs.h")
set(GEN_TAB  "${GEN_DIR}/winmm_table.h")

add_custom_command(
  OUTPUT "${GEN_DEF}" "${GEN_ASM}" "${GEN_PTRS}" "${GEN_TAB}"
  COMMAND "${Python3_EXECUTABLE}" "${GEN_SCRIPT}"
          --dll "${WINMM_DLL}" --out "${GEN_DIR}"
  DEPENDS "${GEN_SCRIPT}" "${WINMM_DLL}"
  COMMENT "Generating winmm pass-through thunks from ${WINMM_DLL}"
  VERBATIM)

add_custom_target(gen_winmm_stubs DEPENDS
  "${GEN_DEF}" "${GEN_ASM}" "${GEN_PTRS}" "${GEN_TAB}")

# ---------------------------------------------------------------------------
# The shim: pass-through thunks + MT7922 HCI advertising patch.
# ---------------------------------------------------------------------------
add_library(winmm SHARED
  src/proxy.cpp
  src/install.cpp
  src/mtk_hci.cpp
  src/hci_adv.cpp
  src/realtek_hooks.cpp
  src/wlan_fix.cpp
  "${GEN_ASM}"
  "${GEN_DEF}")

add_dependencies(winmm gen_winmm_stubs)
target_include_directories(winmm PRIVATE "${GEN_DIR}" "${CMAKE_SOURCE_DIR}/src")
set_target_properties(winmm PROPERTIES
  PREFIX "" OUTPUT_NAME "winmm" SUFFIX ".dll")
