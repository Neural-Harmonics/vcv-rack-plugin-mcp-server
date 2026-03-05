/*
 * RackMcpServer.cpp
 * VCV Rack 2 Plugin — MCP HTTP Bridge
 */

#include "plugin.hpp"

// cpp-httplib (header-only)
#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include <httplib.h>

// Rack headers
#include <rack.hpp>
#include <app/RackWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <patch.hpp>
#include <tag.hpp>

#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <future>
#include <sstream>
#include <cstring>

using namespace rack;

struct RackMcpServer;
class RackHttpServer;

// ─── UI Task Queue ─────────────────────────────────────────────────────────

struct UITaskQueue {
    std::mutex mutex;
    std::queue<std::pair<std::function<void()>, std::shared_ptr<std::promise<void>>>> tasks;

    std::future<void> post(std::function<void()> fn) {
        auto p = std::make_shared<std::promise<void>>();
        {
            std::lock_guard<std::mutex> lock(mutex);
            tasks.push({fn, p});
        }
        return p->get_future();
    }

    void drain() {
        std::queue<std::pair<std::function<void()>, std::shared_ptr<std::promise<void>>>> local;
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::swap(local, tasks);
        }
        while (!local.empty()) {
            auto& [fn, p] = local.front();
            try { fn(); p->set_value(); }
            catch (...) { p->set_exception(std::current_exception()); }
            local.pop();
        }
    }
};

// ─── tiny JSON builder helpers ─────────────────────────────────────────────

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
            s += jsonKV("x", std::to_string(mw->box.pos.x));
            s += jsonKV("y", std::to_string(mw->box.pos.y), true);
        } else {
            s += jsonKV("x", "0", false);
            s += jsonKV("y", "0", true);
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

// ─── MCP tools list (JSON Schema for each tool) ────────────────────────────

static const char* MCP_TOOLS_JSON = R"json([
{"name":"vcvrack_get_status","description":"Get VCV Rack server status: version, sample rate, and loaded module count.","inputSchema":{"type":"object","properties":{}}},
{"name":"vcvrack_list_modules","description":"List all modules currently loaded in the VCV Rack patch with their IDs, slugs, and port counts.","inputSchema":{"type":"object","properties":{}}},
{"name":"vcvrack_get_module","description":"Get detailed information about a specific module: all parameters (with value ranges), inputs, and outputs.","inputSchema":{"type":"object","properties":{"id":{"type":"integer","description":"Module ID"}},"required":["id"]}},
{"name":"vcvrack_add_module","description":"Add a new module to the VCV Rack patch. Use vcvrack_search_library to discover valid plugin/slug values.","inputSchema":{"type":"object","properties":{"plugin":{"type":"string","description":"Plugin slug (e.g. 'Fundamental')"},"slug":{"type":"string","description":"Module slug (e.g. 'VCO-1')"},"x":{"type":"number","description":"X position in pixels (optional, auto-placed after rightmost module if omitted)"},"y":{"type":"number","description":"Y position in pixels (optional)"}},"required":["plugin","slug"]}},
{"name":"vcvrack_delete_module","description":"Delete a module from the VCV Rack patch by ID.","inputSchema":{"type":"object","properties":{"id":{"type":"integer","description":"Module ID to delete"}},"required":["id"]}},
{"name":"vcvrack_get_params","description":"Get all parameters of a module with names, value ranges, and current values.","inputSchema":{"type":"object","properties":{"moduleId":{"type":"integer","description":"Module ID"}},"required":["moduleId"]}},
{"name":"vcvrack_set_params","description":"Set one or more parameters on a module. Call vcvrack_get_params first to discover parameter IDs and valid value ranges.","inputSchema":{"type":"object","properties":{"moduleId":{"type":"integer","description":"Module ID"},"params":{"type":"array","description":"Array of parameter updates","items":{"type":"object","properties":{"id":{"type":"integer","description":"Parameter index (0-based)"},"value":{"type":"number","description":"New parameter value"}},"required":["id","value"]}}},"required":["moduleId","params"]}},
{"name":"vcvrack_list_cables","description":"List all cable connections in the current patch.","inputSchema":{"type":"object","properties":{}}},
{"name":"vcvrack_add_cable","description":"Connect an output port to an input port with a patch cable.","inputSchema":{"type":"object","properties":{"outputModuleId":{"type":"integer","description":"Source module ID"},"outputId":{"type":"integer","description":"Output port index (0-based)"},"inputModuleId":{"type":"integer","description":"Destination module ID"},"inputId":{"type":"integer","description":"Input port index (0-based)"}},"required":["outputModuleId","outputId","inputModuleId","inputId"]}},
{"name":"vcvrack_delete_cable","description":"Remove a cable connection by cable ID.","inputSchema":{"type":"object","properties":{"id":{"type":"integer","description":"Cable ID"}},"required":["id"]}},
{"name":"vcvrack_get_sample_rate","description":"Get the current audio engine sample rate in Hz.","inputSchema":{"type":"object","properties":{}}},
{"name":"vcvrack_search_library","description":"Search the installed plugin library for modules by name, slug, or tag. Use this to discover plugin slugs and module slugs before calling vcvrack_add_module.","inputSchema":{"type":"object","properties":{"q":{"type":"string","description":"Search query matching slug, name, or description"},"tags":{"type":"string","description":"Tag filter e.g. 'VCO', 'VCF', 'LFO', 'Envelope', 'Mixer'"}},"required":[]}},
{"name":"vcvrack_get_plugin","description":"Get detailed information about an installed plugin and its full module list.","inputSchema":{"type":"object","properties":{"slug":{"type":"string","description":"Plugin slug"}},"required":["slug"]}},
{"name":"vcvrack_save_patch","description":"Save the current VCV Rack patch to a .vcv file.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"Absolute file path (e.g. '/home/user/patches/my_patch.vcv')"}},"required":["path"]}},
{"name":"vcvrack_load_patch","description":"Load a VCV Rack patch from a .vcv file, replacing the current patch.","inputSchema":{"type":"object","properties":{"path":{"type":"string","description":"Absolute file path to load"}},"required":["path"]}}
])json";

