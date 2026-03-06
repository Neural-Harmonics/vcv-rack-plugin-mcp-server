# vcv-rack-mcp-server

![VCV Rack MCP Server patch demo](docs/demo-1.png)

`vcv-rack-mcp-server` lets AI clients control a live VCV Rack 2 patch through a dedicated Rack module that exposes both HTTP and MCP endpoints. The screenshot above shows the `MCP Server` module running alongside a simple ambient drone patch created through the server and saved back to Rack.

Connect AI clients to VCV Rack 2 through a module that exposes Rack over HTTP and MCP.
With the `MCP Server` module loaded in a patch, an MCP client can inspect the patch, add modules, connect cables, set parameters, and save or load `.vcv` files.

## What this plugin does

The plugin adds one VCV Rack module:

- `MCP Server` (`RackMcpServer`)

When the module is enabled, it starts a local server on `127.0.0.1` using the configured port (default `2600`) and exposes:

- `POST /mcp` for MCP JSON-RPC requests
- `GET /status` and other REST endpoints for scripts and debugging

Typical uses:

- control a Rack patch from Claude Desktop, Cursor, or another MCP client
- script patch-building from the terminal
- inspect installed plugins and module slugs before creating patches programmatically
- save and load patches from automation tools

## How it works

```text
Claude Desktop / Cursor / any MCP client
        |
        | JSON-RPC 2.0  (POST http://127.0.0.1:2600/mcp)
        v
  MCP Server module inside VCV Rack
        |
        | Rack engine API
        v
  Current patch: modules, cables, params, save/load
```

The server only works while:

- VCV Rack is open
- the `MCP Server` module is present in the patch
- the module is switched on

## Installation

### From VCV Library

Once the plugin is approved in the VCV Library:

1. Open VCV Rack 2.
2. Sign in to your VCV account.
3. Open the Library and subscribe to `MCP Server` by `Neural Harmonics`.
4. Restart Rack if needed.
5. Add the module from the browser under `Utility`.

### From GitHub Releases

