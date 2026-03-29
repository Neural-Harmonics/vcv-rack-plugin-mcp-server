/*
 * RackMcpServer.cpp
 * VCV Rack 2 Plugin — MCP HTTP Bridge
 */

#include "RackMcpServer.hpp"

// cpp-httplib (header-only)
#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include <httplib.h>

// Rack headers
#include <rack.hpp>
#include <app/RackWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <tag.hpp>

#include <sstream>
#include <cstring>
#include <functional>
#include <set>

using namespace rack;

// ─── UI Task Queue ─────────────────────────────────────────────────────────

std::future<void> UITaskQueue::post(std::function<void()> fn, const std::string& label) {
    auto p = std::make_shared<std::promise<void>>();
    {
        std::lock_guard<std::mutex> lock(mutex);
        tasks.push({fn, p, label});
    }
    return p->get_future();
}

bool UITaskQueue::postSync(std::function<void()> fn, const std::string& label, int timeoutSecs) {
    auto fut = post(fn, label);
    if (fut.wait_for(std::chrono::seconds(timeoutSecs)) == std::future_status::timeout) {
        WARN("[RackMcpServer] postSync: task '%s' timed out after %ds (UI thread not draining)",
             label.c_str(), timeoutSecs);
        return false;
    }
    try { fut.get(); } catch (...) {}
    return true;
}

void UITaskQueue::drain() {
    std::queue<Task> local;
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::swap(local, tasks);
    }
    while (!local.empty()) {
        auto& task = local.front();
        const std::string& lbl = task.label.empty() ? "(unlabelled)" : task.label;
        INFO("[RackMcpServer] drain: executing task '%s'", lbl.c_str());
        try {
            task.fn();
            task.promise->set_value();
            INFO("[RackMcpServer] drain: task '%s' completed", lbl.c_str());
        } catch (const std::exception& e) {
            WARN("[RackMcpServer] drain: task '%s' threw exception: %s", lbl.c_str(), e.what());
            task.promise->set_exception(std::current_exception());
        } catch (...) {
            WARN("[RackMcpServer] drain: task '%s' threw unknown exception", lbl.c_str());
            task.promise->set_exception(std::current_exception());
        }
        local.pop();
    }
}

// ─── tiny JSON builder helpers ─────────────────────────────────────────────

// ─── JSON helpers ─────────────────────────────────────────────────────────────

static std::string jsonStr(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out + "\"";
}

static std::string jsonKV(const std::string& k, const std::string& v, bool last = false) {
    return jsonStr(k) + ": " + v + (last ? "" : ", ");
}

static std::string jsonKVs(const std::string& k, const std::string& s, bool last = false) {
    return jsonStr(k) + ": " + jsonStr(s) + (last ? "" : ", ");
}

static std::string ok(const std::string& body) {
    return "{" + jsonKVs("status", "ok") + jsonKV("data", body, true) + "}";
}

static std::string err(const std::string& msg) {
    return "{" + jsonKVs("status", "error") + jsonKVs("message", msg, true) + "}";
}

// ─── Module serialisation helpers ─────────────────────────────────────────

static std::string serializeParamQuantity(ParamQuantity* pq, int paramId) {
    if (!pq) return "{}";
    std::string s = "{";
    s += jsonKV("id", std::to_string(paramId));
    s += jsonKVs("name", pq->name);
    s += jsonKVs("unit", pq->unit);
    s += jsonKV("min", std::to_string(pq->minValue));
    s += jsonKV("max", std::to_string(pq->maxValue));
    s += jsonKV("default", std::to_string(pq->defaultValue));
    s += jsonKV("value", std::to_string(pq->getValue()));
    s += jsonKVs("displayValue", pq->getDisplayValueString());
    
    // Check if it's a switch with labels
    SwitchQuantity* sq = dynamic_cast<SwitchQuantity*>(pq);
    if (sq && !sq->labels.empty()) {
        s += jsonStr("options") + ": [";
        for (size_t i = 0; i < sq->labels.size(); i++) {
            s += jsonStr(sq->labels[i]) + (i < sq->labels.size() - 1 ? ", " : "");
        }
        s += "], ";
    }
    
    s += jsonKV("snap", pq->snapEnabled ? "true" : "false", true);
    s += "}";
    return s;
}

static std::string serializePortInfo(engine::Port* port, PortInfo* pi, int portId, bool isInput) {
    std::string s = "{";
    s += jsonKV("id", std::to_string(portId));
    s += jsonKVs("name", pi ? pi->name : "");
    s += jsonKVs("description", pi ? pi->description : "");
    s += jsonKVs("type", isInput ? "input" : "output");
    if (port) {
        s += jsonKV("connected", port->isConnected() ? "true" : "false");
        s += jsonKV("channels", std::to_string((int)port->getChannels()));
        s += jsonKV("voltage", std::to_string(port->getVoltage()), true);
    } else {
        s += jsonKV("connected", "false", true);
    }
    s += "}";
    return s;
}

static std::string serializeModuleDetail(engine::Module* mod) {
    if (!mod) return "null";
    std::string s = "{";
    s += jsonKV("id", std::to_string(mod->id));
    if (mod->model) {
        s += jsonKVs("plugin", mod->model->plugin ? mod->model->plugin->slug : "");
        s += jsonKVs("slug", mod->model->slug);
        s += jsonKVs("name", mod->model->name);
    }
    app::ModuleWidget* mw = APP->scene->rack->getModule(mod->id);
    if (mw) {
        s += jsonKV("x", std::to_string(mw->box.pos.x));
        s += jsonKV("y", std::to_string(mw->box.pos.y));
    }
    s += jsonStr("params") + ": [";
    for (int i = 0; i < (int)mod->params.size(); i++) {
        s += serializeParamQuantity(mod->paramQuantities[i], i);
        if (i < (int)mod->params.size() - 1) s += ", ";
    }
    s += "], " + jsonStr("inputs") + ": [";
    for (int i = 0; i < (int)mod->inputs.size(); i++) {
        s += serializePortInfo(&mod->inputs[i], i < (int)mod->inputInfos.size() ? mod->inputInfos[i] : nullptr, i, true);
        if (i < (int)mod->inputs.size() - 1) s += ", ";
    }
    s += "], " + jsonStr("outputs") + ": [";
    for (int i = 0; i < (int)mod->outputs.size(); i++) {
        s += serializePortInfo(&mod->outputs[i], i < (int)mod->outputInfos.size() ? mod->outputInfos[i] : nullptr, i, false);
        if (i < (int)mod->outputs.size() - 1) s += ", ";
    }
    s += "]}";
    return s;
}

static std::string serializeModuleSummary(engine::Module* mod) {
    if (!mod) return "null";
    std::string s = "{";
    s += jsonKV("id", std::to_string(mod->id));
    if (mod->model) {
        s += jsonKVs("plugin", mod->model->plugin ? mod->model->plugin->slug : "");
        s += jsonKVs("slug", mod->model->slug);
        s += jsonKVs("name", mod->model->name);
        s += jsonKV("params", std::to_string(mod->params.size()));
        s += jsonKV("inputs", std::to_string(mod->inputs.size()));
        s += jsonKV("outputs", std::to_string(mod->outputs.size()));
        app::ModuleWidget* mw = APP->scene->rack->getModule(mod->id);
        if (mw) {
            s += jsonKV("x", std::to_string((int)mw->box.pos.x));
            s += jsonKV("y", std::to_string((int)mw->box.pos.y));
            s += jsonKV("width", std::to_string((int)mw->box.size.x), true);
        } else {
            s += jsonKV("x", "0");
            s += jsonKV("y", "0");
            s += jsonKV("width", "0", true);
        }
    }
    s += "}";
    return s;
}

static std::string serializeModel(plugin::Model* model) {
    std::string s = "{";
    s += jsonKVs("slug", model->slug);
    s += jsonKVs("name", model->name);
    s += jsonStr("tags") + ": [";
    bool firstTag = true;
    for (int tagId : model->tagIds) {
        if (!firstTag) s += ", ";
        s += jsonStr(rack::tag::getTag(tagId));
        firstTag = false;
    }
    s += "], " + jsonKVs("description", model->description, true) + "}";
    return s;
}

static std::string serializePlugin(plugin::Plugin* plug) {
    std::string s = "{";
    s += jsonKVs("slug", plug->slug);
    s += jsonKVs("name", plug->name);
    s += jsonKVs("author", plug->author);
    s += jsonKVs("version", plug->version);
    s += jsonStr("modules") + ": [";
    bool firstModel = true;
    for (plugin::Model* m : plug->models) {
        if (!firstModel) s += ", ";
        s += serializeModel(m);
        firstModel = false;
    }
    s += "]}";
    return s;
}

// ─── Simple JSON parser ────────────────────────────────────────────────────

static std::string parseJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    auto pos = json.find(searchKey);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + searchKey.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

static double parseJsonDouble(const std::string& json, const std::string& key, double def = 0.0) {
    std::string searchKey = "\"" + key + "\"";
    auto pos = json.find(searchKey);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos + searchKey.size());
    if (pos == std::string::npos) return def;
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ')) pos++;
    // Handle quoted string numbers (AI sometimes sends numbers as JSON strings)
    if (pos < json.size() && json[pos] == '"') pos++;
    try { return std::stod(json.substr(pos)); } catch (...) { return def; }
}

// Extract raw JSON value (string, number, object, array, bool, null) by key
static std::string parseRawValue(const std::string& json, const std::string& key) {
    std::string sk = "\"" + key + "\"";
    auto p = json.find(sk);
    if (p == std::string::npos) return "";
    p += sk.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == ':' || json[p] == '\t' || json[p] == '\n')) p++;
    if (p >= json.size()) return "";
    char c = json[p];
    if (c == '"') {
        auto e = p + 1;
        while (e < json.size()) {
            if (json[e] == '\\') { e += 2; continue; }
            if (json[e] == '"') break;
            e++;
        }
        return json.substr(p, e - p + 1);
    } else if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int depth = 0;
        auto start = p;
        bool inStr = false;
        for (; p < json.size(); p++) {
            char ch = json[p];
            if (ch == '\\') { p++; continue; }
            if (ch == '"') { inStr = !inStr; continue; }
            if (inStr) continue;
            if (ch == open) depth++;
            else if (ch == close && --depth == 0) return json.substr(start, p - start + 1);
        }
        return "";
    } else {
        auto e = p;
        while (e < json.size() && json[e] != ',' && json[e] != '}' && json[e] != ']'
               && json[e] != ' ' && json[e] != '\n' && json[e] != '\r') e++;
        return json.substr(p, e - p);
    }
}

// Extract the JSON-RPC "id" field as raw JSON (preserves number/string/null type)
static std::string parseJsonRpcId(const std::string& body) {
    std::string key = "\"id\"";
    auto pos = body.find(key);
    if (pos == std::string::npos) return "null";
    pos += key.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':' || body[pos] == '\t')) pos++;
    if (pos >= body.size()) return "null";
    char c = body[pos];
    if (c == '"') {
        auto end = pos + 1;
        while (end < body.size()) {
            if (body[end] == '\\') { end += 2; continue; }
            if (body[end] == '"') break;
            end++;
        }
        return body.substr(pos, end - pos + 1);
    } else if (c == 'n') {
        return "null";
    } else {
        auto end = pos;
        while (end < body.size() && body[end] != ',' && body[end] != '}'
               && body[end] != ' ' && body[end] != '\r' && body[end] != '\n') end++;
        return body.substr(pos, end - pos);
    }
}

// ─── JSON-RPC 2.0 / MCP response builders ─────────────────────────────────

static std::string mcpOk(const std::string& id, const std::string& result) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}";
}

static std::string mcpErr(const std::string& id, int code, const std::string& msg) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id +
           ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":" + jsonStr(msg) + "}}";
}

static std::string toolOk(const std::string& text) {
    return "{\"content\":[{\"type\":\"text\",\"text\":" + jsonStr(text) + "}],\"isError\":false}";
}

static std::string toolFail(const std::string& text) {
    return "{\"content\":[{\"type\":\"text\",\"text\":" + jsonStr(text) + "}],\"isError\":true}";
}

