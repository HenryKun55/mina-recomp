cmake_minimum_required(VERSION 3.20)
project(mina-recomp C)

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
list(TRANSFORM GEN_FILES PREPEND "${GEN_DIR}/")

add_library(mina_gen OBJECT ${GEN_FILES} ${GEN_DIR}/tables.c)
target_include_directories(mina_gen PRIVATE runtime/include ${GEN_DIR})
target_compile_options(mina_gen PRIVATE -ffp-contract=off -fno-strict-aliasing
  -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)

# ---- runtime ----------------------------------------------------------------
file(GLOB RUNTIME_SOURCES runtime/src/*.c)
add_executable(mina ${RUNTIME_SOURCES} $<TARGET_OBJECTS:mina_gen>)
target_include_directories(mina PRIVATE runtime/include ${GEN_DIR})
target_compile_options(mina PRIVATE -ffp-contract=off -fno-strict-aliasing -Wall -Wno-unused-function)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
target_link_libraries(mina PRIVATE PkgConfig::SDL2 m pthread)
if(APPLE)
  target_link_libraries(mina PRIVATE "-framework OpenGL")
endif()
