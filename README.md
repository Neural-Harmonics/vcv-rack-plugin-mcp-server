# vcv-rack-mcp-server — VCV Rack 2 × AI/MCP

> Connect Claude (or any MCP client) to VCV Rack 2. Build patches, add modules,
> connect cables and tweak parameters — all through natural language.

```
Claude ↔ MCP Server (Python) ↔ HTTP ↔ vcv-rack-mcp-server plugin (C++) ↔ Rack Engine
```

---

## Installation (end users)

### From VCV Library *(once published)*
1. Open VCV Rack → Library menu → Log in
2. Find **vcv-rack-mcp-server** → click Subscribe
3. Restart Rack — the module appears in the browser under **Utility**

### From GitHub Releases (manual)

1. Go to [Releases](https://github.com/bisegni/vcvrack-mcp-server/releases)
2. Download the `.vcvplugin` for your OS (Windows / macOS / Linux)
3. Double-click it — Rack installs it automatically

---

## Quick start

1. Add the **Rack MCP Server** module to any patch
2. Set the **port** knob (default: **2600**)
3. Click **Enable** — the LED turns green
4. Run the MCP server:
   ```bash
   pip install mcp httpx
   python vcv_rack_mcp_server.py
   ```
5. Add to your Claude Desktop config (`claude_desktop_config.json`):
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
6. Ask Claude: *"Build me a basic subtractive synth: VCO → VCF → VCA with an ADSR envelope."*

---

## Building from source

No manual dependency downloads required — the build system fetches the Rack SDK
and `cpp-httplib` automatically on first run.

### Prerequisites

- CMake 3.21+ **or** GNU Make
- C++17 compiler (GCC 9+, Clang 10+, or MSYS2/MinGW on Windows)
- `curl` or `wget` (for the Makefile path; CMake uses its own downloader)
- `jq` + `zip` (only for `make dist`)

### Using Make (recommended for quick builds)

```bash
make           # configure + download deps + build
make install   # install to your Rack plugins folder
make dist      # create VCVRackMcpServer-VERSION-PLATFORM.vcvplugin in dist/
make clean     # remove build/ and dist/
```

Already have a Rack SDK? Skip the download:

```bash
make RACK_DIR=/path/to/Rack-SDK
```

### Using CMake directly (IDE-friendly)

```bash
cmake -B build
cmake --build build --parallel
cmake --install build
```

Pass `-DRACK_DIR=/path/to/Rack-SDK` to use an existing SDK instead of downloading one.
Downloaded SDKs are cached in `build/_deps/` and reused on subsequent builds.

---

## HTTP API Reference

All responses: `{ "status": "ok", "data": ... }` or `{ "status": "error", "message": "..." }`

| Method | Endpoint | Description |
|---|---|---|
| GET | `/status` | Server alive, sample rate, module count |
| GET | `/modules` | List all modules in patch |
| GET | `/modules/:id` | Full detail: params + ports |
| POST | `/modules/add` | Add a module `{plugin, slug, x?, y?}` |
| DELETE | `/modules/:id` | Remove a module |
| GET | `/modules/:id/params` | Get all param values |
| POST | `/modules/:id/params` | Set params `{params:[{id,value}]}` |
| GET | `/cables` | List all cables |
| POST | `/cables` | Connect ports |
| DELETE | `/cables/:id` | Disconnect a cable |
| GET | `/library` | All installed plugins + modules |
| GET | `/library?q=oscillator` | Free-text module search |
| GET | `/library?tags=VCO,Filter` | Tag-filtered search |
| GET | `/library/:plugin` | All modules in one plugin |
| POST | `/patch/save` | Save patch `{path}` |
| POST | `/patch/load` | Load patch `{path}` |

---

## MCP Tools (AI-facing)

| Tool | Purpose |
|---|---|
| `rack_status` | Check Rack is reachable |
| `rack_list_modules` | See what's in the patch |
| `rack_get_module` | Inspect params & ports |
| `rack_library_search` | Find modules by name/tags |
| `rack_library_plugin` | Browse a plugin's catalogue |
| `rack_add_module` | Add a module |
| `rack_remove_module` | Remove a module |
| `rack_set_params` | Turn knobs |
| `rack_list_cables` | See all connections |
| `rack_connect` | Patch a cable |
| `rack_disconnect` | Remove a cable |
| `rack_save_patch` | Save to disk |
| `rack_load_patch` | Load from disk |

---

## CI / Releasing

GitHub Actions (`.github/workflows/build-plugin.yml`) builds natively on every push:

| Platform | Runner | Toolchain |
| --- | --- | --- |
| Linux x64 | `ubuntu-latest` | GCC + CMake |
| macOS x64 | `macos-latest` | Clang + CMake |
| macOS ARM64 | `macos-latest` | Clang + CMake (cross-compile) |
| Windows x64 | `windows-latest` | MSYS2/MinGW64 + CMake |

To publish a release, tag the commit with a version matching `plugin.json`:

```bash
git tag v2.0.0
git push origin v2.0.0
```

GitHub Actions will build all four platforms and attach the `.vcvplugin` files to a GitHub Release automatically.

---

## VCV Library submission

1. Fork [VCVRack/community](https://github.com/VCVRack/community)
2. Add your entry to `plugins.json`
3. Open a PR — VCV will build and publish the plugin automatically on merge

See the [VCV Library guide](https://vcvrack.com/manual/PluginDevelopmentTutorial#releasing-your-plugin) for full details.

---

## License

MIT — see [LICENSE](LICENSE).
Uses [cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT).