static std::string getMcpModulePositionJson(app::RackWidget* rackWidget, int64_t mcpId) {
    if (!rackWidget || mcpId < 0) {
        return "{" + jsonKV("id", std::to_string(mcpId))
                   + jsonKV("x", "0")
                   + jsonKV("y", "0")
                   + jsonKV("found", "false", true) + "}";
    }
    app::ModuleWidget* mw = rackWidget->getModule(mcpId);
    if (!mw) {
        return "{" + jsonKV("id", std::to_string(mcpId))
                   + jsonKV("x", "0")
                   + jsonKV("y", "0")
                   + jsonKV("found", "false", true) + "}";
    }
    return "{" + jsonKV("id", std::to_string(mcpId))
               + jsonKV("x", std::to_string((int)mw->box.pos.x))
               + jsonKV("y", std::to_string((int)mw->box.pos.y))
               + jsonKV("found", "true", true) + "}";
}

static bool isTerminalOutputModule(const app::ModuleWidget* mw) {
    if (!mw || !mw->model || !mw->model->plugin) return false;
    const std::string pluginSlug = mw->model->plugin->slug;
    const std::string modelSlug = mw->model->slug;
    if (pluginSlug == "Core" &&
        (modelSlug == "AudioInterface2" || modelSlug == "AudioInterface" || modelSlug == "AudioInterface16")) {
        return true;
    }
    return false;
}

struct LayoutPrefs {
    int colsHp = 0;
    int rows = 0;
};

struct RowInfo {
    float y = 0.f;
    float left = 0.f;
    float right = 0.f;
    int count = 0;
};

static LayoutPrefs readLayoutPrefs(const RackMcpServer* parent) {
    if (!parent) return {};
    std::lock_guard<std::mutex> lock(const_cast<RackMcpServer*>(parent)->layoutPrefsMutex);
    LayoutPrefs prefs;
    prefs.colsHp = std::max(0, parent->layoutMatrixColsHp);
    prefs.rows = std::max(0, parent->layoutMatrixRows);
    return prefs;
}

static int roundRowIndex(float y) {
    return (int)std::round(y / RACK_GRID_HEIGHT);
}

static int alignedRowY(int rowIdx) {
    return rowIdx * (int)RACK_GRID_HEIGHT;
}

static int hpToPx(int hp) {
    return hp * (int)RACK_GRID_WIDTH;
}

static bool isWithinRowBounds(int rowIdx, int mcpRowIdx, const LayoutPrefs& prefs) {
    if (prefs.rows <= 0) return true;
    return rowIdx >= mcpRowIdx && rowIdx < (mcpRowIdx + prefs.rows);
}

static bool isWithinColBounds(float x, float width, float mcpX, const LayoutPrefs& prefs) {
    if (prefs.colsHp <= 0) return true;
    float maxXExclusive = mcpX + (float)hpToPx(prefs.colsHp);
    return x >= mcpX && (x + std::max(0.f, width)) <= maxXExclusive;
}

// ─── MCP tools list (JSON Schema for each tool) ────────────────────────────

static const char* MCP_TOOLS_JSON = R"json([
{"name":"vcvrack_get_status","description":"Get VCV Rack server status: version, sample rate, and loaded module count.","inputSchema":{"type":"object","properties":{}}},
{"name":"vcvrack_get_mcp_position","description":"Get the MCP Server module's root position in rack pixel coordinates. Use this as the starting anchor before placing related modules.","inputSchema":{"type":"object","properties":{}}},
{"name":"vcvrack_get_layout_preferences","description":"Get user matrix preferences anchored at the MCP module. matrix_cols_hp is width in HP (15 px/HP) from MCP x, matrix_rows is row count starting at MCP row. 0 means unbounded.","inputSchema":{"type":"object","properties":{}}},
{"name":"vcvrack_set_layout_preferences","description":"Set user matrix preferences anchored at the MCP module. matrix_cols_hp is width in HP (15 px/HP) from MCP x, matrix_rows is row count starting at MCP row. Set either to 0 for unbounded.","inputSchema":{"type":"object","properties":{"matrix_cols_hp":{"type":"integer","minimum":0},"matrix_rows":{"type":"integer","minimum":0}},"required":["matrix_cols_hp","matrix_rows"]}},
{"name":"vcvrack_get_rack_layout","description":"Get a spatial map of the rack: all rows, their occupied x ranges, the MCP module position, current matrix_preferences, and ready-to-use suggested_positions for placing new modules. ALWAYS call this before vcvrack_add_module to get explicit x/y coordinates. Never rely on auto-placement.\n\nResponse fields:\n- grid_unit_px: 1 HP = 15 px (standard VCV Rack unit)\n- row_height_px: 380 px per row (one 3U row)\n- mcp_module: the MCP Server module's current x/y position. MCP is always the world anchor.\n- matrix_preferences: user-defined bounds relative to MCP anchor.\n- rows[]: each row has y (top of row), left_edge, right_edge (first free x in that row), module_count\n- suggested_positions[]: insertion points constrained by matrix_preferences. Prefer 'append_mcp_row' for modules that belong with the main MCP section, and prefer 'insert_before_output' when the row already ends with Audio Interface modules so outputs stay on the far right.\n\nBatching: when adding several modules to the same row, compute the next x yourself as: next_x = previous_x + width_returned_by_add. This avoids calling layout again between each add.","inputSchema":{"type":"object","properties":{}}},
{"name":"vcvrack_list_modules","description":"List all modules currently loaded in the VCV Rack patch. Each entry includes id, plugin, slug, name, param/input/output counts, x position, y position, and width (in pixels). Use x + width to compute where the next module should go in the same row.","inputSchema":{"type":"object","properties":{}}},
{"name":"vcvrack_get_module","description":"Get detailed information about a specific module: all parameters (with value ranges), inputs, and outputs.","inputSchema":{"type":"object","properties":{"module_id":{"type":"integer","description":"Module ID"}},"required":["module_id"]}},
{"name":"vcvrack_add_module","description":"Add a new module to the VCV Rack patch at an explicit pixel position.\n\nMANDATORY PLACEMENT WORKFLOW — follow this every time:\n1. Call vcvrack_get_rack_layout to get mcp_module, rows, and suggested_positions.\n2. Use mcp_module as your spatial anchor to decide whether the module belongs on the MCP row, another existing row, or a new aligned row.\n3. Prefer 'append_mcp_row' for modules related to the main patch section. If the row already ends with Audio Interface modules, prefer 'insert_before_output' so Audio stays at the far right.\n4. Pass the chosen x and y values here. Both x and y are required.\n5. The response includes the new module's width. Store it: next_x = x + width for the following module in the same row.\n\nNever omit x/y. Never auto-place. Consistent explicit positioning keeps the rack readable.\n\nUse vcvrack_search_library to discover valid plugin/slug values before adding.","inputSchema":{"type":"object","properties":{"plugin_slug":{"type":"string","description":"Plugin slug (e.g. 'Fundamental')"},"module_slug":{"type":"string","description":"Module slug (e.g. 'VCO-1')"},"x":{"type":"number","description":"X position in pixels — obtain from vcvrack_get_rack_layout suggested_positions, or compute as previous_x + previous_width"},"y":{"type":"number","description":"Y position in pixels — obtain from vcvrack_get_rack_layout suggested_positions (e.g. 0 for row 0, 380 for row 1)"}},"required":["plugin_slug","module_slug","x","y"]}},
{"name":"vcvrack_delete_module","description":"Delete a module from the VCV Rack patch by ID.","inputSchema":{"type":"object","properties":{"id":{"type":"integer","description":"Module ID to delete"}},"required":["id"]}},
{"name":"vcvrack_get_params","description":"Get all parameters of a module with names, value ranges, current raw values, display strings, and switch options when available. Use this before setting params because many Rack controls are normalized knob values rather than Hz or seconds.","inputSchema":{"type":"object","properties":{"moduleId":{"type":"integer","description":"Module ID"}},"required":["moduleId"]}},
{"name":"vcvrack_set_params","description":"Set one or more parameters on a module. Always call vcvrack_get_params first to discover parameter IDs, min/max ranges, display strings, and switch options. Prefer small batches, keep values inside the reported min/max range, and re-read params after writing to confirm the change applied.","inputSchema":{"type":"object","properties":{"moduleId":{"type":"integer","description":"Module ID"},"params":{"type":"array","description":"Array of parameter updates","items":{"type":"object","properties":{"id":{"type":"integer","description":"Parameter index (0-based)"},"value":{"type":"number","description":"New parameter value"}},"required":["id","value"]}}},"required":["moduleId","params"]}},
{"name":"vcvrack_list_cables","description":"List all cable connections in the current patch.","inputSchema":{"type":"object","properties":{}}},
{"name":"vcvrack_add_cable","description":"Connect an output port to an input port with a patch cable.","inputSchema":{"type":"object","properties":{"outputModuleId":{"type":"integer","description":"Source module ID"},"outputId":{"type":"integer","description":"Output port index (0-based)"},"inputModuleId":{"type":"integer","description":"Destination module ID"},"inputId":{"type":"integer","description":"Input port index (0-based)"}},"required":["outputModuleId","outputId","inputModuleId","inputId"]}},
{"name":"vcvrack_delete_cable","description":"Remove a cable connection by cable ID.","inputSchema":{"type":"object","properties":{"id":{"type":"integer","description":"Cable ID"}},"required":["id"]}},
{"name":"vcvrack_get_sample_rate","description":"Get the current audio engine sample rate in Hz.","inputSchema":{"type":"object","properties":{}}},
{"name":"vcvrack_search_library","description":"Search the installed plugin library for modules. Two modes:\n1. Single search: pass 'q' and/or 'tags'.\n2. Multi-search: pass 'queries' array to find VCO, VCF, VCA, ADSR, Audio etc. in ONE call. Each entry is keyed by its 'label' in the response. Always prefer multi-search when building a patch.","inputSchema":{"type":"object","properties":{"q":{"type":"string","description":"Search query matching slug, name, or description (single search)"},"tags":{"type":"string","description":"Tag filter e.g. 'VCO', 'VCF', 'LFO', 'Envelope', 'Mixer' (single search)"},"queries":{"type":"array","description":"Multi-search: array of independent queries returned grouped by label. Use to find all needed module types in one call.","items":{"type":"object","properties":{"label":{"type":"string","description":"Key for these results in the response e.g. 'vco', 'vca'"},"q":{"type":"string","description":"Search query"},"tags":{"type":"string","description":"Tag filter e.g. 'VCO', 'VCF'"}},"required":["label"]}}},"required":[]}},
{"name":"vcvrack_get_plugin","description":"Get detailed information about an installed plugin and its full module list.","inputSchema":{"type":"object","properties":{"plugin_slug":{"type":"string","description":"Plugin slug"}},"required":["plugin_slug"]}}
])json";

// ─── MCP prompts ────────────────────────────────────────────────────────────

static const char* MCP_PROMPTS_JSON = R"json([
{
  "name": "build_patch",
  "description": "Step-by-step guide for building a VCV Rack patch from scratch. Use this whenever the user asks to create, design, or assemble a patch.",
  "arguments": [
    {"name": "description", "description": "What the patch should do (e.g. 'a basic subtractive synth voice', 'an LFO-modulated filter')", "required": true}
  ]
},
{
  "name": "connect_modules",
  "description": "Guide for wiring cables between already-loaded modules.",
  "arguments": [
    {"name": "from_module", "description": "Source module name or ID", "required": true},
    {"name": "to_module",   "description": "Destination module name or ID", "required": true}
  ]
},
{
  "name": "set_module_params",
  "description": "Guide for reading and adjusting module parameters (knobs, switches, etc.).",
  "arguments": [
    {"name": "module", "description": "Module name or ID to configure", "required": true}
  ]
},
{
  "name": "add_modules_to_rack",
  "description": "Guide for placing one or more new modules into the rack with correct spatial positioning. Use this whenever modules need to be added to an existing or new patch.",
  "arguments": [
    {"name": "modules", "description": "Comma-separated list of modules to add (e.g. 'VCO, VCF, VCA')", "required": true}
  ]
}
])json";