// ─── Module Definition ─────────────────────────────────────────────────────

struct RackMcpServer : Module {
    enum ParamIds { PORT_PARAM, ENABLED_PARAM, NUM_PARAMS };
    enum InputIds { NUM_INPUTS };
    enum OutputIds { HEARTBEAT_OUTPUT, NUM_OUTPUTS };
    enum LightIds { RUNNING_LIGHT, NUM_LIGHTS };

    UITaskQueue taskQueue;
    RackHttpServer* server = nullptr;
    bool wasEnabled = false;
    float heartbeatPhase = 0.f;

    std::mutex pendingDeleteMutex;
    std::vector<uint64_t> pendingDeleteIds;

    RackMcpServer() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(PORT_PARAM, 2000.f, 9999.f, 2600.f, "HTTP Port")->snapEnabled = true;
        configParam(ENABLED_PARAM, 0.f, 1.f, 0.f, "Enable HTTP Server");
        configOutput(HEARTBEAT_OUTPUT, "Server heartbeat");
    }
    ~RackMcpServer();
    void startServer(int port);
    void stopServer();
    void process(const ProcessArgs& args) override {
        bool enabled = params[ENABLED_PARAM].getValue() > 0.5f;
        if (enabled && !wasEnabled) startServer((int)params[PORT_PARAM].getValue());
        else if (!enabled && wasEnabled) stopServer();
        wasEnabled = enabled;
        if (server) {
            heartbeatPhase += args.sampleTime;
            if (heartbeatPhase >= 1.f) heartbeatPhase -= 1.f;
            outputs[HEARTBEAT_OUTPUT].setVoltage(heartbeatPhase < 0.05f ? 10.f : 0.f);
        }
    }
    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "enabled", json_boolean(wasEnabled));
        return rootJ;
    }
    void dataFromJson(json_t* rootJ) override {
        json_t* enabledJ = json_object_get(rootJ, "enabled");
        if (enabledJ) params[ENABLED_PARAM].setValue(json_boolean_value(enabledJ) ? 1.f : 0.f);
    }
};

