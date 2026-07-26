#!/bin/bash
# build_dxt.sh — builds vcvrack-mcp-server.mcpb (Claude Desktop Extension)
#
# Usage: ./build_dxt.sh [--version X.Y.Z]
#   --version   override version (default: read from plugin.json)

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── Parse args ──────────────────────────────────────────────────────────────
VERSION=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# Read version from plugin.json if not overridden
if [[ -z "$VERSION" ]]; then
    VERSION=$(python3 -c "import json,sys; print(json.load(open('$SCRIPT_DIR/plugin.json'))['version'])")
fi

echo "Building vcvrack-mcp-server v$VERSION ..."

# ── Staging dir ─────────────────────────────────────────────────────────────
STAGE=$(mktemp -d)
trap "rm -rf $STAGE" EXIT

# ── manifest.json ───────────────────────────────────────────────────────────
cat > "$STAGE/manifest.json" << EOF
{
  "dxt_version": "0.1",
  "name": "vcvrack-mcp-server",
  "display_name": "VCV Rack MCP Server",
  "version": "$VERSION",
  "description": "Control VCV Rack from Claude. Build patches, add modules, connect cables and tweak parameters using natural language.",
  "long_description": "Connects Claude Desktop to a running VCV Rack instance via the MCP Server module (Neural Harmonics). Once the module is loaded in your rack and toggled ON, Claude can build complete patches, search the plugin library, place and wire modules, adjust parameters, and save/load patches.",
  "author": {
    "name": "Claudio Bisegni",
    "url": "https://github.com/Neural-Harmonics"
  },
  "repository": {
    "type": "git",
    "url": "https://github.com/Neural-Harmonics/vcv-rack-plugin"
  },
  "documentation": "https://github.com/Neural-Harmonics/vcv-rack-plugin/blob/main/README.md",
  "icon": "icon.png",
  "server": {
    "type": "python",
    "entry_point": "vcvrack_client.py",
    "mcp_config": {
      "command": "python3",
      "args": [
        "\${__dirname}/vcvrack_client.py",
        "--mcp-server",
        "--port",
        "\${user_config.port}"
      ]
    }
  },
  "user_config": {
    "port": {
      "type": "number",
      "title": "Server Port",
      "description": "Port configured in the MCP Server module inside VCV Rack (default: 2600).",
      "default": 2600,
      "required": false
    }
  },
  "tools": [
    { "name": "vcvrack_get_status",         "description": "Get VCV Rack server status" },
    { "name": "vcvrack_get_rack_layout",    "description": "Get rack spatial layout and suggested positions" },
    { "name": "vcvrack_search_library",     "description": "Search installed plugins and modules" },
    { "name": "vcvrack_add_module",         "description": "Add a module to the patch" },
    { "name": "vcvrack_delete_module",      "description": "Remove a module" },
    { "name": "vcvrack_add_cable",          "description": "Connect two ports with a cable" },
    { "name": "vcvrack_delete_cable",       "description": "Remove a cable" },
    { "name": "vcvrack_get_params",         "description": "Get module parameters" },
    { "name": "vcvrack_set_params",         "description": "Set module parameters" },
    { "name": "vcvrack_list_modules",       "description": "List all modules in the patch" },
    { "name": "vcvrack_list_cables",        "description": "List all cables in the patch" }
  ],
  "compatibility": {
    "claude_desktop": ">=0.10.0",
    "platforms": ["darwin", "win32", "linux"]
  }
}
EOF

# ── Package ──────────────────────────────────────────────────────────────────
OUT="$SCRIPT_DIR/vcvrack-mcp-server.mcpb"
rm -f "$OUT"
cp "$SCRIPT_DIR/dxt/vcvrack_client.py" "$STAGE/vcvrack_client.py"
if [[ -f "$SCRIPT_DIR/dxt/icon.png" ]]; then
    cp "$SCRIPT_DIR/dxt/icon.png" "$STAGE/icon.png"
fi
cd "$STAGE"
zip -r "$OUT" .

echo "Built: $OUT ($(du -sh "$OUT" | cut -f1))"