static std::string buildPromptMessages(const std::string& name, const std::string& args) {
    auto argVal = [&](const std::string& key) -> std::string {
        size_t pos = args.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = args.find(":", pos);
        if (pos == std::string::npos) return "";
        pos = args.find("\"", pos);
        if (pos == std::string::npos) return "";
        size_t end = args.find("\"", pos + 1);
        if (end == std::string::npos) return "";
        return args.substr(pos + 1, end - pos - 1);
    };

    if (name == "build_patch") {
        std::string desc = argVal("description");
        if (desc.empty()) desc = "a patch";
        std::string text =
            "You are building a VCV Rack patch: " + desc + ".\n\n"
            "Follow these steps in order:\n\n"
            "PREPARATION\n"
            "1. Call vcvrack_get_status to confirm the server is running.\n"
            "2. Call vcvrack_get_mcp_position to get the root MCP module anchor.\n"
            "3. Call vcvrack_search_library to find all required modules (VCO, VCF, VCA, ADSR, Audio, etc.).\n"
            "   Collect plugin slug and module slug for every module you plan to add.\n\n"
            "PLACEMENT — always do this before any vcvrack_add_module call\n"
            "4. Call vcvrack_get_layout_preferences first and respect those bounds for all placements.\n"
            "5. Call vcvrack_get_rack_layout once. Inspect mcp_module, matrix_preferences, rows, and suggested_positions:\n"
            "   - Use mcp_module as the anchor for deciding where this section belongs.\n"
            "   - Prefer 'append_mcp_row' if the modules belong near the MCP root module.\n"
            "   - If the MCP row already ends with Audio Interface modules, prefer 'insert_before_output' so Audio remains the terminal module.\n"
            "   - If the patch should extend the MCP row or another existing row, use that row's 'append_row_N'.\n"
            "   - Use 'new_row' only if starting a logically separate section aligned with mcp_module.x.\n"
            "   Note: grid_unit_px=15 (1 HP), row_height_px=380 (one 3U row).\n"
            "6. Call vcvrack_add_module for each required module, passing x and y from step 5.\n"
            "   After each add, store the returned width. Compute the next module's x as:\n"
            "     next_x = current_x + returned_width\n"
            "   This lets you add a whole row of modules without calling vcvrack_get_rack_layout again.\n"
            "   Save every returned module ID — you will need them for cables and params.\n\n"
            "WIRING\n"
            "7. Call vcvrack_get_module on each module to discover input/output port indices and their labels.\n"
            "8. Call vcvrack_add_cable to wire the signal path (e.g. VCO OUT -> VCF IN -> VCA IN -> Audio IN).\n"
            "   Verify each cable with vcvrack_list_cables if unsure.\n\n"
            "TUNING\n"
            "9. Call vcvrack_get_params before every tuning step to read each control's raw range,\n"
            "   display string, and switch labels. Never guess units from the parameter name alone.\n"
            "10. Use vcvrack_set_params in small batches, keeping values inside the reported min/max range.\n"
            "   Many Rack knobs are normalized control values, not literal Hz or seconds.\n"
            "11. After each write, call vcvrack_get_params again to confirm the change before continuing.\n\n"
            "FINISHING\n"
            "Always verify each step's result before proceeding to the next.";
        return "[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":" + jsonStr(text) + "}}]";
    }

    if (name == "connect_modules") {
        std::string from = argVal("from_module");
        std::string to   = argVal("to_module");
        std::string text =
            "You need to connect " + (from.empty() ? "the source module" : from) +
            " to " + (to.empty() ? "the destination module" : to) + " in VCV Rack.\n\n"
            "Steps:\n"
            "1. Call vcvrack_list_modules to find module IDs if you don't already have them.\n"
            "2. Call vcvrack_get_module on both modules to see their output and input port indices.\n"
            "3. Call vcvrack_add_cable with outputModuleId, outputId, inputModuleId, inputId.\n"
            "4. Verify with vcvrack_list_cables that the cable appears.";
        return "[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":" + jsonStr(text) + "}}]";
    }

    if (name == "set_module_params") {
        std::string mod = argVal("module");
        std::string text =
            "You need to configure parameters on " + (mod.empty() ? "a module" : mod) + ".\n\n"
            "Steps:\n"
            "1. If you don't have the module ID, call vcvrack_list_modules.\n"
            "2. Call vcvrack_get_params with the moduleId to list all parameters, their indices, raw min/max ranges, display strings, and switch options.\n"
            "3. Do not guess units from the parameter name alone. Many modules expose normalized control values, so use the reported display string and ranges as the source of truth.\n"
            "4. Call vcvrack_set_params with a small array of {id, value} objects that stay inside the reported range.\n"
            "5. Call vcvrack_get_params again to confirm the values were applied before making more edits.";
        return "[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":" + jsonStr(text) + "}}]";
    }

    if (name == "add_modules_to_rack") {
        std::string mods = argVal("modules");
        if (mods.empty()) mods = "the requested modules";
        std::string text =
            "You need to add the following modules to the VCV Rack patch: " + mods + ".\n\n"
            "MANDATORY PLACEMENT WORKFLOW — follow exactly:\n\n"
            "1. Call vcvrack_get_mcp_position.\n"
            "   Use the returned x/y as the root anchor for the patch section you are extending.\n\n"
            "2. Call vcvrack_get_layout_preferences and keep all positions inside those bounds.\n"
            "3. Call vcvrack_get_rack_layout.\n"
            "   Read the response carefully:\n"
            "   - 'mcp_module' gives the MCP Server module's x/y position. Use it as the anchor for layout decisions.\n"
            "   - 'rows' shows every occupied row with its y coordinate, left/right edges, and module count.\n"
            "   - 'suggested_positions' gives ready-to-use x/y pairs:\n"
            "       'append_mcp_row' — preferred default for modules that belong near the MCP root row.\n"
            "       'insert_before_output' — insert before terminal Audio Interface modules so outputs stay on the far right.\n"
            "       'append_row_N' — place next to existing modules in row N.\n"
            "       'new_row'      — start a completely new row below all current content, aligned with mcp_module.x.\n"
            "   - grid_unit_px=15 means 1 HP = 15 px. row_height_px=380 means one 3U row = 380 px.\n\n"
            "4. Choose the right suggested position:\n"
            "   - First decide which row the module group belongs on relative to mcp_module.\n"
            "   - If the module belongs with the MCP root section, prefer 'append_mcp_row'.\n"
            "   - If the row already ends with Audio Interface modules and you are extending the signal chain, prefer 'insert_before_output'.\n"
            "   - If it should extend an existing row, use that row's 'append_row_N'.\n"
            "   - If it should start a new, independent signal chain, use 'new_row'.\n\n"
            "5. Call vcvrack_add_module for the first module with the chosen x and y.\n"
            "   The response includes the module's 'width' in pixels.\n\n"
            "6. For each subsequent module in the same row, compute:\n"
            "     next_x = previous_x + previous_width\n"
            "   Call vcvrack_add_module with next_x and the same y.\n"
            "   Do NOT call vcvrack_get_rack_layout again between modules in the same row.\n\n"
            "7. Save every returned module ID for wiring and parameter configuration.\n\n"
            "Never omit x and y. Never rely on auto-placement. Explicit coordinates keep the rack tidy.";
        return "[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":" + jsonStr(text) + "}}]";
    }

    return "[]";
}

// ─── Module Definition ─────────────────────────────────────────────────────

RackMcpServer::RackMcpServer() {
    config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
    configParam(PORT_PARAM, 2000.f, 9999.f, 2600.f, "HTTP Port")->snapEnabled = true;
    configParam(ENABLED_PARAM, 0.f, 1.f, 0.f, "Enable HTTP Server");
    configOutput(HEARTBEAT_OUTPUT, "Server heartbeat");
}

void RackMcpServer::process(const ProcessArgs& args) {
    bool enabled = params[ENABLED_PARAM].getValue() > 0.5f;
    if (enabled && !wasEnabled) startServer((int)params[PORT_PARAM].getValue());
    else if (!enabled && wasEnabled) stopServer();
    wasEnabled = enabled;

    if (server) {
        heartbeatPhase += args.sampleTime;
        if (heartbeatPhase >= 1.f) heartbeatPhase -= 1.f;
        outputs[HEARTBEAT_OUTPUT].setVoltage(heartbeatPhase < 0.05f ? 10.f : 0.f);
    }

    // Activity blink: retrigger 150 ms flash on each API call
    if (activityFlag.exchange(false)) {
        activityTimer = 0.15f;
    }
    if (activityTimer > 0.f) {
        activityTimer -= args.sampleTime;
        lights[ACTIVITY_LIGHT].setBrightness(activityTimer > 0.f ? 1.f : 0.f);
    }
}

json_t* RackMcpServer::dataToJson() {
    json_t* rootJ = json_object();
    int colsHp = 0;
    int rows = 0;
    {
        std::lock_guard<std::mutex> lock(layoutPrefsMutex);
        colsHp = layoutMatrixColsHp;
        rows = layoutMatrixRows;
    }
    json_object_set_new(rootJ, "enabled", json_boolean(wasEnabled));
    json_object_set_new(rootJ, "layout_matrix_cols_hp", json_integer(colsHp));
    json_object_set_new(rootJ, "layout_matrix_rows", json_integer(rows));
    return rootJ;
}

void RackMcpServer::dataFromJson(json_t* rootJ) {
    json_t* enabledJ = json_object_get(rootJ, "enabled");
    if (enabledJ) params[ENABLED_PARAM].setValue(json_boolean_value(enabledJ) ? 1.f : 0.f);
    json_t* colsJ = json_object_get(rootJ, "layout_matrix_cols_hp");
    json_t* rowsJ = json_object_get(rootJ, "layout_matrix_rows");
    std::lock_guard<std::mutex> lock(layoutPrefsMutex);
    layoutMatrixColsHp = colsJ ? std::max(0, (int)json_integer_value(colsJ)) : 8;
    layoutMatrixRows = rowsJ ? std::max(0, (int)json_integer_value(rowsJ)) : 4;
}

// ─── HTTP Server ───────────────────────────────────────────────────────────

RackHttpServer::~RackHttpServer() { stop(); }

rack::math::Vec RackHttpServer::computeAutoPosition(int64_t nearModuleId, float moduleWidth) {
    if (!rackApp || !rackApp->scene || !rackApp->scene->rack) return math::Vec(0, 0);
    app::RackWidget* rw = rackApp->scene->rack;

    app::ModuleWidget* bridge = parent ? rw->getModule(parent->id) : nullptr;
    float anchorX = bridge ? bridge->box.pos.x : 0.f;
    float anchorY = bridge ? bridge->box.pos.y : 0.f;
    int mcpRowIdx = roundRowIndex(anchorY);
    LayoutPrefs prefs = readLayoutPrefs(parent);

    auto valid = [&](float x, float y) {
        int rowIdx = roundRowIndex(y);
        return isWithinRowBounds(rowIdx, mcpRowIdx, prefs)
            && isWithinColBounds(x, moduleWidth, anchorX, prefs);
    };

    if (nearModuleId >= 0) {
        app::ModuleWidget* fw = rw->getModule(nearModuleId);
        if (fw) {
            math::Vec nearPos = math::Vec(fw->box.pos.x + fw->box.size.x, fw->box.pos.y);
            if (valid(nearPos.x, nearPos.y)) return nearPos;
        }
    }

    std::map<int, float> rowRight;
    for (app::ModuleWidget* w : rw->getModules()) {
        if (bridge && w == bridge) continue;
        int rowIdx = roundRowIndex(w->box.pos.y);
        if (!isWithinRowBounds(rowIdx, mcpRowIdx, prefs)) continue;
        float right = w->box.pos.x + w->box.size.x;
        rowRight[rowIdx] = std::max(rowRight[rowIdx], right);
    }

    int maxKnownRowIdx = mcpRowIdx;
    for (const auto& [rowIdx, _] : rowRight) {
        maxKnownRowIdx = std::max(maxKnownRowIdx, rowIdx);
    }
    int maxRowIdx = (prefs.rows > 0) ? (mcpRowIdx + prefs.rows - 1) : (maxKnownRowIdx + 1);
    for (int rowIdx = mcpRowIdx; rowIdx <= maxRowIdx; ++rowIdx) {
        float candidateX = anchorX;
        auto it = rowRight.find(rowIdx);
        if (it != rowRight.end()) {
            candidateX = std::max(anchorX, it->second);
        }
        float candidateY = (float)alignedRowY(rowIdx);
        if (valid(candidateX, candidateY)) {
            return math::Vec(candidateX, candidateY);
        }
    }

    // Signal exhaustion with a negative position.
    return math::Vec(-1.f, -1.f);
}