// ─── HTTP Server ───────────────────────────────────────────────────────────

class RackHttpServer {
public:
    httplib::Server svr;
    std::thread serverThread;
    std::atomic<bool> running{false};
    int port;
    UITaskQueue* taskQueue = nullptr;
    RackMcpServer* parent = nullptr;

    RackHttpServer() : port(2600) {}
    ~RackHttpServer() { stop(); }

    // ─── MCP tool dispatcher ────────────────────────────────────────────────

    std::string dispatchTool(const std::string& name, const std::string& args) {
        auto* rackApp = APP;

        if (name == "vcvrack_get_status") {
            float sr = 0.f; int count = 0;
            taskQueue->post([rackApp, &sr, &count]() {
                sr = rackApp->engine->getSampleRate();
                count = (int)rackApp->engine->getModuleIds().size();
            }).get();
            return toolOk("{\"server\":\"VCV Rack MCP Bridge\",\"version\":\"1.3.0\","
                          "\"sampleRate\":" + std::to_string(sr) +
                          ",\"moduleCount\":" + std::to_string(count) + "}");
        }

        if (name == "vcvrack_list_modules") {
            std::string body;
            taskQueue->post([rackApp, &body]() {
                std::vector<int64_t> ids = rackApp->engine->getModuleIds();
                body = "[";
                for (size_t i = 0; i < ids.size(); i++) {
                    engine::Module* mod = rackApp->engine->getModule(ids[i]);
                    body += (mod ? serializeModuleSummary(mod) : "null");
                    if (i < ids.size() - 1) body += ",";
                }
                body += "]";
            }).get();
            return toolOk(body);
        }

        if (name == "vcvrack_get_module") {
            std::string rawId = parseRawValue(args, "id");
            int64_t id = rawId.empty() ? -1 : (int64_t)std::stod(rawId);
            std::string body;
            taskQueue->post([rackApp, id, &body]() {
                engine::Module* mod = rackApp->engine->getModule(id);
                body = mod ? serializeModuleDetail(mod) : "";
            }).get();
            if (body.empty()) return toolFail("Module not found: " + std::to_string(id));
            return toolOk(body);
        }

        if (name == "vcvrack_add_module") {
            std::string pSlug = parseJsonString(args, "plugin");
            std::string mSlug = parseJsonString(args, "slug");
            float x = (float)parseJsonDouble(args, "x", -1.0);
            float y = (float)parseJsonDouble(args, "y", 0.0);
            plugin::Model* model = nullptr;
            for (plugin::Plugin* p : rack::plugin::plugins)
                if (p->slug == pSlug)
                    for (plugin::Model* m : p->models)
                        if (m->slug == mSlug) { model = m; break; }
            if (!model) return toolFail("Model not found: " + pSlug + "/" + mSlug);
            int64_t moduleId = -1;
            taskQueue->post([rackApp, model, x, y, &moduleId]() mutable {
                engine::Module* m = model->createModule();
                if (!m) return;
                rackApp->engine->addModule(m);
                moduleId = m->id;
                app::ModuleWidget* mw = model->createModuleWidget(m);
                if (!mw) return;
                rackApp->scene->rack->addModule(mw);
                float px = x, py = y;
                if (px < 0.f) {
                    px = 0.f; bool found = false;
                    for (app::ModuleWidget* w : rackApp->scene->rack->getModules()) {
                        float r = w->box.pos.x + w->box.size.x;
                        if (!found || r > px) { px = r; py = w->box.pos.y; found = true; }
                    }
                }
                rackApp->scene->rack->setModulePosForce(mw, math::Vec(px, py));
            }).get();
            if (moduleId < 0) return toolFail("Failed to create module");
            return toolOk("{\"id\":" + std::to_string(moduleId) +
                          ",\"plugin\":" + jsonStr(pSlug) +
                          ",\"slug\":" + jsonStr(mSlug) + "}");
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
            taskQueue->post([rackApp, id, &body]() {
                engine::Module* mod = rackApp->engine->getModule(id);
                if (!mod) return;
                body = "[";
                for (int i = 0; i < (int)mod->params.size(); i++) {
                    body += serializeParamQuantity(mod->paramQuantities[i], i);
                    if (i < (int)mod->params.size() - 1) body += ",";
                }
                body += "]";
            }).get();
            if (body.empty()) return toolFail("Module not found: " + std::to_string(id));
            return toolOk(body);
        }

        if (name == "vcvrack_set_params") {
            int64_t id = (int64_t)parseJsonDouble(args, "moduleId", -1);
            std::string paramsRaw = parseRawValue(args, "params");
            int applied = 0; bool found = false;
            taskQueue->post([rackApp, id, paramsRaw, &applied, &found]() {
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
            }).get();
            if (!found) return toolFail("Module not found: " + std::to_string(id));
            return toolOk("{\"applied\":" + std::to_string(applied) + "}");
        }

        if (name == "vcvrack_list_cables") {
            std::string body;
            taskQueue->post([rackApp, &body]() {
                std::vector<int64_t> ids = rackApp->engine->getCableIds();
                body = "[";
                for (size_t i = 0; i < ids.size(); i++) {
                    engine::Cable* c = rackApp->engine->getCable(ids[i]);
                    if (c) {
                        body += "{\"id\":" + std::to_string(c->id) +
                                ",\"outputModuleId\":" + std::to_string(c->outputModule->id) +
                                ",\"outputId\":" + std::to_string(c->outputId) +
                                ",\"inputModuleId\":" + std::to_string(c->inputModule->id) +
                                ",\"inputId\":" + std::to_string(c->inputId) + "}";
                    } else { body += "null"; }
                    if (i < ids.size() - 1) body += ",";
                }
                body += "]";
            }).get();
            return toolOk(body);
        }

        if (name == "vcvrack_add_cable") {
            int64_t outM = (int64_t)parseJsonDouble(args, "outputModuleId", -1);
            int64_t inM  = (int64_t)parseJsonDouble(args, "inputModuleId", -1);
            int outP = (int)parseJsonDouble(args, "outputId", 0);
            int inP  = (int)parseJsonDouble(args, "inputId", 0);
            int64_t cableId = -1;
            taskQueue->post([rackApp, outM, outP, inM, inP, &cableId]() {
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
            }).get();
            if (cableId < 0) return toolFail("Failed to connect: ports or modules not found");
            return toolOk("{\"id\":" + std::to_string(cableId) + "}");
        }

        if (name == "vcvrack_delete_cable") {
            int64_t id = (int64_t)parseJsonDouble(args, "id", -1);
            bool found = false;
            taskQueue->post([rackApp, id, &found]() {
                app::CableWidget* cw = rackApp->scene->rack->getCable(id);
                if (cw) {
                    found = true;
                    rackApp->scene->rack->removeCable(cw);
                    delete cw;
                } else {
                    engine::Cable* c = rackApp->engine->getCable(id);
                    if (c) { found = true; rackApp->engine->removeCable(c); delete c; }
                }
            }).get();
            if (!found) return toolFail("Cable not found: " + std::to_string(id));
            return toolOk("{\"removed\":true}");
        }

        if (name == "vcvrack_get_sample_rate") {
            float sr = 0.f;
            taskQueue->post([rackApp, &sr]() { sr = rackApp->engine->getSampleRate(); }).get();
            return toolOk("{\"sampleRate\":" + std::to_string(sr) + "}");
        }

        if (name == "vcvrack_search_library") {
            std::string tagFilter = parseJsonString(args, "tags");
            std::string query = parseJsonString(args, "q");
            std::string queryLow = query;
            std::transform(queryLow.begin(), queryLow.end(), queryLow.begin(), ::tolower);
            std::string body = "[";
            bool firstPlugin = true;
            for (plugin::Plugin* plug : rack::plugin::plugins) {
                std::vector<plugin::Model*> filtered;
                for (plugin::Model* m : plug->models) {
                    if (!tagFilter.empty()) {
                        bool hasTag = false;
                        for (int t : m->tagIds) {
                            std::string tn = rack::tag::getTag(t);
                            std::transform(tn.begin(), tn.end(), tn.begin(), ::tolower);
                            if (tagFilter.find(tn) != std::string::npos) { hasTag = true; break; }
                        }
                        if (!hasTag) continue;
                    }
                    if (!queryLow.empty()) {
                        std::string s = m->slug + " " + m->name + " " + m->description;
                        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                        if (s.find(queryLow) == std::string::npos) continue;
                    }
                    filtered.push_back(m);
                }
                if (filtered.empty()) continue;
                if (!firstPlugin) body += ","; firstPlugin = false;
                body += "{\"slug\":" + jsonStr(plug->slug) + ",\"name\":" + jsonStr(plug->name) + ",\"modules\":[";
                for (size_t i = 0; i < filtered.size(); i++) {
                    body += serializeModel(filtered[i]);
                    if (i < filtered.size() - 1) body += ",";
                }
                body += "]}";
            }
            body += "]";
            return toolOk(body);
        }

        if (name == "vcvrack_get_plugin") {
            std::string slug = parseJsonString(args, "slug");
            for (plugin::Plugin* p : rack::plugin::plugins)
                if (p->slug == slug) return toolOk(serializePlugin(p));
            return toolFail("Plugin not found: " + slug);
        }

        if (name == "vcvrack_save_patch") {
            std::string path = parseJsonString(args, "path");
            if (path.empty()) return toolFail("Missing 'path'");
            taskQueue->post([rackApp, path]() { rackApp->patch->save(path); }).get();
            return toolOk("{\"saved\":" + jsonStr(path) + "}");
        }

        if (name == "vcvrack_load_patch") {
            std::string path = parseJsonString(args, "path");
            if (path.empty()) return toolFail("Missing 'path'");
            taskQueue->post([rackApp, path]() { rackApp->patch->load(path); }).get();
            return toolOk("{\"loaded\":" + jsonStr(path) + "}");
        }

        return toolFail("Unknown tool: " + name);
    }

