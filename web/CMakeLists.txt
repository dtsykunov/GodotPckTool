# Emscripten build for browser-based .pck extractor
# Build with: emcmake cmake -B build-wasm web/ && cmake --build build-wasm
cmake_minimum_required(VERSION 3.10)
project(GodotPckToolWasm)

# Mirror version from root CMakeLists.txt — update when root version changes
set(GODOT_PCK_TOOL_VERSION_MAJOR 2)
set(GODOT_PCK_TOOL_VERSION_MINOR 2)
set(GODOT_PCK_TOOL_VERSION_STR "${GODOT_PCK_TOOL_VERSION_MAJOR}.${GODOT_PCK_TOOL_VERSION_MINOR}")

# Generate Include.h (mirrors the configure_file call in root CMakeLists.txt)
configure_file("../src/Include.h.in" "${PROJECT_BINARY_DIR}/Include.h")

# Global includes needed by src/ (mirrors root CMakeLists.txt include_directories)
include_directories(${PROJECT_BINARY_DIR})
include_directories(../src)
include_directories(../third_party/cxxopts/include)
include_directories(../third_party/json/single_include)

set(BUILD_SHARED_LIBS OFF)

if(CMAKE_BUILD_TYPE STREQUAL "")
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Third-party libs (md5, etc.)
add_subdirectory(../third_party third_party_build)

# pck library only — skip CLI executable (it has platform flags invalid under Emscripten)
set(BUILD_GODOTPCKTOOL_EXECUTABLE OFF CACHE BOOL "" FORCE)
add_subdirectory(../src src_build)

# WASM entry point
add_executable(godotpcktool wasm_entry.cpp)
target_link_libraries(godotpcktool PRIVATE pck)

set_target_properties(godotpcktool PROPERTIES
    SUFFIX ".js"
    CXX_STANDARD 17
    CXX_EXTENSIONS OFF
)

# SHELL: prefix prevents CMake from deduplicating/splitting bracketed argument lists
target_link_options(godotpcktool PRIVATE
    "SHELL:-sEXPORTED_FUNCTIONS=['_extractPck','_getExtractedFiles','_getLastError','_listPck','_repackPck','_addToPck','_malloc','_free']"
    "SHELL:-sEXPORTED_RUNTIME_METHODS=['FS','HEAPU8','UTF8ToString','stringToUTF8','lengthBytesUTF8']"
    -sALLOW_MEMORY_GROWTH=1
    -sMODULARIZE=1
    -sEXPORT_NAME=GodotPckTool
)