std::string RackHttpServer::getLayoutPrefsJson() {
    LayoutPrefs prefs = readLayoutPrefs(parent);
    return "{"
        + jsonKV("matrix_cols_hp", std::to_string(prefs.colsHp))
        + jsonKV("matrix_rows", std::to_string(prefs.rows))
        + jsonKVs("anchor", "mcp_module", true)
        + "}";
}

static bool resolveBoundedSuggestion(float requestedX, int requestedRowIdx, float mcpX, int mcpRowIdx,
                                     const LayoutPrefs& prefs, const std::map<int, RowInfo>& rows, int& outRowIdx, float& outX) {
    if (!isWithinRowBounds(requestedRowIdx, mcpRowIdx, prefs)) return false;
    outRowIdx = requestedRowIdx;
    outX = std::max(requestedX, mcpX);
    if (isWithinColBounds(outX, 0.f, mcpX, prefs)) return true;

    int wrappedRow = requestedRowIdx + 1;
    if (!isWithinRowBounds(wrappedRow, mcpRowIdx, prefs)) return false;
    outRowIdx = wrappedRow;
    auto wrapIt = rows.find(wrappedRow);
    outX = mcpX;
    if (wrapIt != rows.end()) outX = std::max(mcpX, wrapIt->second.right);
    return isWithinColBounds(outX, 0.f, mcpX, prefs);
}

static void addSuggestion(std::string& sugJson, bool& sfirst, const std::string& label, const std::string& desc,
                          float x, int rowIdx, float mcpX, const LayoutPrefs& prefs) {
    if (!sfirst) sugJson += ",";
    sugJson += "{";
    sugJson += jsonKVs("label", label);
    sugJson += jsonKVs("description", desc);
    sugJson += jsonKV("x", std::to_string((int)x));
    sugJson += jsonKV("y", std::to_string(alignedRowY(rowIdx)));
    if (prefs.colsHp > 0) {
        int remaining = std::max(0, (int)(mcpX + hpToPx(prefs.colsHp) - x));
        sugJson += jsonKV("remaining_width_px", std::to_string(remaining), true);
    } else {
        sugJson += jsonKV("remaining_width_px", "-1", true);
    }
    sugJson += "}";
    sfirst = false;
}

static std::string buildLayoutJson(const std::map<int, RowInfo>& rows, const std::map<int, float>& rowTerminalOutputX,
                                   float mcpX, float mcpY, const LayoutPrefs& prefs) {
    std::string rowsJson = "[";
    bool first = true;
    for (auto& [idx, r] : rows) {
        if (!first) rowsJson += ",";
        rowsJson += "{" + jsonKV("row", std::to_string(idx))
                  + jsonKV("y", std::to_string((int)r.y))
                  + jsonKV("left_edge", std::to_string((int)r.left))
                  + jsonKV("right_edge", std::to_string((int)r.right))
                  + jsonKV("module_count", std::to_string(r.count), true) + "}";
        first = false;
    }
    rowsJson += "]";

    std::string sugJson = "[";
    bool sfirst = true;
    int mcpRowIdx = roundRowIndex(mcpY);
    std::set<std::pair<int, int>> seen;

    auto addCandidate = [&](const std::string& label, const std::string& desc, float reqX, int reqRow) {
        int rowIdx = reqRow;
        float x = reqX;
        if (!resolveBoundedSuggestion(reqX, reqRow, mcpX, mcpRowIdx, prefs, rows, rowIdx, x)) return;
        std::pair<int, int> key((int)x, rowIdx);
        if (seen.count(key)) return;
        seen.insert(key);
        addSuggestion(sugJson, sfirst, label, desc, x, rowIdx, mcpX, prefs);
    };

    for (auto& [idx, r] : rows) {
        addCandidate("append_row_" + std::to_string(idx),
                     "Append to row " + std::to_string(idx) + " (y=" + std::to_string((int)r.y) + ", " + std::to_string(r.count) + " modules)",
                     r.right, idx);
    }

    auto mcpRowIt = rows.find(mcpRowIdx);
    if (mcpRowIt != rows.end()) {
        addCandidate("append_mcp_row",
                     "Append directly to the row containing the MCP module (recommended for related modules)",
                     mcpRowIt->second.right, mcpRowIdx);
        auto outIt = rowTerminalOutputX.find(mcpRowIdx);
        if (outIt != rowTerminalOutputX.end()) {
            addCandidate("insert_before_output",
                         "Insert before the terminal output module on the MCP row so Audio stays at the far right",
                         outIt->second, mcpRowIdx);
        }
    }

    int newRowIdx = rows.empty() ? mcpRowIdx : (rows.rbegin()->first + 1);
    addCandidate("new_row",
                 "Start a fresh row below all existing modules, horizontally aligned with the MCP module",
                 mcpX, newRowIdx);

    sugJson += "]";
    std::string mcpJson = "{" + jsonKV("x", std::to_string((int)mcpX))
                       + jsonKV("y", std::to_string((int)mcpY), true) + "}";
    std::string prefsJson = "{"
        + jsonKV("matrix_cols_hp", std::to_string(prefs.colsHp))
        + jsonKV("matrix_rows", std::to_string(prefs.rows))
        + jsonKVs("anchor", "mcp_module", true)
        + "}";
    return "{\"grid_unit_px\":15,\"row_height_px\":380,\"mcp_module\":" + mcpJson
         + ",\"matrix_preferences\":" + prefsJson
         + ",\"rows\":" + rowsJson
         + ",\"suggested_positions\":" + sugJson + "}";
}
// ─── MCP tool dispatcher ────────────────────────────────────────────────