    // ─── MCP Streamable HTTP request handler ────────────────────────────────

    void handleMcpPost(const httplib::Request& req, httplib::Response& res) {
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
            res.set_content(mcpOk(id, R"({"protocolVersion":"2024-11-05","capabilities":{"tools":{}},"serverInfo":{"name":"VCV Rack MCP Bridge","version":"1.3.0"}})"), "application/json");
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

    void setupRoutes() {
        auto* rackApp = APP;
        svr.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.set_header("Content-Type", "application/json");
            return httplib::Server::HandlerResponse::Unhandled;
        });
        svr.Options(".*", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

        svr.Get("/status", [rackApp, this](const httplib::Request&, httplib::Response& res) {
            float sr = 0.f; int count = 0;
            taskQueue->post([rackApp, &sr, &count]() {
                sr = rackApp->engine->getSampleRate();
                count = (int)rackApp->engine->getModuleIds().size();
            }).get();
            std::string body = "{" + jsonKVs("server", "VCV Rack MCP Bridge") + jsonKVs("version", "1.3.0") +
                jsonKVs("build", std::string(__DATE__) + " " + __TIME__) +
                jsonKV("sampleRate", std::to_string(sr)) + jsonKV("moduleCount", std::to_string(count), true) + "}";
            res.set_content(ok(body), "application/json");
        });

        svr.Get("/modules", [rackApp, this](const httplib::Request&, httplib::Response& res) {
            std::string body;
            taskQueue->post([rackApp, &body]() {
                std::vector<int64_t> ids = rackApp->engine->getModuleIds();
                body = "[";
                for (size_t i = 0; i < ids.size(); i++) {
                    engine::Module* mod = rackApp->engine->getModule(ids[i]);
                    body += (mod ? serializeModuleSummary(mod) : "null") + (i < ids.size() - 1 ? ", " : "");
                }
                body += "]";
            }).get();
            res.set_content(ok(body), "application/json");
        });

        svr.Get(R"(/modules/(\d+)$)", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            int64_t id = std::stoll(req.matches[1]);
            std::string body;
            taskQueue->post([rackApp, id, &body]() {
                engine::Module* mod = rackApp->engine->getModule(id);
                body = mod ? serializeModuleDetail(mod) : "null";
            }).get();
            if (body == "null") { res.status = 404; res.set_content(err("Module not found"), "application/json"); }
            else res.set_content(ok(body), "application/json");
        });