1. Open the [GitHub Releases](https://github.com/Neural-Harmonics/vcv-rack-plugin-mcp-server/releases) page.
2. Download the `.zip` file for your platform.
3. Unzip it. You should get a folder named `VCVRackMcpServer`.
4. Move that folder into your Rack 2 plugins folder:

```text
macOS Apple Silicon: ~/Library/Application Support/Rack2/plugins-mac-arm64/
macOS Intel: ~/Library/Application Support/Rack2/plugins-mac-x64/
Linux x64: ~/.local/share/Rack2/plugins-lin-x64/
```

5. Restart Rack and add the module from the browser.

### Build your own release package

If you are building from source and want the same installable artifact used on GitHub Releases:

```bash
make dist
```

This creates a `.zip` archive in `dist/`, for example:

```text
dist/VCVRackMcpServer-2.1.0-mac-arm64.zip
```

Unzip that file and move the extracted `VCVRackMcpServer` folder into your Rack plugins directory.

## Quick start in Rack

1. Open VCV Rack 2.
2. Create or open a patch.
3. Add the `MCP Server` module.
4. Leave the port at `2600` unless you need a different port.
5. Toggle `ON/OFF` until the status LED turns green.
6. Keep this patch open while your client connects.

The server will now listen at:

```text
http://127.0.0.1:2600
```

## First patch walkthrough

If you want a quick smoke test, build this minimal patch:

```text
Fundamental VCO -> Core AudioInterface2
```

1. Start VCV Rack and add `MCP Server`.
2. Turn the module on and confirm the LED is green.
3. Add a `Core AudioInterface2` module in Rack so audio has somewhere to go.
4. Configure your audio driver and output device manually in Rack if you have not done that already.
5. Use one of the commands below to add a `Fundamental VCO`.
6. Connect the VCO output to the left and right inputs on `AudioInterface2`.
7. Lower your system volume before testing, then adjust the oscillator frequency to taste.

Using the included CLI helper, the flow looks like this:

```bash
python3 skills/vcvrack_client.py status
python3 skills/vcvrack_client.py library Fundamental
python3 skills/vcvrack_client.py library Core
python3 skills/vcvrack_client.py add Fundamental VCO
python3 skills/vcvrack_client.py add Core AudioInterface2
python3 skills/vcvrack_client.py modules
python3 skills/vcvrack_client.py connect <vco_id> 0 <audio_id> 0
python3 skills/vcvrack_client.py connect <vco_id> 0 <audio_id> 1
```

After the patch is working, save it:

```bash
python3 skills/vcvrack_client.py save ~/Documents/Rack2/patches/first-test.vcv
```

## How to work with this plugin

A reliable workflow is:

```text
Start Rack -> add MCP Server -> turn it on -> verify /status -> connect your AI client -> search library -> add modules -> inspect module IDs and params -> connect cables -> save patch
```

Two practical rules matter a lot for AI-driven patch building:

- Rack edits are executed on Rack's UI thread. If Rack is busy, hidden behind a modal, or otherwise not stepping normally, MCP calls can time out even when the request is valid.
- Parameter values are often raw knob positions, not musical units. Always inspect a module's parameter metadata before trying to set "65 Hz", "200 ms", "saw", or similar concepts directly.

### 1. Verify the server is alive

From a terminal:

```bash
curl -s http://127.0.0.1:2600/status | python3 -m json.tool
```

You should get a JSON response with sample rate and module count.

### 2. Discover available modules before building

Ask the server what plugins and modules are installed:

```bash
curl -s "http://127.0.0.1:2600/library?q=oscillator" | python3 -m json.tool
curl -s "http://127.0.0.1:2600/library/Fundamental" | python3 -m json.tool
```

This is important because VCV Rack automation depends on exact plugin and module slugs.

### 3. Add modules and inspect them

Example: add a VCO and inspect its details.

```bash
curl -s -X POST http://127.0.0.1:2600/modules/add \
  -H "Content-Type: application/json" \
  -d '{"plugin":"Fundamental","slug":"VCO"}' | python3 -m json.tool

curl -s http://127.0.0.1:2600/modules | python3 -m json.tool
curl -s http://127.0.0.1:2600/modules/1 | python3 -m json.tool
```

Use the returned module IDs when wiring cables or setting parameters.

### 4. Connect cables and tune parameters

Example requests:

```bash
curl -s -X POST http://127.0.0.1:2600/cables \
  -H "Content-Type: application/json" \
  -d '{"outputModuleId":1,"outputId":0,"inputModuleId":2,"inputId":0}' | python3 -m json.tool

curl -s -X POST http://127.0.0.1:2600/modules/1/params \
  -H "Content-Type: application/json" \
  -d '{"params":[{"id":0,"value":0.0}]}' | python3 -m json.tool
```

Important notes for parameter automation:

- Call `GET /modules/:id/params` first and use the returned `name`, `min`, `max`, `value`, `displayValue`, and optional `options` fields as the source of truth.
- Do not assume parameter `0` means frequency in Hz or that a value like `65` means 65 Hz. Many modules expose normalized or module-specific control ranges.
- Prefer small `POST /modules/:id/params` batches, then read the params again to confirm the change before continuing.
- If a write times out, first confirm Rack is responsive and the `MCP Server` module is still enabled, then retry with a smaller step.

### 5. Save your patch

```bash
curl -s -X POST http://127.0.0.1:2600/patch/save \
  -H "Content-Type: application/json" \
  -d '{"path":"/tmp/my-patch.vcv"}' | python3 -m json.tool
```

## Use with MCP clients

### Claude Desktop

Add this to `~/Library/Application Support/Claude/claude_desktop_config.json` on macOS, or the equivalent config path on your platform:

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

Then:

1. restart Claude Desktop
2. open VCV Rack with the `MCP Server` module enabled
3. ask Claude to inspect or build a patch

Example prompts:

- `List the modules currently in my Rack patch.`
- `Search the installed Rack library for oscillators and add a good starting VCO.`
- `Build a simple subtractive synth with VCO, VCF, VCA, ADSR, and Audio.`
- `Build a simple ambient drone. Inspect each module's params before setting them, and treat displayed param values as authoritative instead of guessing in Hz.`
- `Save the current patch to ~/Documents/Rack2/patches/test.vcv`.

## Troubleshooting MCP timeouts

If an MCP tool such as `vcvrack_set_params` reports a timeout:

1. Make sure VCV Rack is still open, the patch is active, and the `MCP Server` module LED is green.
2. Check that Rack is responsive and not blocked by a dialog, browser search field, menu, or file picker.
3. Retry the workflow in this order: `vcvrack_get_status` -> `vcvrack_get_module` -> `vcvrack_get_params` -> `vcvrack_set_params`.
4. Write one or two params at a time and keep values inside the reported `min` and `max` range.
5. Re-read params after each write instead of assuming the model guessed the control mapping correctly.

### Cursor or another MCP client

Use the same server URL:

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

## Use with the included CLI helper

This repo includes [`skills/vcvrack_client.py`](skills/vcvrack_client.py), a small Python CLI for talking to the local Rack server.

Check connectivity:

```bash
python3 skills/vcvrack_client.py status
```

Command format:

```bash
python3 skills/vcvrack_client.py [--port PORT] <command> [args...]
```

Common commands:

| Command | Example | What it does |
|---|---|---|
| `status` | `python3 skills/vcvrack_client.py status` | Check server health |
| `modules` | `python3 skills/vcvrack_client.py modules` | List modules in the patch |
| `module <id>` | `python3 skills/vcvrack_client.py module 42` | Inspect one module |
| `library` | `python3 skills/vcvrack_client.py library` | List installed plugins |
| `library <plugin>` | `python3 skills/vcvrack_client.py library Fundamental` | List modules in one plugin |
| `add <plugin> <slug>` | `python3 skills/vcvrack_client.py add Fundamental VCO` | Add a module |
| `remove <id>` | `python3 skills/vcvrack_client.py remove 42` | Remove a module |
| `params <id>` | `python3 skills/vcvrack_client.py params 42` | List parameters |
| `set-param <id> <paramId> <value> ...` | `python3 skills/vcvrack_client.py set-param 42 0 0.0` | Set parameters |
| `cables` | `python3 skills/vcvrack_client.py cables` | List cable connections |
| `connect <outMod> <outPort> <inMod> <inPort>` | `python3 skills/vcvrack_client.py connect 1 0 2 0` | Create a cable |
| `disconnect <cableId>` | `python3 skills/vcvrack_client.py disconnect 7` | Remove a cable |
| `save <path>` | `python3 skills/vcvrack_client.py save /tmp/test.vcv` | Save patch |
| `load <path>` | `python3 skills/vcvrack_client.py load /tmp/test.vcv` | Load patch |

Ready-made walkthroughs live in `skills/examples/`.

## REST API reference

All responses use one of these envelopes:

```json
{ "status": "ok", "data": ... }
```

```json
{ "status": "error", "message": "..." }
```

| Method | Endpoint | Body / Query | Description |
|---|---|---|---|
| `GET` | `/status` | - | Server info, sample rate, module count |
| `GET` | `/modules` | - | List all modules |
| `GET` | `/modules/:id` | - | Inspect one module |
| `POST` | `/modules/add` | `{plugin, slug, x?, y?, nearModuleId?}` | Add a module |
| `DELETE` | `/modules/:id` | - | Remove a module |
| `GET` | `/modules/:id/params` | - | List parameter values |
| `POST` | `/modules/:id/params` | `{params:[{id,value}]}` | Set parameters |
| `GET` | `/cables` | - | List cables |
| `POST` | `/cables` | `{outputModuleId, outputId, inputModuleId, inputId}` | Create a cable |
| `DELETE` | `/cables/:id` | - | Remove a cable |
| `GET` | `/sample-rate` | - | Return sample rate |
| `GET` | `/library` | `?q=&tags=` | Search installed plugins/modules |
| `GET` | `/library/:plugin` | - | List one plugin's modules |
| `POST` | `/patch/save` | `{path}` | Save patch |
| `POST` | `/patch/load` | `{path}` | Load patch |
| `POST` | `/mcp` | JSON-RPC body | MCP endpoint |
| `GET` | `/mcp` | - | SSE stream |

## Building from source

### Prerequisites

- CMake 3.21+ or GNU Make
- a C++17 compiler
- `curl` or `wget` if you use the `Makefile` path
- `jq` and `zip` for `make dist`

### Build with Make

```bash
make
make install
make dist
```

Use an existing Rack SDK if you already have one:

```bash
make RACK_DIR=/path/to/Rack-SDK
```

### Build with CMake

```bash
cmake -B build
cmake --build build --parallel
cmake --install build
```

## Releasing

1. Update `plugin.json` to the new version.
2. Commit and push.
3. Build release artifacts or let CI build them.
4. Create a GitHub release for that version.

If you tag releases from git, keep the tag aligned with `plugin.json`, for example:

```bash
git tag v2.1.0
git push origin v2.1.0
```

## Publishing to the VCV Library

For an open-source plugin, the normal path is through the [`VCVRack/library`](https://github.com/VCVRack/library) repository.

1. Open an issue in `VCVRack/library`.
2. Use the plugin slug as the issue title: `VCVRackMcpServer`.
3. Include the source repository URL: `https://github.com/Neural-Harmonics/vcv-rack-plugin-mcp-server`.
4. Wait for the maintainer to review and add the plugin.
5. For later updates, comment on the same issue with the new version and commit SHA.

The metadata used for approval comes primarily from `plugin.json`, so keep its URLs, version, and module information current.

## Notes and limitations

- The plugin controls the Rack patch, not Rack application settings.
- Audio driver and device selection still need to be configured manually in Rack.
- The server is local-only by default and intended for use on the same machine.
- Exact plugin/module slugs depend on what is installed in your Rack library.

## License

MIT. See [LICENSE](LICENSE).

This project also uses [cpp-httplib](https://github.com/yhirose/cpp-httplib), which is MIT-licensed.