std::string RackHttpServer::dispatchTool(const std::string& name, const std::string& args) {
        INFO("[RackMcpServer] dispatchTool → %s", name.c_str());
        auto* rackApp = this->rackApp;

        if (name == "vcvrack_get_status") {
            float sr = 0.f; int count = 0;
            if (!taskQueue->postSync([rackApp, &sr, &count]() {
                if (!rackApp || !rackApp->engine) return;
                sr = rackApp->engine->getSampleRate();
                count = (int)rackApp->engine->getModuleIds().size();
            }, "get_status")) return toolFail("UI thread not responding (timed out)");
            return toolOk("{\"server\":\"VCV Rack MCP Bridge\",\"version\":\"1.3.0\","
                          "\"sampleRate\":" + std::to_string(sr) +
                          ",\"moduleCount\":" + std::to_string(count) + "}");
        }

        if (name == "vcvrack_get_mcp_position") {
            std::string body;
            int64_t mcpId = parent ? parent->id : -1;
            if (!taskQueue->postSync([rackApp, &body, mcpId]() {
                body = getMcpModulePositionJson(rackApp && rackApp->scene ? rackApp->scene->rack : nullptr, mcpId);
            }, "get_mcp_position")) return toolFail("UI thread not responding (timed out)");
            return toolOk(body);
        }

        if (name == "vcvrack_get_layout_preferences") {
            return toolOk(getLayoutPrefsJson());
        }

        if (name == "vcvrack_set_layout_preferences") {
            int colsHp = (int)parseJsonDouble(args, "matrix_cols_hp", -1);
            int rows = (int)parseJsonDouble(args, "matrix_rows", -1);
            if (colsHp < 0 || rows < 0) {
                return toolFail("matrix_cols_hp and matrix_rows are required and must be >= 0");
            }
            if (parent) {
                std::lock_guard<std::mutex> lock(parent->layoutPrefsMutex);
                parent->layoutMatrixColsHp = colsHp;
                parent->layoutMatrixRows = rows;
            }
            return toolOk(getLayoutPrefsJson());
        }

        if (name == "vcvrack_list_modules") {
            std::string body;
            if (!taskQueue->postSync([rackApp, &body]() {
                if (!rackApp || !rackApp->engine) { body = "[]"; return; }
                std::vector<int64_t> ids = rackApp->engine->getModuleIds();
                body = "[";
                for (size_t i = 0; i < ids.size(); i++) {
                    engine::Module* mod = rackApp->engine->getModule(ids[i]);
                    body += (mod ? serializeModuleSummary(mod) : "null");
                    if (i < ids.size() - 1) body += ",";
                }
                body += "]";
            }, "list_modules")) return toolFail("UI thread not responding (timed out)");
            return toolOk(body);
        }

        if (name == "vcvrack_get_module") {
            std::string rawId = parseRawValue(args, "module_id");
            if (!rawId.empty() && rawId.front() == '"') rawId = rawId.substr(1, rawId.size() - 2);
            int64_t id = rawId.empty() ? -1 : std::stoll(rawId);
            std::string body;
            if (!taskQueue->postSync([rackApp, id, &body]() {
                if (!rackApp || !rackApp->engine) return;
                engine::Module* mod = rackApp->engine->getModule(id);
                body = mod ? serializeModuleDetail(mod) : "";
            }, "get_module/" + std::to_string(id))) return toolFail("UI thread not responding (timed out)");
            if (body.empty()) return toolFail("Module not found: " + std::to_string(id));
            return toolOk(body);
        }

        if (name == "vcvrack_get_rack_layout") {
            std::map<int, RowInfo> rows;
            std::map<int, float> rowTerminalOutputX;
            float mcpX = 0.f, mcpY = 0.f;
            LayoutPrefs prefs = readLayoutPrefs(parent);
            int64_t mcpId = parent ? parent->id : -1;
            if (!taskQueue->postSync([rackApp, &rows, &rowTerminalOutputX, &mcpX, &mcpY, mcpId]() {
                if (!rackApp || !rackApp->engine) return;
                for (int64_t id : rackApp->engine->getModuleIds()) {
                    app::ModuleWidget* mw = rackApp->scene->rack->getModule(id);
                    if (!mw) continue;
                    if (id == mcpId) { mcpX = mw->box.pos.x; mcpY = mw->box.pos.y; }
                    int rowIdx = (int)std::round(mw->box.pos.y / RACK_GRID_HEIGHT);
                    float rowY  = rowIdx * RACK_GRID_HEIGHT;
                    float right = mw->box.pos.x + mw->box.size.x;
                    if (isTerminalOutputModule(mw)) {
                        auto outIt = rowTerminalOutputX.find(rowIdx);
                        if (outIt == rowTerminalOutputX.end() || mw->box.pos.x < outIt->second) {
                            rowTerminalOutputX[rowIdx] = mw->box.pos.x;
                        }
                    }
                    auto it = rows.find(rowIdx);
                    if (it == rows.end()) {
                        rows[rowIdx] = {rowY, mw->box.pos.x, right, 1};
                    } else {
                        it->second.left  = std::min(it->second.left,  mw->box.pos.x);
                        it->second.right = std::max(it->second.right, right);
                        it->second.count++;
                    }
                }
            }, "get_rack_layout")) return toolFail("UI thread not responding (timed out)");
            return toolOk(buildLayoutJson(rows, rowTerminalOutputX, mcpX, mcpY, prefs));
        }

        if (name == "vcvrack_add_module") {
            std::string pSlug = parseJsonString(args, "plugin_slug");
            std::string mSlug = parseJsonString(args, "module_slug");
            float x = (float)parseJsonDouble(args, "x", -1.0);
            float y = (float)parseJsonDouble(args, "y", -1.0);
            plugin::Model* model = nullptr;
            for (plugin::Plugin* p : rack::plugin::plugins)
                if (p->slug == pSlug)
                    for (plugin::Model* m : p->models)
                        if (m->slug == mSlug) { model = m; break; }
            if (!model) return toolFail("Model not found: " + pSlug + "/" + mSlug);
            if (x < 0.f) return toolFail("x is required. Call vcvrack_get_rack_layout first and use a suggested_positions entry.");
            if (y < 0.f) return toolFail("y is required. Call vcvrack_get_rack_layout first and use a suggested_positions entry.");
            int64_t moduleId = -1;
            float finalX = 0.f, finalY = 0.f, finalW = 0.f;
            LayoutPrefs prefs = readLayoutPrefs(parent);
            int64_t mcpId = parent ? parent->id : -1;
            if (!taskQueue->postSync([rackApp, model, x, y, prefs, mcpId, &moduleId, &finalX, &finalY, &finalW]() mutable {
                if (!rackApp->engine || !rackApp->scene || !rackApp->scene->rack) return;
                engine::Module* m = model->createModule();
                if (!m) return;
                app::ModuleWidget* mw = model->createModuleWidget(m);
                if (!mw) { delete m; return; }
                app::ModuleWidget* mcp = rackApp->scene->rack->getModule(mcpId);
                float mcpX = mcp ? mcp->box.pos.x : 0.f;
                int mcpRowIdx = roundRowIndex(mcp ? mcp->box.pos.y : 0.f);
                int rowIdx = roundRowIndex(y);
                if (!isWithinRowBounds(rowIdx, mcpRowIdx, prefs) || !isWithinColBounds(x, mw->box.size.x, mcpX, prefs)) {
                    mw->module = nullptr; // detach before delete so ~ModuleWidget() doesn't call engine->removeModule on unregistered module
                    delete mw;
                    delete m;
                    return;
                }
                rackApp->engine->addModule(m);
                moduleId = m->id;
                rackApp->scene->rack->addModule(mw);
                rackApp->scene->rack->setModulePosForce(mw, math::Vec(x, y));
                finalX = mw->box.pos.x;
                finalY = mw->box.pos.y;
                finalW = mw->box.size.x;
            }, "add_module/" + pSlug + "/" + mSlug)) return toolFail("UI thread not responding (timed out)");
            if (moduleId < 0 || finalW <= 0.f) return toolFail("Failed to create module (outside matrix preferences or invalid model)");
            return toolOk("{\"id\":" + std::to_string(moduleId)
                        + ",\"plugin\":" + jsonStr(pSlug)
                        + ",\"slug\":" + jsonStr(mSlug)
                        + ",\"x\":" + std::to_string((int)finalX)
                        + ",\"y\":" + std::to_string((int)finalY)
                        + ",\"width\":" + std::to_string((int)finalW) + "}");
        }

        if (name == "vcvrack_delete_module") {
            int64_t id = (int64_t)parseJsonDouble(args, "id", -1);
            if (parent && parent->id == id)
                return toolFail("Cannot delete the MCP server module itself");
            if (parent) {
                std::lock_guard<std::mutex> lock(parent->pendingDeleteMutex);
                parent->pendingDeleteIds.push_back((uint64_t)id);
            }
            return toolOk("{\"queued\":true,\"id\":" + std::to_string(id) + "}");
        }

        if (name == "vcvrack_get_params") {
            int64_t id = (int64_t)parseJsonDouble(args, "moduleId", -1);
            std::string body;
            if (!taskQueue->postSync([rackApp, id, &body]() {
                if (!rackApp || !rackApp->engine) return;
                engine::Module* mod = rackApp->engine->getModule(id);
                if (!mod) return;
                body = "[";
                for (int i = 0; i < (int)mod->params.size(); i++) {
                    body += serializeParamQuantity(mod->paramQuantities[i], i);
                    if (i < (int)mod->params.size() - 1) body += ",";
                }
                body += "]";
            }, "get_params/" + std::to_string(id))) return toolFail("UI thread not responding (timed out)");
            if (body.empty()) return toolFail("Module not found: " + std::to_string(id));
            return toolOk(body);
        }

        if (name == "vcvrack_set_params") {
            int64_t id = (int64_t)parseJsonDouble(args, "moduleId", -1);
            std::string paramsRaw = parseRawValue(args, "params");
            int applied = 0; bool found = false;
            if (!taskQueue->postSync([rackApp, id, paramsRaw, &applied, &found]() {
                if (!rackApp || !rackApp->engine) return;
                engine::Module* mod = rackApp->engine->getModule(id);
                if (!mod) return;
                found = true;
                size_t pos = 0;
                while (pos < paramsRaw.size()) {
                    size_t start = paramsRaw.find('{', pos);
                    if (start == std::string::npos) break;
                    size_t end = paramsRaw.find('}', start);
                    if (end == std::string::npos) break;
                    std::string obj = paramsRaw.substr(start, end - start + 1);
                    int paramId = (int)parseJsonDouble(obj, "id", -1);
                    double value = parseJsonDouble(obj, "value", 0.0);
                    if (paramId >= 0 && paramId < (int)mod->params.size()) {
                        rackApp->engine->setParamValue(mod, paramId, (float)value);
                        applied++;
                    }
                    pos = end + 1;
                }
            }, "set_params/" + std::to_string(id))) return toolFail("UI thread not responding (timed out)");
            if (!found) return toolFail("Module not found: " + std::to_string(id));
            return toolOk("{\"applied\":" + std::to_string(applied) + "}");
        }

        if (name == "vcvrack_list_cables") {
            std::string body;
            if (!taskQueue->postSync([rackApp, &body]() {
                if (!rackApp || !rackApp->engine) { body = "[]"; return; }
                std::vector<int64_t> ids = rackApp->engine->getCableIds();
                body = "[";
                for (size_t i = 0; i < ids.size(); i++) {
                    engine::Cable* c = rackApp->engine->getCable(ids[i]);
                    if (c && c->outputModule && c->inputModule) {
                        body += "{\"id\":" + std::to_string(c->id) +
                                ",\"outputModuleId\":" + std::to_string(c->outputModule->id) +
                                ",\"outputId\":" + std::to_string(c->outputId) +
                                ",\"inputModuleId\":" + std::to_string(c->inputModule->id) +
                                ",\"inputId\":" + std::to_string(c->inputId) + "}";
                    } else { body += "null"; }
                    if (i < ids.size() - 1) body += ",";
                }
                body += "]";
            }, "list_cables")) return toolFail("UI thread not responding (timed out)");
            return toolOk(body);
        }

        if (name == "vcvrack_add_cable") {
            int64_t outM = (int64_t)parseJsonDouble(args, "outputModuleId", -1);
            int64_t inM  = (int64_t)parseJsonDouble(args, "inputModuleId", -1);
            int outP = (int)parseJsonDouble(args, "outputId", 0);
            int inP  = (int)parseJsonDouble(args, "inputId", 0);
            int64_t cableId = -1;
            if (!taskQueue->postSync([rackApp, outM, outP, inM, inP, &cableId]() {
                if (!rackApp->engine || !rackApp->scene || !rackApp->scene->rack) return;
                engine::Module* oMod = rackApp->engine->getModule(outM);
                engine::Module* iMod = rackApp->engine->getModule(inM);
                if (!oMod || !iMod) return;
                app::ModuleWidget* oWidget = rackApp->scene->rack->getModule(outM);
                app::ModuleWidget* iWidget = rackApp->scene->rack->getModule(inM);
                if (!oWidget || !iWidget) return;
                app::PortWidget* oPort = oWidget->getOutput(outP);
                app::PortWidget* iPort = iWidget->getInput(inP);
                if (!oPort || !iPort) return;
                engine::Cable* c = new engine::Cable;
                c->outputModule = oMod; c->outputId = outP;
                c->inputModule = iMod;  c->inputId = inP;
                rackApp->engine->addCable(c);
                cableId = c->id;
                app::CableWidget* cw = new app::CableWidget;
                cw->color = rackApp->scene->rack->getNextCableColor();
                cw->setCable(c);
                cw->outputPort = oPort;
                cw->inputPort = iPort;
                rackApp->scene->rack->addCable(cw);
            }, "add_cable/" + std::to_string(outM) + "→" + std::to_string(inM))) return toolFail("UI thread not responding (timed out)");
            if (cableId < 0) return toolFail("Failed to connect: ports or modules not found");
            return toolOk("{\"id\":" + std::to_string(cableId) + "}");
        }

        if (name == "vcvrack_delete_cable") {
            int64_t id = (int64_t)parseJsonDouble(args, "id", -1);
            bool found = false;
            if (!taskQueue->postSync([rackApp, id, &found]() {
                if (!rackApp->engine || !rackApp->scene || !rackApp->scene->rack) return;
                app::CableWidget* cw = rackApp->scene->rack->getCable(id);
                if (cw) {
                    found = true;
                    rackApp->scene->rack->removeCable(cw);
                    delete cw;
                } else {
                    engine::Cable* c = rackApp->engine->getCable(id);
                    if (c) { found = true; rackApp->engine->removeCable(c); delete c; }
                }
            }, "delete_cable/" + std::to_string(id))) return toolFail("UI thread not responding (timed out)");
            if (!found) return toolFail("Cable not found: " + std::to_string(id));
            return toolOk("{\"removed\":true}");
        }

        if (name == "vcvrack_get_sample_rate") {
            float sr = 0.f;
            if (!taskQueue->postSync([rackApp, &sr]() { sr = rackApp->engine->getSampleRate(); }, "get_sample_rate")) return toolFail("UI thread not responding (timed out)");
            return toolOk("{\"sampleRate\":" + std::to_string(sr) + "}");
        }

        if (name == "vcvrack_search_library") {
            // Core search: given a tag-filter string and a query string, returns a
            // JSON array of { slug, name, modules[] } plugin objects.
            auto runSearch = [&](const std::string& tagFilter, const std::string& query) -> std::string {
                std::string qLow = query;
                std::transform(qLow.begin(), qLow.end(), qLow.begin(), ::tolower);
                std::string tagLow = tagFilter;
                std::transform(tagLow.begin(), tagLow.end(), tagLow.begin(), ::tolower);
                std::string out = "[";
                bool firstPlug = true;
                for (plugin::Plugin* plug : rack::plugin::plugins) {
                    std::vector<plugin::Model*> filtered;
                    for (plugin::Model* m : plug->models) {
                        if (!tagLow.empty()) {
                            bool hasTag = false;
                            for (int t : m->tagIds) {
                                std::string tn = rack::tag::getTag(t);
                                std::transform(tn.begin(), tn.end(), tn.begin(), ::tolower);
                                if (tagLow.find(tn) != std::string::npos) { hasTag = true; break; }
                            }
                            if (!hasTag) continue;
                        }
                        if (!qLow.empty()) {
                            std::string s = m->slug + " " + m->name + " " + m->description;
                            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                            if (s.find(qLow) == std::string::npos) continue;
                        }
                        filtered.push_back(m);
                    }
                    if (filtered.empty()) continue;
                    if (!firstPlug) out += ","; firstPlug = false;
                    out += "{\"slug\":" + jsonStr(plug->slug) + ",\"name\":" + jsonStr(plug->name) + ",\"modules\":[";
                    for (size_t i = 0; i < filtered.size(); i++) {
                        out += serializeModel(filtered[i]);
                        if (i < filtered.size() - 1) out += ",";
                    }
                    out += "]}";
                }
                out += "]";
                return out;
            };

            // Multi-search mode: queries array present
            std::string queriesRaw = parseRawValue(args, "queries");
            if (!queriesRaw.empty() && queriesRaw[0] == '[') {
                // Iterate array items (each is a JSON object)
                std::string result = "{";
                bool firstEntry = true;
                size_t pos = 0;
                // Skip opening '['
                while (pos < queriesRaw.size() && queriesRaw[pos] != '{') pos++;
                while (pos < queriesRaw.size()) {
                    if (queriesRaw[pos] != '{') { pos++; continue; }
                    // Find matching '}'
                    int depth = 0;
                    bool inStr = false;
                    size_t start = pos;
                    for (; pos < queriesRaw.size(); pos++) {
                        char ch = queriesRaw[pos];
                        if (ch == '\\') { pos++; continue; }
                        if (ch == '"') { inStr = !inStr; continue; }
                        if (inStr) continue;
                        if (ch == '{') depth++;
                        else if (ch == '}' && --depth == 0) { pos++; break; }
                    }
                    std::string item = queriesRaw.substr(start, pos - start);
                    std::string label = parseJsonString(item, "label");
                    std::string itemQ = parseJsonString(item, "q");
                    std::string itemTags = parseJsonString(item, "tags");
                    if (!label.empty()) {
                        if (!firstEntry) result += ","; firstEntry = false;
                        result += jsonStr(label) + ":" + runSearch(itemTags, itemQ);
                    }
                }
                result += "}";
                return toolOk(result);
            }

            // Single-search mode (backwards-compatible)
            return toolOk(runSearch(parseJsonString(args, "tags"), parseJsonString(args, "q")));
        }

        if (name == "vcvrack_get_plugin") {
            std::string slug = parseJsonString(args, "plugin_slug");
            for (plugin::Plugin* p : rack::plugin::plugins)
                if (p->slug == slug) return toolOk(serializePlugin(p));
            return toolFail("Plugin not found: " + slug);
        }

        return toolFail("Unknown tool: " + name);
}

