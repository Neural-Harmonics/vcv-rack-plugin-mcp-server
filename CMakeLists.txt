cmake_minimum_required(VERSION 3.21)
project(VCVRackMcpServer VERSION 2.1.0 LANGUAGES CXX)
# NOTE: The major version MUST be 2 — VCV Rack 2 rejects plugins whose
# version does not start with "2." (ABI compatibility check).
# Use minor.patch for your own versioning: 2.<minor>.<patch>

# =============================================================================
# vcv-rack-mcp-server — VCV Rack 2 Plugin  (CMake build)
#
# USAGE:
#   cmake -B build
#   cmake --build build
#   cmake --install build   # installs to Rack plugins folder
#
# CMake automatically downloads:
#   - cpp-httplib (into dep/include)
#   - Rack SDK (into build/_deps/rack-sdk, cached between builds)
#
# Optional: Use your own Rack SDK with -DRACK_DIR=/path/to/Rack-SDK
# =============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Detect platform/architecture early ───────────────────────────────────────
if(APPLE)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    set(RACK_ARCH "mac-arm64")
  else()
    set(RACK_ARCH "mac-x64")
  endif()
  set(PLUGIN_EXT ".dylib")
elseif(WIN32)
  set(RACK_ARCH "win-x64")
  set(PLUGIN_EXT ".dll")
else()
  set(RACK_ARCH "lin-x64")
  set(PLUGIN_EXT ".so")
endif()

# ── Rack SDK auto-download and location ──────────────────────────────────────
set(RACK_SDK_VERSION "2.6.6")

if(NOT DEFINED RACK_DIR)
  if(DEFINED ENV{RACK_DIR})
    set(RACK_DIR $ENV{RACK_DIR})
  else()
    # Auto-download Rack SDK if not provided
    set(RACK_SDK_URL "https://vcvrack.com/downloads/Rack-SDK-${RACK_SDK_VERSION}-${RACK_ARCH}.zip")
    set(RACK_SDK_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps/rack-sdk")
    set(RACK_DIR "${RACK_SDK_DOWNLOAD_DIR}/Rack-SDK")
    
    if(NOT EXISTS "${RACK_DIR}/include/rack.hpp")
      message(STATUS "Rack SDK not found. Downloading from ${RACK_SDK_URL}...")
      
      file(MAKE_DIRECTORY "${RACK_SDK_DOWNLOAD_DIR}")
      set(RACK_SDK_ZIP "${RACK_SDK_DOWNLOAD_DIR}/Rack-SDK.zip")
      
      # Download SDK
      file(DOWNLOAD
        "${RACK_SDK_URL}"
        "${RACK_SDK_ZIP}"
        STATUS RACK_SDK_DOWNLOAD_STATUS
        SHOW_PROGRESS
        TLS_VERIFY ON
      )
      
      list(GET RACK_SDK_DOWNLOAD_STATUS 0 RACK_SDK_DOWNLOAD_CODE)
      list(GET RACK_SDK_DOWNLOAD_STATUS 1 RACK_SDK_DOWNLOAD_MESSAGE)
      
      if(NOT RACK_SDK_DOWNLOAD_CODE EQUAL 0)
        message(FATAL_ERROR "Failed to download Rack SDK: ${RACK_SDK_DOWNLOAD_MESSAGE}")
      endif()
      
      # Extract SDK
      message(STATUS "Extracting Rack SDK...")
      file(ARCHIVE_EXTRACT
        INPUT "${RACK_SDK_ZIP}"
        DESTINATION "${RACK_SDK_DOWNLOAD_DIR}"
      )
      
      # Verify extraction
      if(NOT EXISTS "${RACK_DIR}/include/rack.hpp")
        message(FATAL_ERROR "Failed to extract Rack SDK. rack.hpp not found at ${RACK_DIR}/include/rack.hpp")
      endif()
      
      message(STATUS "Rack SDK downloaded and extracted successfully")
    else()
      message(STATUS "Using cached Rack SDK at: ${RACK_DIR}")
    endif()
  endif()
endif()

message(STATUS "Using Rack SDK at: ${RACK_DIR}")

# ── Generate plugin.json from template (single source of truth: PROJECT_VERSION) ─
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/plugin.json.in
  ${CMAKE_CURRENT_BINARY_DIR}/plugin.json
  @ONLY
)

# ── Dependency auto-download: cpp-httplib (header-only, MIT) ─────────────────
set(HTTPLIB_VERSION_FILE ${CMAKE_CURRENT_SOURCE_DIR}/deps/httplib.version)
if(EXISTS ${HTTPLIB_VERSION_FILE})
  file(READ ${HTTPLIB_VERSION_FILE} HTTPLIB_VERSION)
  string(STRIP "${HTTPLIB_VERSION}" HTTPLIB_VERSION)
else()
  set(HTTPLIB_VERSION "v0.18.0")
endif()

