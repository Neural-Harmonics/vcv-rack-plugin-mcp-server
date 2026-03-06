# =============================================================================
# vcv-rack-mcp-server — VCV Rack 2 Plugin
# Makefile: thin CMake wrapper
#
# All heavy lifting (Rack SDK download, cpp-httplib download, cross-compile
# flags) is handled by CMakeLists.txt.
#
# USAGE:
#   make           # configure + build  (downloads deps automatically)
#   make install   # install to your Rack plugins folder
#   make dist      # create distributable .zip package in dist/
#   make clean     # remove build/ and dist/
#
# Override build directory:
#   make BUILD_DIR=mybuild
#
# Point at an existing Rack SDK (skips auto-download):
#   make RACK_DIR=/path/to/Rack-SDK
# =============================================================================

BUILD_DIR ?= build

# Read slug + version from plugin.json (requires jq; fallback to literals)
SLUG    := $(shell jq -r '.slug'    plugin.json 2>/dev/null || echo VCVRackMcpServer)
VERSION := $(shell jq -r '.version' plugin.json 2>/dev/null || echo 2.0.0)

# Detect host OS and derive the plugin binary name + dist platform tag
UNAME := $(shell uname -s 2>/dev/null)
ifeq ($(UNAME),Darwin)
  PLUGIN_FILE   := plugin.dylib
  ARCH          := $(shell uname -m)
  ifeq ($(ARCH),arm64)
    DIST_PLATFORM := mac-arm64
  else
    DIST_PLATFORM := mac-x64
  endif
else ifeq ($(UNAME),Linux)
  PLUGIN_FILE   := plugin.so
  DIST_PLATFORM := lin-x64
else
  PLUGIN_FILE   := plugin.dll
  DIST_PLATFORM := win-x64
endif

# Pass RACK_DIR through to CMake if set
ifdef RACK_DIR
  CMAKE_RACK_DIR := -DRACK_DIR=$(RACK_DIR)
else
  CMAKE_RACK_DIR :=
endif

# ── Phony targets ─────────────────────────────────────────────────────────────
.PHONY: all dep install dist clean test test-unit test-integration

# ── Configure (also triggers dep downloads) ───────────────────────────────────
$(BUILD_DIR)/CMakeCache.txt: CMakeLists.txt plugin.json
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release $(CMAKE_RACK_DIR)

dep: $(BUILD_DIR)/CMakeCache.txt

# ── Build ─────────────────────────────────────────────────────────────────────
$(BUILD_DIR)/$(PLUGIN_FILE): $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) --parallel

all: $(BUILD_DIR)/$(PLUGIN_FILE)

# ── Install to Rack plugins folder ───────────────────────────────────────────
install: $(BUILD_DIR)/$(PLUGIN_FILE)
	cmake --install $(BUILD_DIR)

# ── Package as .zip (plugin binary + generated plugin.json + res/) ─
dist: $(BUILD_DIR)/$(PLUGIN_FILE)
	cmake --build $(BUILD_DIR) --target dist
	@echo "Created dist/$(SLUG)-$(VERSION)-$(DIST_PLATFORM).zip"

# ── Tests ─────────────────────────────────────────────────────────────────────
# Unit tests: compile standalone C++ test binary and run it (no Rack needed)
tests/test_json_helpers: tests/test_json_helpers.cpp
	c++ -std=c++17 -o tests/test_json_helpers tests/test_json_helpers.cpp

test-unit: tests/test_json_helpers
	tests/test_json_helpers

# Integration tests: require VCV Rack running with MCP Server module ON
# Pass PORT=XXXX to override the default port 2600.
TEST_PORT ?= 2600
test-integration:
	python3 tests/test_server.py --port $(TEST_PORT)

# Run unit tests only (safe, no VCV Rack needed)
test: test-unit

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) dist tests/test_json_helpers