// ─── MCP Streamable HTTP request handler ────────────────────────────────

void RackHttpServer::handleMcpPost(const httplib::Request& req, httplib::Response& res) {
        const std::string& body = req.body;
        std::string id     = parseJsonRpcId(body);
        std::string method = parseJsonString(body, "method");

        // Notifications require no response
        if (method.rfind("notifications/", 0) == 0) {
            res.status = 202;
            res.set_content("", "application/json");
            return;
        }

        if (method == "initialize") {
            res.set_content(mcpOk(id, R"({"protocolVersion":"2024-11-05","capabilities":{"tools":{},"prompts":{}},"serverInfo":{"name":"VCV Rack MCP Bridge","version":"1.3.0"}})"), "application/json");
            return;
        }

        if (method == "ping") {
            res.set_content(mcpOk(id, "{}"), "application/json");
            return;
        }

        if (method == "tools/list") {
            res.set_content(mcpOk(id, "{\"tools\":" + std::string(MCP_TOOLS_JSON) + "}"), "application/json");
            return;
        }

        if (method == "prompts/list") {
            res.set_content(mcpOk(id, "{\"prompts\":" + std::string(MCP_PROMPTS_JSON) + "}"), "application/json");
            return;
        }

        if (method == "prompts/get") {
            std::string params   = parseRawValue(body, "params");
            std::string promptName = parseJsonString(params, "name");
            std::string promptArgs = parseRawValue(params, "arguments");
            if (promptArgs.empty()) promptArgs = "{}";
            if (promptName.empty()) {
                res.set_content(mcpErr(id, -32602, "Missing prompt name"), "application/json");
                return;
            }
            std::string messages = buildPromptMessages(promptName, promptArgs);
            if (messages == "[]") {
                res.set_content(mcpErr(id, -32602, "Unknown prompt: " + promptName), "application/json");
                return;
            }
            res.set_content(mcpOk(id, "{\"description\":" + jsonStr(promptName) + ",\"messages\":" + messages + "}"), "application/json");
            return;
        }

        if (method == "tools/call") {
            std::string params   = parseRawValue(body, "params");
            if (params.empty()) {
                res.set_content(mcpErr(id, -32602, "Missing params"), "application/json");
                return;
            }
            std::string toolName = parseJsonString(params, "name");
            std::string toolArgs = parseRawValue(params, "arguments");
            if (toolArgs.empty()) toolArgs = "{}";
            try {
                res.set_content(mcpOk(id, dispatchTool(toolName, toolArgs)), "application/json");
            } catch (const std::exception& e) {
                res.set_content(mcpOk(id, toolFail(std::string("Internal error: ") + e.what())), "application/json");
            }
            return;
        }

        res.set_content(mcpErr(id, -32601, "Method not found: " + method), "application/json");
}