        svr.Post("/modules/add", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            std::string pSlug = parseJsonString(req.body, "plugin"), mSlug = parseJsonString(req.body, "slug");
            float x = (float)parseJsonDouble(req.body, "x", -1.0), y = (float)parseJsonDouble(req.body, "y", 0.0);
            plugin::Model* model = nullptr;
            for (plugin::Plugin* p : rack::plugin::plugins) if (p->slug == pSlug) for (plugin::Model* m : p->models) if (m->slug == mSlug) { model = m; break; }
            if (!model) { res.status = 404; res.set_content(err("Model not found"), "application/json"); return; }
            int64_t moduleId = -1;
            taskQueue->post([rackApp, model, x, y, &moduleId]() mutable {
                engine::Module* m = model->createModule(); if (!m) return;
                rackApp->engine->addModule(m); moduleId = m->id;
                app::ModuleWidget* mw = model->createModuleWidget(m); if (!mw) return;
                rackApp->scene->rack->addModule(mw);
                float px = x, py = y;
                if (px < 0.f) {
                    px = 0.f; bool found = false;
                    for (app::ModuleWidget* w : rackApp->scene->rack->getModules()) {
                        float r = w->box.pos.x + w->box.size.x;
                        if (!found || r > px) { px = r; py = w->box.pos.y; found = true; }
                    }
                }
                rackApp->scene->rack->setModulePosForce(mw, math::Vec(px, py));
            }).get();
            if (moduleId < 0) { res.status = 500; res.set_content(err("Failed to create module"), "application/json"); }
            else res.set_content(ok("{" + jsonKV("id", std::to_string(moduleId)) + jsonKVs("plugin", pSlug) + jsonKVs("slug", mSlug, true) + "}"), "application/json");
        });

        svr.Delete(R"(/modules/(\d+)$)", [this](const httplib::Request& req, httplib::Response& res) {
            int64_t id = std::stoll(req.matches[1]);
            if (parent && parent->id == id) { res.status = 403; res.set_content(err("Cannot delete server module"), "application/json"); return; }
            if (parent) { std::lock_guard<std::mutex> lock(parent->pendingDeleteMutex); parent->pendingDeleteIds.push_back((uint64_t)id); }
            res.set_content(ok("{\"status\":\"queued\",\"id\":" + std::to_string(id) + "}"), "application/json");
        });

        svr.Get(R"(/modules/(\d+)/params)", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            int64_t id = std::stoll(req.matches[1]);
            std::string body;
            taskQueue->post([rackApp, id, &body]() {
                engine::Module* mod = rackApp->engine->getModule(id);
                if (!mod) { body = "null"; return; }
                body = "[";
                for (int i = 0; i < (int)mod->params.size(); i++) {
                    body += serializeParamQuantity(mod->paramQuantities[i], i) + (i < (int)mod->params.size() - 1 ? ", " : "");
                }
                body += "]";
            }).get();
            if (body == "null") { res.status = 404; res.set_content(err("Module not found"), "application/json"); }
            else res.set_content(ok(body), "application/json");
        });

        svr.Post(R"(/modules/(\d+)/params)", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            int64_t id = std::stoll(req.matches[1]);
            const std::string requestBody = req.body;
            int applied = 0; bool found = false;
            taskQueue->post([rackApp, id, requestBody, &applied, &found]() {
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
            }).get();
            if (!found) { res.status = 404; res.set_content(err("Module not found"), "application/json"); }
            else res.set_content(ok("{\"applied\":" + std::to_string(applied) + "}"), "application/json");
        });

        svr.Get("/cables", [rackApp, this](const httplib::Request&, httplib::Response& res) {
            std::string body;
            taskQueue->post([rackApp, &body]() {
                std::vector<int64_t> ids = rackApp->engine->getCableIds();
                body = "[";
                for (size_t i = 0; i < ids.size(); i++) {
                    engine::Cable* c = rackApp->engine->getCable(ids[i]);
                    if (c) {
                        body += "{\"id\":" + std::to_string(c->id) + ", \"outputModuleId\":" + std::to_string(c->outputModule->id) + 
                                ", \"outputId\":" + std::to_string(c->outputId) + ", \"inputModuleId\":" + std::to_string(c->inputModule->id) + 
                                ", \"inputId\":" + std::to_string(c->inputId) + "}";
                    } else { body += "null"; }
                    if (i < ids.size() - 1) body += ", ";
                }
                body += "]";
            }).get();
            res.set_content(ok(body), "application/json");
        });

        svr.Post("/cables", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            int64_t outM = (int64_t)parseJsonDouble(req.body, "outputModuleId", -1), inM = (int64_t)parseJsonDouble(req.body, "inputModuleId", -1);
            int outP = (int)parseJsonDouble(req.body, "outputId", 0), inP = (int)parseJsonDouble(req.body, "inputId", 0);
            int64_t cableId = -1;
            
            taskQueue->post([rackApp, outM, outP, inM, inP, &cableId]() {
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
            }).get();

            if (cableId < 0) { res.status = 404; res.set_content(err("Failed to connect: ports or modules not found"), "application/json"); }
            else res.set_content(ok("{\"id\":" + std::to_string(cableId) + "}"), "application/json");
        });

        svr.Delete(R"(/cables/(\d+))", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            int64_t id = std::stoll(req.matches[1]);
            bool found = false;
            
            taskQueue->post([rackApp, id, &found]() {
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
            }).get();

            if (!found) { res.status = 404; res.set_content(err("Cable not found"), "application/json"); }
            else res.set_content(ok("{\"removed\":true}"), "application/json");
        });

        svr.Get("/sample-rate", [rackApp, this](const httplib::Request&, httplib::Response& res) {
            float sr = 0.f;
            taskQueue->post([rackApp, &sr]() { sr = rackApp->engine->getSampleRate(); }).get();
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

        svr.Post("/patch/save", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            std::string path = parseJsonString(req.body, "path");
            if (path.empty()) { res.status = 400; res.set_content(err("Missing path"), "application/json"); return; }
            taskQueue->post([rackApp, path]() { rackApp->patch->save(path); }).get();
            res.set_content(ok("{\"saved\":" + jsonStr(path) + "}"), "application/json");
        });

        svr.Post("/patch/load", [rackApp, this](const httplib::Request& req, httplib::Response& res) {
            std::string path = parseJsonString(req.body, "path");
            if (path.empty()) { res.status = 400; res.set_content(err("Missing path"), "application/json"); return; }
            taskQueue->post([rackApp, path]() { rackApp->patch->load(path); }).get();
            res.set_content(ok("{\"loaded\":" + jsonStr(path) + "}"), "application/json");
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
    }

    void start() { setupRoutes(); running = true; serverThread = std::thread([this]() { INFO("[RackMcpServer] Port %d", port); svr.listen("127.0.0.1", port); running = false; }); }
    void stop() { if (running) { svr.stop(); if (serverThread.joinable()) serverThread.join(); running = false; } }
};

RackMcpServer::~RackMcpServer() { stopServer(); }
void RackMcpServer::startServer(int port) { stopServer(); server = new RackHttpServer(); server->port = port; server->taskQueue = &taskQueue; server->parent = this; server->start(); lights[RUNNING_LIGHT].setBrightness(1.f); }
void RackMcpServer::stopServer() { if (server) { server->stop(); delete server; server = nullptr; } lights[RUNNING_LIGHT].setBrightness(0.f); }

// ─── Port text field ──────────────────────────────────────────────────────

struct PortTextField : LedDisplayTextField {
    RackMcpServer* module = nullptr;
    PortTextField() {
        multiline = false;
        color = nvgRGB(0x00, 0xff, 0x66); // Bright 'terminal' green
        bgColor = nvgRGB(0x00, 0x00, 0x00); // Black background
        textOffset = Vec(2.f, 0.f);
    }
    void step() override {
        LedDisplayTextField::step();
        if (!module) return;
        // Sync from param only when not being edited
        if (APP->event->getSelectedWidget() != this) {
            int p = (int)module->params[RackMcpServer::PORT_PARAM].getValue();
            std::string s = std::to_string(p); if (text != s) setText(s);
        }
    }
    void onSelectKey(const SelectKeyEvent& e) override {
        LedDisplayTextField::onSelectKey(e);
        if (module && e.action == GLFW_PRESS && (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER)) {
            module->params[RackMcpServer::PORT_PARAM].setValue((float)std::atoi(text.c_str()));
            APP->event->setSelectedWidget(nullptr);
        }
    }
};

struct PanelLabelWidget : TransparentWidget {
    void drawLabel(const DrawArgs& args, float x, float y, std::string txt, float fontSize, NVGcolor col) {
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFontSize(args.vg, fontSize);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, col);
        nvgText(args.vg, x, y, txt.c_str(), NULL);
    }

    void draw(const DrawArgs& args) override {
        float cx = box.size.x / 2.f;
        NVGcolor bright = nvgRGB(0xda, 0xda, 0xda); // Bright gray/silver
        
        // Title
        drawLabel(args, cx, mm2px(12.f), "MCP", 13.f, bright);
        drawLabel(args, cx, mm2px(20.f), "BRIDGE", 10.f, bright);

        // Section labels
        drawLabel(args, cx, mm2px(36.f),  "PORT",   7.5f, bright);
        drawLabel(args, cx, mm2px(62.f),  "ON / OFF", 7.f, bright);
        drawLabel(args, cx, mm2px(79.f),  "STATUS", 7.f, bright);
        drawLabel(args, cx, mm2px(102.f), "BEAT",   7.f, bright);
    }
};

