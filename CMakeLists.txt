cmake_minimum_required(VERSION 3.16)
# C is needed only for the glib-compile-resources output (a generated .c file).
project(mb-benchmark-gui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Accelerator SDKs. Every one is optional and auto-detected: a backend whose SDK
# is absent is compiled out, its models never appear in the lists, and its
# checkbox is greyed out with the reason. The build works on any host.
# ---------------------------------------------------------------------------

# HailoRT ships no pkg-config or CMake package — headers under /usr/local/include,
# libhailort in /usr/local/lib.
find_library(HAILORT_LIB hailort PATHS /usr/local/lib /usr/lib)
find_path(HAILORT_INC hailo/hailort.hpp PATHS /usr/local/include /usr/include)
if(HAILORT_LIB AND HAILORT_INC)
  set(MB_HAVE_HAILO ON)
  message(STATUS "Hailo-8 backend:   ENABLED (${HAILORT_LIB})")
else()
  set(MB_HAVE_HAILO OFF)
  message(STATUS "Hailo-8 backend:   disabled (libhailort not found)")
endif()

# DeepX DXRT is installed system-wide and ships a CMake package that sets
# DXRT_FOUND + DXRT_LIBRARY (no imported target, so link the variable).
find_package(dxrt QUIET)
if(DXRT_FOUND)
  set(MB_HAVE_DEEPX ON)
  message(STATUS "DeepX M1 backend:  ENABLED (${DXRT_LIBRARY})")
else()
  set(MB_HAVE_DEEPX OFF)
  message(STATUS "DeepX M1 backend:  disabled (DXRT not found)")
endif()

# Axelera libaxruntime. AXELERA_DIR must hold include/axruntime/axruntime.h and
# lib/libaxruntime.so; the default is the system Voyager runtime install.
if(NOT DEFINED AXELERA_DIR)
  file(GLOB _axelera_runtimes "/opt/axelera/runtime-*")
  list(SORT _axelera_runtimes)
  list(REVERSE _axelera_runtimes)          # newest first
  foreach(_d ${_axelera_runtimes})
    if(EXISTS "${_d}/include/axruntime/axruntime.h")
      set(AXELERA_DIR "${_d}")
      break()
    endif()
  endforeach()
endif()
set(MB_HAVE_AXELERA OFF)
if(DEFINED AXELERA_DIR AND EXISTS "${AXELERA_DIR}/include/axruntime/axruntime.h")
  find_library(AXR_LIB axruntime PATHS "${AXELERA_DIR}/lib" NO_DEFAULT_PATH)
  if(AXR_LIB)
    set(MB_HAVE_AXELERA ON)
    message(STATUS "Axelera backend:   ENABLED (${AXR_LIB})")
  endif()
endif()
if(NOT MB_HAVE_AXELERA)
  message(STATUS "Axelera backend:   disabled (libaxruntime / headers not found)")
endif()

# MemryX MX3 (memx-accl). Unlike the power *telemetry* classes — which really
# are Python-only, hence the helper subprocess in Probes — the MxAccl inference
# runtime does ship a linkable C++ library and headers.
# NB the runtime lives in libmx_accl (the memx-accl package); libmemx is the
# *driver* library and carries none of the MxAccl symbols.
find_library(MEMX_ACCL_LIB mx_accl PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib)
find_library(MEMX_LIB memx PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib)
find_path(MEMX_INC memx/accl/MxAccl.h PATHS /usr/include /usr/local/include)
if(MEMX_ACCL_LIB AND MEMX_INC)
  set(MB_HAVE_MEMRYX ON)
  message(STATUS "MemryX MX3 backend: ENABLED (${MEMX_ACCL_LIB})")
else()
  set(MB_HAVE_MEMRYX OFF)
  message(STATUS "MemryX MX3 backend: disabled (libmx_accl / MxAccl.h not found)")
endif()

# Qualcomm Hexagon NSP via QAIRT (QNN "AI Engine Direct"). On Ubuntu for
# Dragonwing this comes from the ubuntu-qcom-iot/qcom-ppa archive as
# qairt-headers (/usr/include/QNN) + qairt-libs + qairt-dsp-binaries.
#
# Nothing is linked against QNN: the shim dlopen's whichever backend library is
# selected (libQnnHtp.so / libQnnGpu.so / libQnnCpu.so), so only libdl is
# needed. That is deliberate — it is also what would let the same binary drive
# the SoC's GPU and CPU backends given artifacts built for them.
#
# The SDK headers include each other with quoted relative paths, so the QNN
# include root itself has to be on the search path — /usr/include is not enough.
find_path(QNN_INC QnnInterface.h PATHS /usr/include/QNN /opt/qcom/aistack/qairt/include/QNN)
if(QNN_INC)
  set(MB_HAVE_QUALCOMM ON)
  message(STATUS "Qualcomm NSP backend: ENABLED (${QNN_INC})")
else()
  set(MB_HAVE_QUALCOMM OFF)
  message(STATUS "Qualcomm NSP backend: disabled (QNN headers not found)")
endif()

# libftdi1 (optional) — external reference-grade power from INA228 sensors on an
# FT232H USB->I2C bridge. Without watts the efficiency graphs stay at zero for
# any card that has no on-die power sensor.
pkg_check_modules(FTDI QUIET libftdi1)
if(FTDI_FOUND)
  message(STATUS "INA228 power:      ENABLED (libftdi1 ${FTDI_VERSION})")
else()
  message(STATUS "INA228 power:      disabled (libftdi1 not found)")
endif()

if(NOT MB_HAVE_HAILO AND NOT MB_HAVE_DEEPX AND NOT MB_HAVE_AXELERA AND NOT MB_HAVE_MEMRYX
   AND NOT MB_HAVE_QUALCOMM)
  message(WARNING "No accelerator SDK found — the GUI will build but can run "
                  "no benchmarks (telemetry only).")
endif()

# ---------------------------------------------------------------------------
# Assets -> GResource (the About dialog loads its logo from it).
# ---------------------------------------------------------------------------
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/mb-benchmark-gui.gresource.xml)
set(GRESOURCE_C ${CMAKE_CURRENT_BINARY_DIR}/mb-benchmark-gui.gresource.c)
execute_process(
  COMMAND ${GLIB_COMPILE_RESOURCES} --generate-dependencies
          --sourcedir=${CMAKE_CURRENT_SOURCE_DIR} ${GRESOURCE_XML}
  OUTPUT_VARIABLE GRESOURCE_DEPS OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE "\n" ";" GRESOURCE_DEPS "${GRESOURCE_DEPS}")  # already absolute