void RackHttpServer::setupRoutes() {
        rackApp = APP;
        auto* rackApp = this->rackApp; // local alias for lambda captures
        svr.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.set_header("Content-Type", "application/json");
            return httplib::Server::HandlerResponse::Unhandled;
        });
        svr.Options(".*", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

        svr.Get("/status", [rackApp, this](const httplib::Request&, httplib::Response& res) {
            if (!rackApp || !rackApp->engine) { res.status = 503; res.set_content(err("Engine not available"), "application/json"); return; }
            float sr = 0.f; int count = 0;
            if (!taskQueue->postSync([rackApp, &sr, &count]() {
                if (!rackApp->engine) return;
                sr = rackApp->engine->getSampleRate();
                count = (int)rackApp->engine->getModuleIds().size();
            })) { res.status = 503; res.set_content(err("UI thread not responding (timed out)"), "application/json"); return; }
            std::string body = "{" + jsonKVs("server", "VCV Rack MCP Bridge") + jsonKVs("version", "1.3.0") +
                jsonKVs("build", std::string(__DATE__) + " " + __TIME__) +
                jsonKV("sampleRate", std::to_string(sr)) + jsonKV("moduleCount", std::to_string(count), true) + "}";
            res.set_content(ok(body), "application/json");
        });

        svr.Get("/mcp-module/position", [rackApp, this](const httplib::Request&, httplib::Response& res) {
            if (!rackApp || !rackApp->engine) { res.status = 503; res.set_content(err("Engine not available"), "application/json"); return; }
            std::string body;
            int64_t mcpId = parent ? parent->id : -1;
            if (!taskQueue->postSync([rackApp, &body, mcpId]() {
                body = getMcpModulePositionJson(rackApp && rackApp->scene ? rackApp->scene->rack : nullptr, mcpId);
            })) { res.status = 503; res.set_content(err("UI thread not responding (timed out)"), "application/json"); return; }
            res.set_content(ok(body), "application/json");
        });

        svr.Get("/layout/preferences", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(ok(getLayoutPrefsJson()), "application/json");
        });

        svr.Post("/layout/preferences", [this](const httplib::Request& req, httplib::Response& res) {
            int colsHp = (int)parseJsonDouble(req.body, "matrix_cols_hp", -1);
            int rows = (int)parseJsonDouble(req.body, "matrix_rows", -1);
            if (colsHp < 0 || rows < 0) {
                res.status = 400;
                res.set_content(err("matrix_cols_hp and matrix_rows are required and must be >= 0"), "application/json");
                return;
            }
            if (parent) {
                std::lock_guard<std::mutex> lock(parent->layoutPrefsMutex);
                parent->layoutMatrixColsHp = colsHp;
                parent->layoutMatrixRows = rows;
            }
            res.set_content(ok(getLayoutPrefsJson()), "application/json");
        });

        svr.Get("/modules", [rackApp, this](const httplib::Request&, httplib::Response& res) {
            if (!rackApp || !rackApp->engine) { res.status = 503; res.set_content(err("Engine not available"), "application/json"); return; }
            std::string body;
            if (!taskQueue->postSync([rackApp, &body]() {
                if (!rackApp->engine) { body = "[]"; return; }
                std::vector<int64_t> ids = rackApp->engine->getModuleIds();
                body = "[";
                for (size_t i = 0; i < ids.size(); i++) {
                    engine::Module* mod = rackApp->engine->getModule(ids[i]);
                    body += (mod ? serializeModuleSummary(mod) : "null") + (i < ids.size() - 1 ? ", " : "");
                }
                body += "]";
            })) { res.status = 503; res.set_content(err("UI thread not responding (timed out)"), "application/json"); return; }
            res.set_content(ok(body), "application/json");
        });

        svr.Get("/rack/layout", [rackApp, this](const httplib::Request&, httplib::Response& res) {
            if (!rackApp || !rackApp->engine) { res.status = 503; res.set_content(err("Engine not available"), "application/json"); return; }

            std::map<int, RowInfo> rows;
            std::map<int, float> rowTerminalOutputX;
            float mcpX = 0.f, mcpY = 0.f;
            LayoutPrefs prefs = readLayoutPrefs(parent);
            int64_t mcpId = parent ? parent->id : -1;

            if (!taskQueue->postSync([rackApp, &rows, &rowTerminalOutputX, &mcpX, &mcpY, mcpId]() {
                for (int64_t id : rackApp->engine->getModuleIds()) {
                    app::ModuleWidget* mw = rackApp->scene->rack->getModule(id);
                    if (!mw) continue;
                    if (id == mcpId) { mcpX = mw->box.pos.x; mcpY = mw->box.pos.y; }
                    int rowIdx = (int)std::round(mw->box.pos.y / RACK_GRID_HEIGHT);
                    float rowY  = rowIdx * RACK_GRID_HEIGHT;
                    float right = mw->box.pos.x + mw->box.size.x;
                    if (isTerminalOutputModule(mw)) {
                        auto outIt = rowTerminalOutputX.find(rowIdx);
                        if (outIt == rowTerminalOutputX.end() || mw->box.pos.x < outIt->second) {
                            rowTerminalOutputX[rowIdx] = mw->box.pos.x;
                        }
                    }
                    auto it = rows.find(rowIdx);
                    if (it == rows.end()) {
                        rows[rowIdx] = {rowY, mw->box.pos.x, right, 1};
                    } else {
                        it->second.left  = std::min(it->second.left,  mw->box.pos.x);
                        it->second.right = std::max(it->second.right, right);
                        it->second.count++;
                    }
                }
            })) { res.status = 503; res.set_content(err("UI thread not responding (timed out)"), "application/json"); return; }

            std::string body = buildLayoutJson(rows, rowTerminalOutputX, mcpX, mcpY, prefs);
            res.set_content(ok(body), "application/json");
        });

        svr.Get(R"(/modules/(\d+)$)", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            if (!rackApp || !rackApp->engine) { res.status = 503; res.set_content(err("Engine not available"), "application/json"); return; }
            int64_t id = std::stoll(req.matches[1]);
            std::string body;
            if (!taskQueue->postSync([rackApp, id, &body]() {
                if (!rackApp->engine) return;
                engine::Module* mod = rackApp->engine->getModule(id);
                body = mod ? serializeModuleDetail(mod) : "null";
            })) { res.status = 503; res.set_content(err("UI thread not responding (timed out)"), "application/json"); return; }
            if (body == "null") { res.status = 404; res.set_content(err("Module not found"), "application/json"); }
            else res.set_content(ok(body), "application/json");
        });

        svr.Post("/modules/add", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            if (!rackApp || !rackApp->engine || !rackApp->scene || !rackApp->scene->rack) { res.status = 503; res.set_content(err("Engine not available"), "application/json"); return; }
            std::string pSlug = parseJsonString(req.body, "plugin"), mSlug = parseJsonString(req.body, "slug");
            float x = (float)parseJsonDouble(req.body, "x", -1.0), y = (float)parseJsonDouble(req.body, "y", -1.0);
            int64_t nearId = (int64_t)parseJsonDouble(req.body, "nearModuleId", -1.0);
            plugin::Model* model = nullptr;
            for (plugin::Plugin* p : rack::plugin::plugins) if (p->slug == pSlug) for (plugin::Model* m : p->models) if (m->slug == mSlug) { model = m; break; }
            if (!model) { res.status = 404; res.set_content(err("Model not found"), "application/json"); return; }
            int64_t moduleId = -1;
            std::string failReason;
            LayoutPrefs prefs = readLayoutPrefs(parent);
            int64_t mcpId = parent ? parent->id : -1;
            if (!taskQueue->postSync([this, rackApp, model, x, y, nearId, prefs, mcpId, &moduleId, &failReason]() mutable {
                if (!rackApp->engine || !rackApp->scene || !rackApp->scene->rack) return;
                engine::Module* m = model->createModule(); if (!m) return;
                app::ModuleWidget* mw = model->createModuleWidget(m); if (!mw) { delete m; return; }
                math::Vec pos;
                if (x >= 0.f) {
                    if (y < 0.f) { failReason = "y is required when x is provided"; mw->module = nullptr; delete mw; delete m; return; }
                    pos = math::Vec(x, y);
                } else {
                    pos = computeAutoPosition(nearId, mw->box.size.x);
                }
                app::ModuleWidget* mcp = rackApp->scene->rack->getModule(mcpId);
                float mcpX = mcp ? mcp->box.pos.x : 0.f;
                int mcpRowIdx = roundRowIndex(mcp ? mcp->box.pos.y : 0.f);
                int rowIdx = roundRowIndex(pos.y);
                if (pos.x < 0.f || pos.y < 0.f) {
                    failReason = "No valid matrix slot available for auto-placement";
                    mw->module = nullptr; // detach before delete so ~ModuleWidget() doesn't call engine->removeModule on unregistered module
                    delete mw;
                    delete m;
                    return;
                }
                if (!isWithinRowBounds(rowIdx, mcpRowIdx, prefs) || !isWithinColBounds(pos.x, mw->box.size.x, mcpX, prefs)) {
                    failReason = "Requested placement is outside matrix preferences";
                    mw->module = nullptr; // detach before delete so ~ModuleWidget() doesn't call engine->removeModule on unregistered module
                    delete mw;
                    delete m;
                    return;
                }
                rackApp->engine->addModule(m); moduleId = m->id;
                rackApp->scene->rack->addModule(mw);
                rackApp->scene->rack->setModulePosForce(mw, pos);
            })) { res.status = 503; res.set_content(err("UI thread not responding (timed out)"), "application/json"); return; }
            if (moduleId < 0) {
                res.status = 400;
                res.set_content(err(failReason.empty() ? "Failed to create module" : failReason), "application/json");
            }
            else res.set_content(ok("{" + jsonKV("id", std::to_string(moduleId)) + jsonKVs("plugin", pSlug) + jsonKVs("slug", mSlug, true) + "}"), "application/json");
        });

        svr.Delete(R"(/modules/(\d+)$)", [this](const httplib::Request& req, httplib::Response& res) {
            int64_t id = std::stoll(req.matches[1]);
            if (parent && parent->id == id) { res.status = 403; res.set_content(err("Cannot delete server module"), "application/json"); return; }
            if (parent) { std::lock_guard<std::mutex> lock(parent->pendingDeleteMutex); parent->pendingDeleteIds.push_back((uint64_t)id); }
            res.set_content(ok("{\"status\":\"queued\",\"id\":" + std::to_string(id) + "}"), "application/json");
        });

        svr.Get(R"(/modules/(\d+)/params)", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            if (!rackApp || !rackApp->engine) { res.status = 503; res.set_content(err("Engine not available"), "application/json"); return; }
            int64_t id = std::stoll(req.matches[1]);
            std::string body;
            if (!taskQueue->postSync([rackApp, id, &body]() {
                if (!rackApp->engine) return;
                engine::Module* mod = rackApp->engine->getModule(id);
                if (!mod) { body = "null"; return; }
                body = "[";
                for (int i = 0; i < (int)mod->params.size(); i++) {
                    body += serializeParamQuantity(mod->paramQuantities[i], i) + (i < (int)mod->params.size() - 1 ? ", " : "");
                }
                body += "]";
            })) { res.status = 503; res.set_content(err("UI thread not responding (timed out)"), "application/json"); return; }
            if (body == "null") { res.status = 404; res.set_content(err("Module not found"), "application/json"); }
            else res.set_content(ok(body), "application/json");
        });

        svr.Post(R"(/modules/(\d+)/params)", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            if (!rackApp || !rackApp->engine) { res.status = 503; res.set_content(err("Engine not available"), "application/json"); return; }
            int64_t id = std::stoll(req.matches[1]);
            const std::string requestBody = req.body;
            int applied = 0; bool found = false;
            if (!taskQueue->postSync([rackApp, id, requestBody, &applied, &found]() {
                engine::Module* mod = rackApp->engine->getModule(id);
                if (!mod) return;
                found = true;
                size_t pos = 0;
                while (pos < requestBody.size()) {
                    size_t start = requestBody.find('{', pos); if (start == std::string::npos) break;
                    size_t end = requestBody.find('}', start); if (end == std::string::npos) break;
                    std::string obj = requestBody.substr(start, end - start + 1);
                    int paramId = (int)parseJsonDouble(obj, "id", -1);
                    double value = parseJsonDouble(obj, "value", 0.0);
                    if (paramId >= 0 && paramId < (int)mod->params.size()) { rackApp->engine->setParamValue(mod, paramId, (float)value); applied++; }
                    pos = end + 1;
                }
            })) { res.status = 503; res.set_content(err("UI thread not responding (timed out)"), "application/json"); return; }
            if (!found) { res.status = 404; res.set_content(err("Module not found"), "application/json"); }
            else res.set_content(ok("{\"applied\":" + std::to_string(applied) + "}"), "application/json");
        });

        svr.Get("/cables", [rackApp, this](const httplib::Request&, httplib::Response& res) {
            if (!rackApp || !rackApp->engine) { res.status = 503; res.set_content(err("Engine not available"), "application/json"); return; }
            std::string body;
            if (!taskQueue->postSync([rackApp, &body]() {
                if (!rackApp->engine) { body = "[]"; return; }
                std::vector<int64_t> ids = rackApp->engine->getCableIds();
                body = "[";
                for (size_t i = 0; i < ids.size(); i++) {
                    engine::Cable* c = rackApp->engine->getCable(ids[i]);
                    if (c && c->outputModule && c->inputModule) {
                        body += "{\"id\":" + std::to_string(c->id) + ", \"outputModuleId\":" + std::to_string(c->outputModule->id) +
                                ", \"outputId\":" + std::to_string(c->outputId) + ", \"inputModuleId\":" + std::to_string(c->inputModule->id) +
                                ", \"inputId\":" + std::to_string(c->inputId) + "}";
                    } else { body += "null"; }
                    if (i < ids.size() - 1) body += ", ";
                }
                body += "]";
            })) { res.status = 503; res.set_content(err("UI thread not responding (timed out)"), "application/json"); return; }
            res.set_content(ok(body), "application/json");
        });

        svr.Post("/cables", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            if (!rackApp || !rackApp->engine || !rackApp->scene || !rackApp->scene->rack) { res.status = 503; res.set_content(err("Engine not available"), "application/json"); return; }
            int64_t outM = (int64_t)parseJsonDouble(req.body, "outputModuleId", -1), inM = (int64_t)parseJsonDouble(req.body, "inputModuleId", -1);
            int outP = (int)parseJsonDouble(req.body, "outputId", 0), inP = (int)parseJsonDouble(req.body, "inputId", 0);
            int64_t cableId = -1;
            
            if (!taskQueue->postSync([rackApp, outM, outP, inM, inP, &cableId]() {
                if (!rackApp->engine || !rackApp->scene || !rackApp->scene->rack) return;
                engine::Module* oMod = rackApp->engine->getModule(outM);
                engine::Module* iMod = rackApp->engine->getModule(inM);
                if (!oMod || !iMod) return;

                app::ModuleWidget* oWidget = rackApp->scene->rack->getModule(outM);
                app::ModuleWidget* iWidget = rackApp->scene->rack->getModule(inM);
                if (!oWidget || !iWidget) return;

                app::PortWidget* oPort = oWidget->getOutput(outP);
                app::PortWidget* iPort = iWidget->getInput(inP);
                if (!oPort || !iPort) return;

                // 1. Create engine cable
                engine::Cable* c = new engine::Cable;
                c->outputModule = oMod;
                c->outputId = outP;
                c->inputModule = iMod;
                c->inputId = inP;
                rackApp->engine->addCable(c);
                cableId = c->id;

                // 2. Create UI cable widget
                app::CableWidget* cw = new app::CableWidget;
                cw->color = rackApp->scene->rack->getNextCableColor();
                cw->setCable(c);
                cw->outputPort = oPort;
                cw->inputPort = iPort;
                rackApp->scene->rack->addCable(cw);
            })) { res.status = 503; res.set_content(err("UI thread not responding (timed out)"), "application/json"); return; }

            if (cableId < 0) { res.status = 404; res.set_content(err("Failed to connect: ports or modules not found"), "application/json"); }
            else res.set_content(ok("{\"id\":" + std::to_string(cableId) + "}"), "application/json");
        });

        svr.Delete(R"(/cables/(\d+))", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            if (!rackApp || !rackApp->engine || !rackApp->scene || !rackApp->scene->rack) { res.status = 503; res.set_content(err("Engine not available"), "application/json"); return; }
            int64_t id = std::stoll(req.matches[1]);
            bool found = false;

            if (!taskQueue->postSync([rackApp, id, &found]() {
                if (!rackApp->engine || !rackApp->scene || !rackApp->scene->rack) return;
                app::CableWidget* cw = rackApp->scene->rack->getCable(id);
                if (cw) {
                    found = true;
                    rackApp->scene->rack->removeCable(cw);
                    delete cw;
                } else {
                    // Fallback: if no widget, try removing from engine
                    engine::Cable* c = rackApp->engine->getCable(id);
                    if (c) {
                        found = true;
                        rackApp->engine->removeCable(c);
                        delete c;
                    }
                }
            })) { res.status = 503; res.set_content(err("UI thread not responding (timed out)"), "application/json"); return; }

            if (!found) { res.status = 404; res.set_content(err("Cable not found"), "application/json"); }
            else res.set_content(ok("{\"removed\":true}"), "application/json");
        });

        svr.Get("/sample-rate", [rackApp, this](const httplib::Request&, httplib::Response& res) {
            if (!rackApp || !rackApp->engine) { res.status = 503; res.set_content(err("Engine not available"), "application/json"); return; }
            float sr = 0.f;
            if (!taskQueue->postSync([rackApp, &sr]() { if (rackApp && rackApp->engine) sr = rackApp->engine->getSampleRate(); })) { res.status = 503; res.set_content(err("UI thread not responding (timed out)"), "application/json"); return; }
            res.set_content(ok("{\"sampleRate\":" + std::to_string(sr) + "}"), "application/json");
        });

        svr.Get("/library", [](const httplib::Request& req, httplib::Response& res) {
            std::string tagFilter = req.has_param("tags") ? req.get_param_value("tags") : "";
            std::string query = req.has_param("q") ? req.get_param_value("q") : "";
            std::string queryLow = query; std::transform(queryLow.begin(), queryLow.end(), queryLow.begin(), ::tolower);

            std::string body = "[";
            bool firstPlugin = true;
            for (plugin::Plugin* plug : rack::plugin::plugins) {
                std::vector<plugin::Model*> filtered;
                for (plugin::Model* m : plug->models) {
                    if (!tagFilter.empty()) {
                        bool hasTag = false;
                        for (int t : m->tagIds) {
                            std::string tn = rack::tag::getTag(t); std::transform(tn.begin(), tn.end(), tn.begin(), ::tolower);
                            if (tagFilter.find(tn) != std::string::npos) { hasTag = true; break; }
                        }
                        if (!hasTag) continue;
                    }
                    if (!queryLow.empty()) {
                        std::string s = m->slug + " " + m->name + " " + m->description; std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                        if (s.find(queryLow) == std::string::npos) continue;
                    }
                    filtered.push_back(m);
                }
                if (filtered.empty()) continue;
                if (!firstPlugin) body += ", "; firstPlugin = false;
                body += "{\"slug\":" + jsonStr(plug->slug) + ", \"name\":" + jsonStr(plug->name) + ", \"modules\": [";
                for (size_t i = 0; i < filtered.size(); i++) {
                    body += serializeModel(filtered[i]) + (i < filtered.size() - 1 ? ", " : "");
                }
                body += "]}";
            }
            body += "]";
            res.set_content(ok(body), "application/json");
        });

        svr.Get(R"(/library/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
            std::string slug = req.matches[1];
            for (plugin::Plugin* p : rack::plugin::plugins) {
                if (p->slug == slug) { res.set_content(ok(serializePlugin(p)), "application/json"); return; }
            }
            res.status = 404; res.set_content(err("Plugin not found"), "application/json");
        });

        // ── MCP Streamable HTTP transport (protocol version 2024-11-05) ─────
        // POST /mcp  – JSON-RPC 2.0 requests (initialize / tools/list / tools/call)
        svr.Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
            handleMcpPost(req, res);
        });

        // GET /mcp  – SSE stream for server-sent notifications
        svr.Get("/mcp", [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.body   = ": VCV Rack MCP Bridge\n\n";
            res.status = 200;
        });

        // Signal activity LED for every completed request (thread-safe atomic write)
        svr.set_logger([this](const httplib::Request&, const httplib::Response&) {
            if (parent) parent->activityFlag.store(true, std::memory_order_relaxed);
        });
}