set(HTTPLIB_URL https://raw.githubusercontent.com/yhirose/cpp-httplib/${HTTPLIB_VERSION}/httplib.h)
set(HTTPLIB_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dep/include)
set(HTTPLIB_HEADER ${HTTPLIB_INCLUDE_DIR}/httplib.h)

if(NOT EXISTS ${HTTPLIB_HEADER})
  file(MAKE_DIRECTORY ${HTTPLIB_INCLUDE_DIR})
  message(STATUS "Downloading cpp-httplib ${HTTPLIB_VERSION}...")
  file(DOWNLOAD
    ${HTTPLIB_URL}
    ${HTTPLIB_HEADER}
    STATUS HTTPLIB_DOWNLOAD_STATUS
    TLS_VERIFY ON
  )
  list(GET HTTPLIB_DOWNLOAD_STATUS 0 HTTPLIB_DOWNLOAD_CODE)
  list(GET HTTPLIB_DOWNLOAD_STATUS 1 HTTPLIB_DOWNLOAD_MESSAGE)
  if(NOT HTTPLIB_DOWNLOAD_CODE EQUAL 0)
    message(FATAL_ERROR "Failed to download cpp-httplib: ${HTTPLIB_DOWNLOAD_MESSAGE}")
  endif()
endif()

# ── Rack SDK include / link paths ─────────────────────────────────────────────
set(RACK_INCLUDE_DIR  ${RACK_DIR}/include)
set(RACK_DEP_DIR      ${RACK_DIR}/dep/include)
set(RACK_LIB          ${RACK_DIR}/libRack${CMAKE_SHARED_LIBRARY_SUFFIX})

if(NOT EXISTS ${RACK_INCLUDE_DIR}/rack.hpp)
  message(FATAL_ERROR "rack.hpp not found in ${RACK_INCLUDE_DIR}. Check RACK_DIR.")
endif()

# ── Plugin shared library ─────────────────────────────────────────────────────
add_library(VCVRackMcpServer MODULE
  plugin.cpp
  RackMcpServer.cpp
)

set_target_properties(VCVRackMcpServer PROPERTIES
  PREFIX   ""
  SUFFIX   ${PLUGIN_EXT}
  OUTPUT_NAME "plugin"
)

target_include_directories(VCVRackMcpServer PRIVATE
  ${RACK_INCLUDE_DIR}
  ${RACK_DEP_DIR}
  ${HTTPLIB_INCLUDE_DIR}
)

target_compile_options(VCVRackMcpServer PRIVATE
  -fPIC
  -O3
  -funsafe-math-optimizations
  -fno-omit-frame-pointer
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wno-unused-parameter>
)

# Platform-specific optimization flags
if(RACK_ARCH STREQUAL "lin-x64" OR RACK_ARCH STREQUAL "win-x64")
  target_compile_options(VCVRackMcpServer PRIVATE -march=nocona)  # SSE4.2 baseline for x64
elseif(RACK_ARCH STREQUAL "mac-x64")
  target_compile_options(VCVRackMcpServer PRIVATE -march=nehalem)  # SSE4.2 for Intel Mac
endif()
# mac-arm64 doesn't need -march flag

# Define platform macro (replace hyphens with underscores for valid C++ identifiers)
string(REPLACE "-" "_" RACK_ARCH_MACRO "${RACK_ARCH}")
target_compile_definitions(VCVRackMcpServer PRIVATE
  ARCH_${RACK_ARCH_MACRO}   # lets source code #ifdef on arch if needed
)

if(APPLE)
  target_link_options(VCVRackMcpServer PRIVATE
    -undefined dynamic_lookup   # Rack symbols resolved at load time by Rack
    -Wl,-install_name,@rpath/plugin.dylib
  )
elseif(UNIX)
  target_link_options(VCVRackMcpServer PRIVATE
    -Wl,--allow-shlib-undefined  # same — Rack resolves symbols at load time
  )
  target_link_libraries(VCVRackMcpServer PRIVATE pthread)
elseif(WIN32)
  target_link_libraries(VCVRackMcpServer PRIVATE ws2_32 mswsock)
endif()

# ── Install target ────────────────────────────────────────────────────────────
# Determines Rack plugins folder per platform
if(APPLE)
  set(RACK_PLUGINS_DIR "$ENV{HOME}/Library/Application Support/Rack2/plugins-${RACK_ARCH}")
elseif(WIN32)
  set(RACK_PLUGINS_DIR "$ENV{APPDATA}/Rack2/plugins")
else()
  set(RACK_PLUGINS_DIR "$ENV{HOME}/.Rack2/plugins")
endif()

set(PLUGIN_INSTALL_DIR ${RACK_PLUGINS_DIR}/VCVRackMcpServer)

install(TARGETS VCVRackMcpServer
  LIBRARY DESTINATION ${PLUGIN_INSTALL_DIR}
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/plugin.json
  DESTINATION ${PLUGIN_INSTALL_DIR}
)
install(DIRECTORY res/
  DESTINATION ${PLUGIN_INSTALL_DIR}/res
  OPTIONAL
)

message(STATUS "Install target: ${PLUGIN_INSTALL_DIR}")

# ── Summary ───────────────────────────────────────────────────────────────────
message(STATUS "vcv-rack-mcp-server plugin configured.")
message(STATUS "  Platform : ${RACK_ARCH}")
message(STATUS "  Rack SDK : ${RACK_DIR}")
message(STATUS "  Build    : cmake --build <build_dir>")
message(STATUS "  Install  : cmake --install <build_dir>")
