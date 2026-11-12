cmake_minimum_required(VERSION 3.20)
project(cruisn_rt64_bridge_host_tests LANGUAGES CXX)

set(RT64_3DS_RENDERER_DIR "" CACHE PATH
    "Path to pinned rt64-3ds src/renderer (host interpreter test only)")
if(NOT EXISTS "${RT64_3DS_RENDERER_DIR}/rt64_3ds_dl.cpp" OR
   NOT EXISTS "${RT64_3DS_RENDERER_DIR}/rt64_3ds_interp.cpp")
    message(FATAL_ERROR
        "Set RT64_3DS_RENDERER_DIR to the pinned rt64-3ds src/renderer directory")
endif()

add_executable(cruisn_rt64_bridge_tests
    src/cruisn_rt64_bridge.cpp
    src/cruisn_rdram_snapshot.cpp
    tests/bridge_tests.cpp
    "${RT64_3DS_RENDERER_DIR}/rt64_3ds_dl.cpp"
    "${RT64_3DS_RENDERER_DIR}/rt64_3ds_interp.cpp")
target_compile_features(cruisn_rt64_bridge_tests PRIVATE cxx_std_20)
target_include_directories(cruisn_rt64_bridge_tests PRIVATE
    include "${RT64_3DS_RENDERER_DIR}")
