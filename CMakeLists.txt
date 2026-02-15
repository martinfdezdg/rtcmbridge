cmake_minimum_required(VERSION 3.16)
project(rtcmbridge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_NATS_TOOLS "Build tools requiring libnats" ON)
include(CTest)

find_package(Threads REQUIRED)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(NATS QUIET libnats)
endif()

if(NATS_FOUND)
  add_library(NATS::nats INTERFACE IMPORTED)
  target_include_directories(NATS::nats INTERFACE ${NATS_INCLUDE_DIRS})
  target_link_libraries(NATS::nats INTERFACE ${NATS_LIBRARIES})
elseif(BUILD_NATS_TOOLS)
  find_library(NATS_LIBRARY nats)
  if(NATS_LIBRARY)
    add_library(NATS::nats UNKNOWN IMPORTED)
    set_target_properties(NATS::nats PROPERTIES IMPORTED_LOCATION "${NATS_LIBRARY}")
  else()
    message(FATAL_ERROR "libnats not found. Install it or configure with -DBUILD_NATS_TOOLS=OFF")
  endif()
endif()

add_library(rtcmbridge_core
  src/core/mountpoint_config.cpp
  src/core/ntrip_client.cpp
  src/core/rtcm_frame.cpp
  src/core/rtcm_1005.cpp
)

target_include_directories(rtcmbridge_core PUBLIC include)
target_link_libraries(rtcmbridge_core PUBLIC Threads::Threads)

add_executable(rtcm_decoder_ntrip src/tools/rtcm_decoder_ntrip.cpp)
target_link_libraries(rtcm_decoder_ntrip PRIVATE Threads::Threads)

add_executable(rtcm_rinex_recorder src/tools/rtcm_rinex_recorder.cpp)
target_link_libraries(rtcm_rinex_recorder PRIVATE rtcmbridge_core)

add_executable(rtcm_station_position src/tools/rtcm_station_position.cpp)
target_link_libraries(rtcm_station_position PRIVATE rtcmbridge_core)

if(BUILD_NATS_TOOLS)
  add_executable(rtcm_bridge src/tools/rtcm_bridge.cpp)
  target_link_libraries(rtcm_bridge PRIVATE Threads::Threads NATS::nats)

  add_executable(rtcm_decoder_nats src/tools/rtcm_decoder_nats.cpp)
  target_link_libraries(rtcm_decoder_nats PRIVATE Threads::Threads NATS::nats)
endif()

set_target_properties(
  rtcm_decoder_ntrip
  rtcm_rinex_recorder
  rtcm_station_position
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(TARGET rtcm_bridge)
  set_target_properties(rtcm_bridge PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

if(TARGET rtcm_decoder_nats)
  set_target_properties(rtcm_decoder_nats PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

if(BUILD_TESTING)
  add_executable(rtcm_core_tests tests/test_rtcm_core.cpp)
  target_link_libraries(rtcm_core_tests PRIVATE rtcmbridge_core Threads::Threads)
  add_test(NAME rtcm_core_tests COMMAND rtcm_core_tests)
endif()