void RackHttpServer::start() {
    setupRoutes();
    running = true;
    serverThread = std::thread([this]() {
        INFO("[RackMcpServer] Port %d", port);
        svr.listen("127.0.0.1", port);
        running = false;
    });
}

void RackHttpServer::stop() {
    if (running) {
        svr.stop();
        if (serverThread.joinable()) serverThread.join();
        running = false;
    }
}

RackMcpServer::~RackMcpServer() { stopServer(); }
void RackMcpServer::startServer(int port) {
    INFO("[RackMcpServer] startServer: port=%d", port);
    stopServer();
    server = new RackHttpServer();
    server->port = port;
    server->taskQueue = &taskQueue;
    server->parent = this;
    server->start();
    lights[RUNNING_LIGHT].setBrightness(1.f);
    INFO("[RackMcpServer] startServer: server running");
}
void RackMcpServer::stopServer() {
    if (server) {
        INFO("[RackMcpServer] stopServer: stopping HTTP server");
        server->stop();
        delete server;
        server = nullptr;
        INFO("[RackMcpServer] stopServer: done");
    }
    lights[RUNNING_LIGHT].setBrightness(0.f);
}

// ─── Port text field ──────────────────────────────────────────────────────

PortTextField::PortTextField() {
    multiline = false;
    color = nvgRGB(0x7d, 0xec, 0xc2);
    bgColor = nvgRGB(0x14, 0x1d, 0x33);
    textOffset = Vec(0.f, 0.f);
}

void PortTextField::step() {
    LedDisplayTextField::step();
    if (!module) return;
    if (APP->event->getSelectedWidget() != this) {
        int p = (int)module->params[RackMcpServer::PORT_PARAM].getValue();
        std::string s = std::to_string(p);
        if (text != s) setText(s);
    }
}

void PortTextField::drawLayer(const DrawArgs& args, int layer) {
    if (layer != 1) return;
    nvgScissor(args.vg, RECT_ARGS(args.clipBox));
    nvgFontFaceId(args.vg, APP->window->uiFont->handle);
    nvgFontSize(args.vg, 12.f);
    nvgFillColor(args.vg, color);
    nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgText(args.vg, box.size.x / 2.f, box.size.y / 2.f, text.c_str(), NULL);
    nvgResetScissor(args.vg);
}

void PortTextField::onSelectKey(const SelectKeyEvent& e) {
    LedDisplayTextField::onSelectKey(e);
    if (module && e.action == GLFW_PRESS && (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER)) {
        module->params[RackMcpServer::PORT_PARAM].setValue((float)std::atoi(text.c_str()));
        APP->event->setSelectedWidget(nullptr);
    }
}

void PanelLabelWidget::drawLabel(const DrawArgs& args, float x, float y, std::string txt, float fontSize, NVGcolor col, int align) {
    nvgFontFaceId(args.vg, APP->window->uiFont->handle);
    nvgFontSize(args.vg, fontSize);
    nvgTextLetterSpacing(args.vg, 0.3f);
    nvgTextAlign(args.vg, align | NVG_ALIGN_MIDDLE);
    nvgFillColor(args.vg, col);
    nvgText(args.vg, x, y, txt.c_str(), NULL);
}

void PanelLabelWidget::drawDivider(const DrawArgs& args, float y) {
    nvgBeginPath(args.vg);
    nvgMoveTo(args.vg, mm2px(4.5f), y);
    nvgLineTo(args.vg, box.size.x - mm2px(4.5f), y);
    nvgStrokeWidth(args.vg, 1.0f);
    nvgStrokeColor(args.vg, nvgRGBA(0x9a, 0xa8, 0xc9, 70));
    nvgStroke(args.vg);
}

void PanelLabelWidget::drawCard(const DrawArgs& args, float xMm, float yMm, float wMm, float hMm) {
    Rect r = Rect(mm2px(Vec(xMm, yMm)), mm2px(Vec(wMm, hMm)));
    nvgBeginPath(args.vg);
    nvgRoundedRect(args.vg, r.pos.x, r.pos.y, r.size.x, r.size.y, 7.f);
    nvgFillColor(args.vg, nvgRGBA(0x13, 0x1a, 0x2d, 150));
    nvgFill(args.vg);
    nvgStrokeWidth(args.vg, 1.2f);
    nvgStrokeColor(args.vg, nvgRGBA(0x84, 0x95, 0xbb, 90));
    nvgStroke(args.vg);
}

void PanelLabelWidget::draw(const DrawArgs& args) {
    const float cx = box.size.x / 2.f;
    const float left = mm2px(6.5f);
    NVGcolor title = nvgRGB(0xef, 0xf3, 0xff);
    NVGcolor label = nvgRGB(0xaa, 0xb5, 0xd3);

    drawLabel(args, cx, mm2px(12.f), "MCP SERVER", 11.0f, title);
    drawLabel(args, cx, mm2px(19.f), "local bridge", 6.0f, nvgRGBA(0xc0, 0xcb, 0xe8, 140));

    drawDivider(args, mm2px(27.f));
    drawLabel(args, left, mm2px(36.f), "PORT", 7.2f, label, NVG_ALIGN_LEFT);
    drawCard(args, 4.8f, 39.f, 20.9f, 12.0f);

    drawDivider(args, mm2px(57.f));
    drawLabel(args, mm2px(15.24f), mm2px(60.f), "POWER", 7.2f, label, NVG_ALIGN_CENTER);
    drawCard(args, 4.8f, 63.f, 20.9f, 12.5f);

    drawDivider(args, mm2px(78.f));
    drawLabel(args, left, mm2px(81.f), "STATUS", 7.2f, label, NVG_ALIGN_LEFT);
    drawCard(args, 4.8f, 84.f, 20.9f, 14.f);
    // Sub-labels inside STATUS card
    NVGcolor sublabel = nvgRGBA(0xaa, 0xb5, 0xd3, 160);
    drawLabel(args, mm2px(10.5f), mm2px(95.f), "ONLINE", 5.5f, sublabel, NVG_ALIGN_CENTER);
    drawLabel(args, mm2px(19.5f), mm2px(95.f), "ACTIVE", 5.5f, sublabel, NVG_ALIGN_CENTER);

    drawDivider(args, mm2px(102.f));
    drawLabel(args, left, mm2px(105.f), "CLOCK", 7.2f, label, NVG_ALIGN_LEFT);
    drawCard(args, 4.8f, 108.f, 20.9f, 14.5f);
}

// ─── Widget ───────────────────────────────────────────────────────────────

enum class MatrixTarget {
    Columns,
    Rows
};

struct MatrixTextField : TextField {
    RackMcpServer* module = nullptr;
    MatrixTarget target = MatrixTarget::Columns;

    void step() override {
        TextField::step();
        if (!module) return;
        if (APP->event->getSelectedWidget() != this) {
            int value = 0;
            {
                std::lock_guard<std::mutex> lock(module->layoutPrefsMutex);
                value = (target == MatrixTarget::Columns) ? module->layoutMatrixColsHp : module->layoutMatrixRows;
            }
            std::string s = std::to_string(value);
            if (text != s) setText(s);
        }
    }

    void onSelectKey(const SelectKeyEvent& e) override {
        TextField::onSelectKey(e);
        if (!module) return;
        if (e.action == GLFW_PRESS && (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER)) {
            int value = std::max(0, std::atoi(text.c_str()));
            std::lock_guard<std::mutex> lock(module->layoutPrefsMutex);
            if (target == MatrixTarget::Columns) module->layoutMatrixColsHp = value;
            else module->layoutMatrixRows = value;
            APP->event->setSelectedWidget(nullptr);
        }
    }
};

struct MatrixEditItem : MenuItem {
    RackMcpServer* module = nullptr;
    MatrixTarget target = MatrixTarget::Columns;

    Menu* createChildMenu() override {
        Menu* menu = new Menu;
        auto* field = createWidget<MatrixTextField>(Vec(0, 0));
        field->box.size.x = 140.f;
        field->module = module;
        field->target = target;
        {
            std::lock_guard<std::mutex> lock(module->layoutPrefsMutex);
            int value = (target == MatrixTarget::Columns) ? module->layoutMatrixColsHp : module->layoutMatrixRows;
            field->setText(std::to_string(value));
        }
        menu->addChild(field);
        return menu;
    }
};

RackMcpServerWidget::RackMcpServerWidget(RackMcpServer* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/RackMcpServer.svg")));

        // Corner screws
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Panel labels
        auto* labels = createWidget<PanelLabelWidget>(Vec(0, 0));
        labels->box.size = box.size;
        addChild(labels);

        // Port text field
        portField = createWidget<PortTextField>(mm2px(Vec(6.0f, 40.75f)));
        portField->box.size = mm2px(Vec(18.5f, 8.5f));
        portField->module = module;
        portField->setText(module
            ? std::to_string((int)module->params[RackMcpServer::PORT_PARAM].getValue())
            : "2600");
        addChild(portField);

        // Sticky enable switch
        addParam(createParamCentered<CKSS>(
            mm2px(Vec(15.24f, 69.25f)), module, RackMcpServer::ENABLED_PARAM));

        // STATUS: online LED (green) — left column
        addChild(createLightCentered<MediumLight<GreenLight>>(
            mm2px(Vec(10.5f, 90.f)), module, RackMcpServer::RUNNING_LIGHT));

        // STATUS: activity LED (yellow) — right column, blinks on API calls
        addChild(createLightCentered<MediumLight<YellowLight>>(
            mm2px(Vec(19.5f, 90.f)), module, RackMcpServer::ACTIVITY_LIGHT));

        // Heartbeat output — re-centered in shifted CLOCK card (108–122.5 mm)
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(15.24f, 115.25f)), module, RackMcpServer::HEARTBEAT_OUTPUT));

}

void RackMcpServerWidget::step() {
    ModuleWidget::step();
    RackMcpServer* m = getModule<RackMcpServer>();
    if (!m) return;
    {
        std::vector<uint64_t> toDelete;
        { std::lock_guard<std::mutex> lock(m->pendingDeleteMutex); std::swap(toDelete, m->pendingDeleteIds); }
        for (uint64_t id : toDelete) {
            engine::Module* mod = APP->engine->getModule(id);
            if (mod) {
                app::ModuleWidget* mw = APP->scene->rack->getModule(mod->id);
                if (mw) {
                    APP->scene->rack->deselectAll();
                    APP->scene->rack->select(mw);
                    APP->scene->rack->deleteSelectionAction();
                } else {
                    APP->engine->removeModule(mod);
                    delete mod;
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(m->taskQueue.mutex);
        size_t pending = m->taskQueue.tasks.size();
        if (pending > 0)
            INFO("[RackMcpServer] step: draining %zu pending task(s)", pending);
    }
    m->taskQueue.drain();
}

void RackMcpServerWidget::appendContextMenu(Menu* menu) {
    ModuleWidget::appendContextMenu(menu);
    RackMcpServer* m = getModule<RackMcpServer>();
    if (!m) return;

    menu->addChild(new MenuSeparator());
    menu->addChild(createMenuLabel("Matrix (MCP Anchor)"));

    auto editCols = new MatrixEditItem();
    editCols->text = "Columns";
    editCols->rightText = RIGHT_ARROW;
    editCols->module = m;
    editCols->target = MatrixTarget::Columns;
    menu->addChild(editCols);

    auto editRows = new MatrixEditItem();
    editRows->text = "Rows";
    editRows->rightText = RIGHT_ARROW;
    editRows->module = m;
    editRows->target = MatrixTarget::Rows;
    menu->addChild(editRows);

    auto unboundedHint = new MenuItem();
    unboundedHint->text = "0 = unbounded";
    unboundedHint->disabled = true;
    menu->addChild(unboundedHint);

}

Model* modelRackMcpServer = createModel<RackMcpServer, RackMcpServerWidget>("RackMcpServer");