add_custom_command(
  OUTPUT ${GRESOURCE_C}
  COMMAND ${GLIB_COMPILE_RESOURCES} --target=${GRESOURCE_C} --generate-source
          --sourcedir=${CMAKE_CURRENT_SOURCE_DIR} ${GRESOURCE_XML}
  DEPENDS ${GRESOURCE_XML} ${GRESOURCE_DEPS}
  COMMENT "Compiling GResource bundle"
  VERBATIM)

add_executable(mb-benchmark
  src/main.cpp
  src/MainWindow.cpp
  src/ControlPanel.cpp
  src/GraphArea.cpp
  src/Probes.cpp
  src/Catalog.cpp
  src/Logger.cpp
  src/Automation.cpp
  src/Fetcher.cpp
  src/Bench.cpp
  src/bench_runners.cpp
  ${GRESOURCE_C}
)

target_include_directories(mb-benchmark PRIVATE src)
target_link_libraries(mb-benchmark PRIVATE PkgConfig::GTKMM Threads::Threads)
target_compile_options(mb-benchmark PRIVATE
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

if(MB_HAVE_HAILO)
  target_sources(mb-benchmark PRIVATE src/bench_hailo.cpp)
  target_include_directories(mb-benchmark PRIVATE ${HAILORT_INC})
  target_link_libraries(mb-benchmark PRIVATE ${HAILORT_LIB})
  target_compile_definitions(mb-benchmark PRIVATE MB_HAVE_HAILO=1)
endif()

if(MB_HAVE_DEEPX)
  target_sources(mb-benchmark PRIVATE src/bench_deepx.cpp)
  target_include_directories(mb-benchmark PRIVATE /usr/local/include)
  target_link_libraries(mb-benchmark PRIVATE ${DXRT_LIBRARY})
  target_compile_definitions(mb-benchmark PRIVATE MB_HAVE_DEEPX=1)
endif()

if(MB_HAVE_AXELERA)
  target_sources(mb-benchmark PRIVATE src/bench_axelera.cpp)
  target_include_directories(mb-benchmark PRIVATE "${AXELERA_DIR}/include")
  target_link_libraries(mb-benchmark PRIVATE ${AXR_LIB})
  # libaxruntime leaves some symbols to be resolved by the Voyager runtime.
  target_link_options(mb-benchmark PRIVATE -Wl,--allow-shlib-undefined)
  target_compile_definitions(mb-benchmark PRIVATE MB_HAVE_AXELERA=1)
endif()

if(MB_HAVE_MEMRYX)
  target_sources(mb-benchmark PRIVATE src/bench_memryx.cpp)
  target_include_directories(mb-benchmark PRIVATE ${MEMX_INC})
  target_link_libraries(mb-benchmark PRIVATE ${MEMX_ACCL_LIB})
  if(MEMX_LIB)
    target_link_libraries(mb-benchmark PRIVATE ${MEMX_LIB})
  endif()
  target_compile_definitions(mb-benchmark PRIVATE MB_HAVE_MEMRYX=1)
endif()

if(MB_HAVE_QUALCOMM)
  target_sources(mb-benchmark PRIVATE src/bench_qualcomm.cpp src/qnnruntime.cpp)
  target_include_directories(mb-benchmark PRIVATE ${QNN_INC})
  target_link_libraries(mb-benchmark PRIVATE ${CMAKE_DL_LIBS})
  # The vendor's own *_INIT macros do not initialise every member of their
  # structs; that warning is theirs, not ours.
  set_source_files_properties(src/qnnruntime.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-missing-field-initializers")
  target_compile_definitions(mb-benchmark PRIVATE MB_HAVE_QUALCOMM=1)
endif()

if(FTDI_FOUND)
  target_include_directories(mb-benchmark PRIVATE ${FTDI_INCLUDE_DIRS})
  target_link_libraries(mb-benchmark PRIVATE ${FTDI_LIBRARIES})
  target_link_directories(mb-benchmark PRIVATE ${FTDI_LIBRARY_DIRS})
  target_compile_definitions(mb-benchmark PRIVATE MB_HAVE_FTDI=1)
endif()

install(TARGETS mb-benchmark RUNTIME DESTINATION bin)
