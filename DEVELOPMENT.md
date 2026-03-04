# Local Development Guide

Everything you need to build, run, and test `vcv-rack-mcp-server` locally
without touching GitHub Actions.

---

## Overview of the dev loop

```
edit C++ → make install → restart Rack → test HTTP → test MCP
```

Each iteration takes about 5–10 seconds for an incremental build.

---

## 1. One-time setup

No manual SDK or dependency downloads required. Both `make` and `cmake` fetch
the Rack SDK and `cpp-httplib` automatically on the first run, and cache them
in `build/_deps/` for subsequent builds.

If you already have a Rack SDK and want to skip the download:

```bash
export RACK_DIR="$HOME/Rack-SDK"   # make picks this up automatically
```

### Install Python MCP server deps (once)

```bash
pip install mcp httpx
```

---

## 2. The inner build loop

```bash
# Build + install directly into Rack's plugins folder (downloads deps on first run)
make install
```

`make install` copies the compiled plugin into your Rack user folder automatically:

| OS | Plugins folder |
| --- | --- |
| macOS | `~/Documents/Rack2/plugins/` |
| Linux | `~/.Rack2/plugins/` |
| Windows | `%APPDATA%\Rack2\plugins\` |

Then **restart Rack** (plugins are loaded at startup). Search for **"Rack MCP Server"** in the module browser.

### Available make targets

| Target | What it does |
| --- | --- |
| `make` | Configure (download deps) + build |
| `make install` | Build + copy plugin to Rack plugins folder |
| `make dist` | Build + package as `.vcvplugin` in `dist/` |
| `make clean` | Remove `build/` and `dist/` |

### CMake directly (IDE / VS Code / CLion)

```bash
cmake -B build
cmake --build build --parallel
cmake --install build
```

Pass `-DRACK_DIR=/path/to/Rack-SDK` to use an existing SDK.

### Tip: faster iteration without restarting Rack

```bash
# Terminal 1 — rebuild + reinstall on every save
find . -name "*.cpp" -o -name "*.hpp" | entr -c make install

# Terminal 2 — relaunch Rack after each build (macOS)
pkill -x "Rack" ; sleep 0.5 ; open -a "VCV Rack 2 Free"
```

---

## 3. Check Rack's log for errors

If the plugin fails to load or crashes, check the log first:

| OS | Log location |
| --- | --- |
| macOS | `~/Documents/Rack2/log.txt` |
| Linux | `~/.Rack2/log.txt` |
| Windows | `%APPDATA%\Rack2\log.txt` |

```bash
tail -f ~/Documents/Rack2/log.txt
```

A successful load looks like:

```
[INFO  src/plugin/Plugin.cpp:76] Loaded plugin VCVRackMcpServer
[INFO  ...] [RackMcpServer] HTTP server starting on port 2600
```

---

## 4. Test the HTTP server directly

With Rack running and the module enabled, test endpoints with `curl`:

```bash
BASE="http://127.0.0.1:2600"

# Is the server up?
curl -s $BASE/status | jq .

# List modules in the current patch
curl -s $BASE/modules | jq .

# Search the library
curl -s "$BASE/library?q=oscillator" | jq .
curl -s "$BASE/library?tags=VCO" | jq .

# Add a module (use slugs from /library)
curl -s -X POST $BASE/modules/add \
  -H "Content-Type: application/json" \
  -d '{"plugin":"Fundamental","slug":"VCO","x":100,"y":0}' | jq .

# Inspect a module (replace 42 with the returned id)
curl -s $BASE/modules/42 | jq .

# Set a param
curl -s -X POST $BASE/modules/42/params \
  -H "Content-Type: application/json" \
  -d '{"params":[{"id":0,"value":0.5}]}' | jq .

# Connect two modules
curl -s -X POST $BASE/cables \
  -H "Content-Type: application/json" \
  -d '{"outputModuleId":42,"outputId":0,"inputModuleId":43,"inputId":0}' | jq .

# List cables
curl -s $BASE/cables | jq .

# Save patch
curl -s -X POST $BASE/patch/save \
  -H "Content-Type: application/json" \
  -d '{"path":"/tmp/test.vcv"}' | jq .
```

---

## 5. Test the MCP server

Run the Python MCP server pointing at Rack:

```bash
RACK_PORT=2600 python vcv_rack_mcp_server.py
```

### Interactive inspector (recommended)

```bash
pip install "mcp[cli]"
mcp dev vcv_rack_mcp_server.py
```

Opens a browser UI at `http://localhost:5173` — call every tool manually and
see the exact JSON exchanged, without wiring up Claude.

### Test with Claude Desktop

Add to `~/Library/Application Support/Claude/claude_desktop_config.json` (macOS):

```json
{
  "mcpServers": {
    "vcvrack": {
      "command": "python",
      "args": ["/absolute/path/to/vcv_rack_mcp_server.py"],
      "env": { "RACK_PORT": "2600" }
    }
  }
}
```

Restart Claude Desktop, then try:

> *"What modules do I have in my current patch?"*
> *"Add a VCO from Fundamental and connect its sine output to the Audio module input."*

---

## 6. Debug builds

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
cmake --install build
```

Run Rack from the terminal to see the crash output directly:

```bash
# macOS
/Applications/VCV\ Rack\ 2\ Free.app/Contents/MacOS/Rack

# Linux
./Rack
```

For deeper debugging (GDB / LLDB), build Rack from source and attach to the
process — see the [Rack building docs](https://vcvrack.com/manual/Building).

---

## 7. Checklist before pushing

```bash
# Clean build from scratch (catches missing includes)
make clean && make

# Verify plugin.json is valid JSON
python3 -c "import json; json.load(open('plugin.json')); print('plugin.json OK')"

# Quick HTTP smoke test (Rack must be running with module enabled)
curl -sf http://127.0.0.1:2600/status | python3 -m json.tool
```

---

## Quick reference

| Task | Command |
| --- | --- |
| Build | `make` |
| Build + install | `make install` |
| Package for release | `make dist` |
| Clean | `make clean` |
| CMake configure | `cmake -B build` |
| CMake build + install | `cmake --build build && cmake --install build` |
| Test HTTP | `curl -s http://127.0.0.1:2600/status \| jq .` |
| Inspect MCP tools | `mcp dev vcv_rack_mcp_server.py` |
| Watch Rack log | `tail -f ~/Documents/Rack2/log.txt` |
