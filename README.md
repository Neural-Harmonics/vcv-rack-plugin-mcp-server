# vcv-rack-mcp-server — VCV Rack 2 × AI/MCP

> Connect Claude (or any MCP client) to VCV Rack 2. Build patches, add modules,
> connect cables and tweak parameters — all through natural language.

---

## How it works

The **Rack MCP Server** module embeds a lightweight HTTP server directly inside VCV Rack. It speaks two protocols:

- **MCP (Model Context Protocol)** — native JSON-RPC 2.0 at `POST /mcp`, for direct integration with Claude Desktop, Cursor, and any MCP-compatible AI client.
- **REST API** — plain HTTP endpoints for scripts and the included CLI skill.

```
Claude Desktop / Cursor / any MCP client
        |
        | JSON-RPC 2.0  (POST http://127.0.0.1:2600/mcp)
        v
  Rack MCP Server module  (C++ plugin inside VCV Rack)
        |
        | Rack engine API
        v
  VCV Rack patch (modules, cables, params)
```

---

## Installation

### From VCV Library *(once published)*
1. Open VCV Rack → Library menu → Log in
2. Find **vcv-rack-mcp-server** → Subscribe
3. Restart Rack — the module appears in the browser under **Utility**

### From GitHub Releases (manual)
1. Go to [Releases](https://github.com/bisegni/vcvrack-mcp-server/releases)
2. Download the `.vcvplugin` for your OS
3. Double-click it — Rack installs it automatically

---

## Quick start

1. Open VCV Rack 2 and add the **Rack MCP Server** module to any patch
2. Set the **PORT** field (default: **2600**)
3. Flip the **ON/OFF** switch — the STATUS LED turns green

The server is now listening on `http://127.0.0.1:2600`.

---

## Option A — Direct MCP (Claude Desktop, Cursor, etc.)

The module exposes a native MCP endpoint. No Python server required.

### Claude Desktop

Add to `~/Library/Application Support/Claude/claude_desktop_config.json` (macOS) or the equivalent path on your OS:

```json
{
  "mcpServers": {
    "vcvrack": {
      "type": "http",
      "url": "http://127.0.0.1:2600/mcp"
    }
  }
}
```

Restart Claude Desktop. You will see VCV Rack tools available in the tool panel.

### Cursor / VS Code (via MCP extension)

Add to your MCP settings:

```json
{
  "servers": {
    "vcvrack": {
      "type": "http",
      "url": "http://127.0.0.1:2600/mcp"
    }
  }
}
```

### Manual test (curl)

```bash
# 1. Initialize the MCP session
curl -s -X POST http://127.0.0.1:2600/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}'

# 2. List available tools
curl -s -X POST http://127.0.0.1:2600/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'

# 3. Call a tool
curl -s -X POST http://127.0.0.1:2600/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"vcvrack_get_status","arguments":{}}}'
```

### MCP Tools

| Tool | Description |
|---|---|
| `vcvrack_get_status` | Server version, sample rate, module count |
| `vcvrack_list_modules` | All modules in the current patch |
| `vcvrack_get_module` | Full detail: params, inputs, outputs for one module |
| `vcvrack_add_module` | Add a module `{plugin, slug, x?, y?}` |
| `vcvrack_delete_module` | Remove a module by ID |
| `vcvrack_get_params` | All parameter names, ranges, and values for a module |
| `vcvrack_set_params` | Set one or more parameters `{moduleId, params:[{id, value}]}` |
| `vcvrack_list_cables` | All cable connections |
| `vcvrack_add_cable` | Connect an output to an input |
| `vcvrack_delete_cable` | Remove a cable by ID |
| `vcvrack_get_sample_rate` | Engine sample rate in Hz |
| `vcvrack_search_library` | Search installed plugins by name, slug, or tag |
| `vcvrack_get_plugin` | Full module list for one plugin |
| `vcvrack_save_patch` | Save current patch to a `.vcv` file |
| `vcvrack_load_patch` | Load a `.vcv` file |

### Example prompts for Claude

Once connected, try:

- *"Build me a basic subtractive synth: VCO → VCF → VCA with an ADSR envelope."*
- *"Search for oscillator modules and add a VCO from the Fundamental plugin."*
- *"Set the VCO frequency to 440 Hz and connect it to the audio interface."*
- *"Save the current patch to ~/patches/my_synth.vcv"*

---

## Option B — Claude Code Skills (CLI)

For use with **Claude Code** (the terminal AI assistant), this repo ships ready-made skills in `skills/`.

### Setup

No installation needed — the skills use `skills/vcvrack_client.py` which only requires Python 3 and the standard library.

Check connectivity:

```bash
python skills/vcvrack_client.py status
```

Expected output:

```json
{
  "ok": true,
  "data": {
    "server": "VCV Rack MCP Bridge",
    "version": "1.3.0",
    "sampleRate": 44100.0,
    "moduleCount": 3
  }
}
```

### Using the skill in Claude Code

The skill file at `skills/vcvrack.md` is automatically loaded by Claude Code when you invoke:

```
/vcvrack
```

Or reference it explicitly in your message:

> "Using the vcvrack skill, build a patch with an LFO modulating a VCF cutoff."

### CLI command reference

```bash
python skills/vcvrack_client.py [--port PORT] <command> [args...]
```

| Command | Example | Description |
|---|---|---|
| `status` | `... status` | Check server health |
| `modules` | `... modules` | List all loaded modules |
| `module <id>` | `... module 42` | Inspect params, inputs, outputs |
| `library` | `... library` | Browse all installed plugins |
| `library <query>` | `... library oscillator` | Search by name/tag |
| `add <plugin> <slug>` | `... add Fundamental VCO-1` | Add a module (auto-placed) |
| `add <plugin> <slug> <x> <y>` | `... add Fundamental VCO-1 0 0` | Add at position |
| `remove <id>` | `... remove 42` | Remove a module |
| `params <id>` | `... params 42` | Get all params |
| `set-param <id> <paramId> <value>` | `... set-param 42 0 440` | Set a parameter |
| `cables` | `... cables` | List all cables |
| `connect <outMod> <outPort> <inMod> <inPort>` | `... connect 1 0 2 0` | Connect ports |
| `disconnect <cableId>` | `... disconnect 7` | Remove a cable |
| `save <path>` | `... save ~/patches/my.vcv` | Save patch |
| `load <path>` | `... load ~/patches/my.vcv` | Load patch |

### Example walkthroughs

Ready-to-run examples are in `skills/examples/`:

| File | Patch |
|---|---|
| `examples/01_vco_to_audio.md` | VCO → Audio out (minimal test tone) |
| `examples/02_vco_vcf_vca.md` | VCO → VCF → VCA → Audio (classic subtractive voice) |
| `examples/03_lfo_modulation.md` | LFO modulating VCF cutoff |

### Typical workflow

```
status → library → add → module → connect → set-param → save
```

1. **`status`** — confirm the server is alive
2. **`library <query>`** — find plugin/module slugs
3. **`add`** — place modules; note the returned `id`
4. **`module <id>`** — discover available inputs, outputs, and parameter indices
5. **`connect`** — wire an output to an input
6. **`set-param`** — tune knobs to desired values
7. **`save`** — persist the patch

> **Audio Interface note:** The MCP server cannot configure the audio driver or device — that requires a manual click in VCV Rack's Audio module UI. Add `AudioInterface2` (Core plugin) and connect your signal chain to it; then ask the user to select their driver and device.

---

## REST API Reference

The REST endpoints remain available alongside MCP for scripts and testing.

All responses: `{ "status": "ok", "data": ... }` or `{ "status": "error", "message": "..." }`

| Method | Endpoint | Body / Params | Description |
|---|---|---|---|
| GET | `/status` | — | Server info, sample rate, module count |
| GET | `/modules` | — | List all modules |
| GET | `/modules/:id` | — | Full detail: params + ports |
| POST | `/modules/add` | `{plugin, slug, x?, y?}` | Add a module |
| DELETE | `/modules/:id` | — | Remove a module |
| GET | `/modules/:id/params` | — | Get all param values |
| POST | `/modules/:id/params` | `{params:[{id,value}]}` | Set params |
| GET | `/cables` | — | List all cables |
| POST | `/cables` | `{outputModuleId, outputId, inputModuleId, inputId}` | Connect ports |
| DELETE | `/cables/:id` | — | Disconnect a cable |
| GET | `/sample-rate` | — | Engine sample rate |
| GET | `/library` | `?q=&tags=` | Search plugins + modules |
| GET | `/library/:plugin` | — | All modules in one plugin |
| POST | `/patch/save` | `{path}` | Save patch |
| POST | `/patch/load` | `{path}` | Load patch |
| POST | `/mcp` | JSON-RPC 2.0 body | MCP endpoint |
| GET | `/mcp` | — | MCP SSE stream |

---

## Building from source

### Prerequisites

- CMake 3.21+ **or** GNU Make
- C++17 compiler (GCC 9+, Clang 10+, MSYS2/MinGW on Windows)
- `curl` or `wget` (Makefile path only; CMake uses its own downloader)
- `jq` + `zip` (only for `make dist`)

### Make

```bash
make           # configure + download deps + build
make install   # install to your Rack plugins folder
make dist      # create .vcvplugin archive in dist/
make clean     # remove build/ and dist/
```

Use an existing Rack SDK:

```bash
make RACK_DIR=/path/to/Rack-SDK
```

### CMake

```bash
cmake -B build
cmake --build build --parallel
cmake --install build
```

Pass `-DRACK_DIR=/path/to/Rack-SDK` to use an existing SDK.

---

## CI / Releasing

GitHub Actions builds natively on every push:

| Platform | Runner | Toolchain |
|---|---|---|
| Linux x64 | `ubuntu-latest` | GCC + CMake |
| macOS x64 | `macos-latest` | Clang + CMake |
| macOS ARM64 | `macos-latest` | Clang + CMake (cross-compile) |
| Windows x64 | `windows-latest` | MSYS2/MinGW64 + CMake |

To publish a release, tag the commit with the version from `plugin.json`:

```bash
git tag v2.0.0
git push origin v2.0.0
```

All four platform archives are built and attached to the GitHub Release automatically.

---

## VCV Library submission

1. Fork [VCVRack/community](https://github.com/VCVRack/community)
2. Add your entry to `plugins.json`
3. Open a PR — VCV will build and publish on merge

See the [VCV Library guide](https://vcvrack.com/manual/PluginDevelopmentTutorial#releasing-your-plugin) for details.

---

## License

MIT — see [LICENSE](LICENSE).
Uses [cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT).
