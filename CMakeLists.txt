cmake_minimum_required(VERSION 3.20)
if(DEFINED ENV{VITASDK} AND CMAKE_TOOLCHAIN_FILE MATCHES "vita")
  set(VITA ON)
endif()
project(mina-recomp C)
if(VITA)
  include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
  set(VITA_APP_NAME "Mina the Hollower")
  set(VITA_TITLEID "MINA00001")
  set(VITA_VERSION "00.01")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(MINA_EXE "${CMAKE_SOURCE_DIR}/gamedata/app/Contents/MacOS/MinaTheHollower" CACHE FILEPATH
    "The game's executable (MinaTheHollower inside the .app)")
if(NOT EXISTS "${MINA_EXE}")
  message(FATAL_ERROR "Game executable not found: ${MINA_EXE} (link your install at gamedata/app)")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# ---- generated code -------------------------------------------------------
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
set(GEN_INPUTS
  ${CMAKE_SOURCE_DIR}/tools/recomp/gen.py
  ${CMAKE_SOURCE_DIR}/tools/recomp/a64.py
  ${CMAKE_SOURCE_DIR}/tools/recomp/simd.py
  ${CMAKE_SOURCE_DIR}/tools/analysis/macho.py
  ${CMAKE_SOURCE_DIR}/runtime/natives.txt
  ${MINA_EXE})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GEN_INPUTS})
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/recomp/gen.py ${MINA_EXE} ${GEN_DIR}
          --natives ${CMAKE_SOURCE_DIR}/runtime/natives.txt
  RESULT_VARIABLE GEN_RESULT)
if(NOT GEN_RESULT EQUAL 0)
  message(FATAL_ERROR "recompilation failed")
endif()
file(STRINGS "${GEN_DIR}/files.txt" GEN_FILES)

# Shaders: the game's metallibs translated to GLSL (needs llvm-dis: brew install llvm)
get_filename_component(MINA_DATA "${MINA_EXE}/../../Resources/data" ABSOLUTE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/tools/shaders/air2glsl.py ${MINA_DATA}/shaders.pak.yc)
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/shaders/air2glsl.py
          ${MINA_DATA}/shaders.pak.yc ${GEN_DIR}/shaders.c
  RESULT_VARIABLE SHADER_RESULT)
if(NOT SHADER_RESULT EQUAL 0)
  message(FATAL_ERROR "shader translation failed")
endif()
list(TRANSFORM GEN_FILES PREPEND "${GEN_DIR}/")

if(VITA)
  # Cortex-A9: no FMA (fused multiply-add rounds once more), NEON for the vector helpers
  set(MINA_ARCH_FLAGS -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -O2)
  # no debug info for the generated code: GCC's var-tracking is superlinear on
  # the biggest recompiled functions (it took half the build time)
  set(MINA_GEN_FLAGS -g0 -fno-var-tracking -fno-var-tracking-assignments)
  set(MINA_DEFS MINA_FAST_FMA)
  option(MINA_VITA_NULL_GL "Vita build without a renderer (game logic only)" ON)
  if(MINA_VITA_NULL_GL)
    list(APPEND MINA_DEFS MINA_NULL_GL)
  endif()
else()
  set(MINA_ARCH_FLAGS)
  set(MINA_GEN_FLAGS)
  set(MINA_DEFS)
endif()

add_library(mina_gen OBJECT ${GEN_FILES} ${GEN_DIR}/tables.c)
target_include_directories(mina_gen PRIVATE runtime/include ${GEN_DIR})
target_compile_definitions(mina_gen PRIVATE ${MINA_DEFS})
target_compile_options(mina_gen PRIVATE ${MINA_ARCH_FLAGS} ${MINA_GEN_FLAGS} -ffp-contract=off -fno-strict-aliasing
  -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)

# ---- runtime ----------------------------------------------------------------
file(GLOB RUNTIME_SOURCES runtime/src/*.c)
add_executable(mina ${RUNTIME_SOURCES} ${GEN_DIR}/shaders.c $<TARGET_OBJECTS:mina_gen>)
target_include_directories(mina PRIVATE runtime/include ${GEN_DIR})
target_compile_definitions(mina PRIVATE ${MINA_DEFS})
target_compile_options(mina PRIVATE ${MINA_ARCH_FLAGS} -ffp-contract=off -fno-strict-aliasing -Wall -Wno-unused-function)

if(VITA)
  target_include_directories(mina PRIVATE "$ENV{VITASDK}/arm-vita-eabi/include/SDL2")
  target_link_libraries(mina PRIVATE
    SDL2 vitaGL vitashark SceShaccCgExt mathneon taihen_stub
    SceShaccCg_stub SceKernelDmacMgr_stub SceCtrl_stub SceTouch_stub SceHid_stub SceMotion_stub
    SceAudio_stub SceAudioIn_stub SceDisplay_stub SceGxm_stub SceCommonDialog_stub SceSysmodule_stub
    ScePower_stub SceAppMgr_stub SceAppUtil_stub SceIme_stub SceLibKernel_stub SceProcessmgr_stub
    SceIofilemgr_stub stdc++ pthread m c)
  vita_create_self(eboot.bin mina UNSAFE)
  vita_create_vpk(mina.vpk ${VITA_TITLEID} eboot.bin VERSION ${VITA_VERSION} NAME "${VITA_APP_NAME}")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  target_link_libraries(mina PRIVATE PkgConfig::SDL2 m pthread)
  if(APPLE)
    target_link_libraries(mina PRIVATE "-framework OpenGL")
  endif()
endif()