// ─── Widget ───────────────────────────────────────────────────────────────

struct RackMcpServerWidget : ModuleWidget {
    PortTextField* portField = nullptr;

    RackMcpServerWidget(RackMcpServer* module) {
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

        // Port text field — positioned where the knob was
        portField = createWidget<PortTextField>(mm2px(Vec(5.f, 38.f)));
        portField->box.size = mm2px(Vec(20.5f, 8.f));
        portField->module = module;
        portField->setText(module
            ? std::to_string((int)module->params[RackMcpServer::PORT_PARAM].getValue())
            : "2600");
        addChild(portField);

        // Sticky enable switch
        addParam(createParamCentered<CKSS>(
            mm2px(Vec(15.24, 68.f)), module, RackMcpServer::ENABLED_PARAM));

        // Running status LED
        addChild(createLightCentered<MediumLight<GreenLight>>(
            mm2px(Vec(15.24, 84.f)), module, RackMcpServer::RUNNING_LIGHT));

        // Heartbeat output
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(15.24, 108.f)), module, RackMcpServer::HEARTBEAT_OUTPUT));
    }

    void step() override {
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
                        // High-level safe deletion via selection action
                        APP->scene->rack->deselectAll();
                        APP->scene->rack->select(mw);
                        APP->scene->rack->deleteSelectionAction();
                    } else {
                        // Fallback: remove from engine if no widget exists
                        APP->engine->removeModule(mod);
                        delete mod;
                    }
                }
            }
        }
        m->taskQueue.drain();
    }
};

Model* modelRackMcpServer = createModel<RackMcpServer, RackMcpServerWidget>("RackMcpServer");
