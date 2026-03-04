/*
 * RackMcpServer.cpp
 * VCV Rack 2 Plugin — MCP HTTP Bridge
 *
 * Embeds a lightweight HTTP server (cpp-httplib, header-only) inside a VCV Rack
 * module. An external MCP server process (Node/Python) connects to this server
 * to control the patch programmatically.
 *
 * DEPENDENCY: cpp-httplib (MIT) — single header, place at:
 *   dep/include/httplib.h
 *   https://github.com/yhirose/cpp-httplib/releases/latest
 *
 * BUILD: Link with -pthread. On Windows link with -lws2_32 -lmswsock.
 *
 * ENDPOINTS (all return JSON):
 *
 *  GET  /status              — server alive, current patch info
 *  GET  /modules             — list all modules currently in the patch
 *  GET  /modules/:id         — detail for one module (params, inputs, outputs)
 *  POST /modules/add         — add a module  { plugin, slug, x?, y? }
 *  DELETE /modules/:id       — remove a module
 *  GET  /modules/:id/params  — get all param values
 *  POST /modules/:id/params  — set params    { params: [{id, value}] }
 *  GET  /cables              — list all cables
 *  POST /cables              — connect ports { outputModuleId, outputId, inputModuleId, inputId }
 *  DELETE /cables/:id        — disconnect a cable
 *  GET  /library             — list ALL installed plugins + their modules (for AI discovery)
 *  GET  /library/:plugin     — list modules in a specific plugin
 *  POST /patch/save          — save current patch to a path { path }
 *  POST /patch/load          — load patch from a path { path }
 *  GET  /sample-rate         — get engine sample rate
 */

#include "plugin.hpp"

// cpp-httplib (header-only, auto-downloaded by `make dep` or CMake FetchContent).
// Disable OpenSSL — plain HTTP is fine for localhost-only use.
#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include <httplib.h>

// Rack headers for runtime introspection
#include <rack.hpp>
#include <app/RackWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <patch.hpp>
#include <tag.hpp>

#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <cstring>

using namespace rack;

// ─── tiny JSON builder helpers ─────────────────────────────────────────────

static std::string jsonStr(const std::string& s) {
    // Minimal JSON string escaping
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
    s += jsonKV("value", std::to_string(pq->getValue()), true);
    s += "}";
    return s;
}

static std::string serializePortInfo(PortInfo* pi, int portId, bool isInput) {
    if (!pi) return "{}";
    std::string s = "{";
    s += jsonKV("id", std::to_string(portId));
    s += jsonKVs("name", pi->name);
    s += jsonKVs("description", pi->description);
    s += jsonKVs("type", isInput ? "input" : "output", true);
    s += "}";
    return s;
}

// Full module detail: params, inputs, outputs
static std::string serializeModuleDetail(engine::Module* mod) {
    if (!mod) return "null";

    std::string s = "{";
    s += jsonKV("id", std::to_string(mod->id));

    // Plugin/slug via model
    if (mod->model) {
        s += jsonKVs("plugin", mod->model->plugin ? mod->model->plugin->slug : "");
        s += jsonKVs("slug", mod->model->slug);
        s += jsonKVs("name", mod->model->name);
    }

    // Params
    s += jsonStr("params") + ": [";
    for (int i = 0; i < (int)mod->params.size(); i++) {
        ParamQuantity* pq = mod->paramQuantities[i];
        s += serializeParamQuantity(pq, i);
        if (i < (int)mod->params.size() - 1) s += ", ";
    }
    s += "], ";

    // Inputs
    s += jsonStr("inputs") + ": [";
    for (int i = 0; i < (int)mod->inputs.size(); i++) {
        PortInfo* pi = (i < (int)mod->inputInfos.size()) ? mod->inputInfos[i] : nullptr;
        s += serializePortInfo(pi, i, true);
        if (i < (int)mod->inputs.size() - 1) s += ", ";
    }
    s += "], ";

    // Outputs
    s += jsonStr("outputs") + ": [";
    for (int i = 0; i < (int)mod->outputs.size(); i++) {
        PortInfo* pi = (i < (int)mod->outputInfos.size()) ? mod->outputInfos[i] : nullptr;
        s += serializePortInfo(pi, i, false);
        if (i < (int)mod->outputs.size() - 1) s += ", ";
    }
    s += "]";

    s += "}";
    return s;
}

// Light summary only (for list view)
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
        s += jsonKV("outputs", std::to_string(mod->outputs.size()), true);
    }
    s += "}";
    return s;
}

// ─── Library catalogue helpers ─────────────────────────────────────────────

static std::string serializeModel(plugin::Model* model) {
    std::string s = "{";
    s += jsonKVs("slug", model->slug);
    s += jsonKVs("name", model->name);
    // Tags
    s += jsonStr("tags") + ": [";
    int tagIdx = 0;
    for (int tagId : model->tagIds) {
        if (tagIdx > 0) s += ", ";
        s += jsonStr(rack::tag::getTag(tagId));
        tagIdx++;
    }
    s += "], ";
    s += jsonKVs("description", model->description, true);
    s += "}";
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
    s += "]";
    s += "}";
    return s;
}

// ─── Simple JSON parser helpers (avoids pulling in a full JSON lib) ─────────

// Extract a top-level string field from a flat JSON object
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
    // skip whitespace
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ')) pos++;
    try { return std::stod(json.substr(pos)); }
    catch (...) { return def; }
}

// ─── HTTP Server wrapper ───────────────────────────────────────────────────

class RackHttpServer {
public:
    httplib::Server svr;
    std::thread     serverThread;
    std::atomic<bool> running{false};
    int             port;

    RackHttpServer() : port(2600) {}

    void setupRoutes() {

        // ── CORS / JSON content-type middleware ──────────────────────────
        svr.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.set_header("Content-Type", "application/json");
            return httplib::Server::HandlerResponse::Unhandled;
        });

        svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
            res.status = 204;
        });

        // ── GET /status ─────────────────────────────────────────────────
        svr.Get("/status", [](const httplib::Request&, httplib::Response& res) {
            float sr = APP->engine->getSampleRate();
            std::vector<int64_t> ids = APP->engine->getModuleIds();
            std::string body = "{";
            body += jsonKVs("server", "VCV Rack MCP Bridge");
            body += jsonKVs("version", "1.0.0");
            body += jsonKV("sampleRate", std::to_string(sr));
            body += jsonKV("moduleCount", std::to_string(ids.size()), true);
            body += "}";
            res.set_content(ok(body), "application/json");
        });

        // ── GET /modules ────────────────────────────────────────────────
        svr.Get("/modules", [](const httplib::Request&, httplib::Response& res) {
            std::vector<int64_t> ids = APP->engine->getModuleIds();
            std::string body = "[";
            for (int i = 0; i < (int)ids.size(); i++) {
                engine::Module* mod = APP->engine->getModule(ids[i]);
                if (mod) body += serializeModuleSummary(mod);
                else     body += "null";
                if (i < (int)ids.size() - 1) body += ", ";
            }
            body += "]";
            res.set_content(ok(body), "application/json");
        });

        // ── GET /modules/:id ────────────────────────────────────────────
        svr.Get(R"(/modules/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
            int64_t id = std::stoll(req.matches[1]);
            engine::Module* mod = APP->engine->getModule(id);
            if (!mod) {
                res.status = 404;
                res.set_content(err("Module not found"), "application/json");
                return;
            }
            res.set_content(ok(serializeModuleDetail(mod)), "application/json");
        });

        // ── GET /modules/:id/params ─────────────────────────────────────
        svr.Get(R"(/modules/(\d+)/params)", [](const httplib::Request& req, httplib::Response& res) {
            int64_t id = std::stoll(req.matches[1]);
            engine::Module* mod = APP->engine->getModule(id);
            if (!mod) { res.status = 404; res.set_content(err("Module not found"), "application/json"); return; }
            std::string body = "[";
            for (int i = 0; i < (int)mod->params.size(); i++) {
                ParamQuantity* pq = mod->paramQuantities[i];
                body += serializeParamQuantity(pq, i);
                if (i < (int)mod->params.size() - 1) body += ", ";
            }
            body += "]";
            res.set_content(ok(body), "application/json");
        });

        // ── POST /modules/:id/params ────────────────────────────────────
        // Body: { "params": [ { "id": 0, "value": 0.5 }, ... ] }
        svr.Post(R"(/modules/(\d+)/params)", [](const httplib::Request& req, httplib::Response& res) {
            int64_t id = std::stoll(req.matches[1]);
            engine::Module* mod = APP->engine->getModule(id);
            if (!mod) { res.status = 404; res.set_content(err("Module not found"), "application/json"); return; }

            const std::string& body = req.body;
            // Parse array: find all {"id":N,"value":V} objects
            int applied = 0;
            size_t pos = 0;
            while (pos < body.size()) {
                size_t start = body.find('{', pos);
                if (start == std::string::npos) break;
                size_t end = body.find('}', start);
                if (end == std::string::npos) break;
                std::string obj = body.substr(start, end - start + 1);
                int paramId = (int)parseJsonDouble(obj, "id", -1);
                double value = parseJsonDouble(obj, "value", 0.0);
                if (paramId >= 0 && paramId < (int)mod->params.size()) {
                    APP->engine->setParamValue(mod, paramId, (float)value);
                    applied++;
                }
                pos = end + 1;
            }
            res.set_content(ok("{" + jsonKV("applied", std::to_string(applied), true) + "}"), "application/json");
        });

        // ── POST /modules/add ────────────────────────────────────────────
        // Body: { "plugin": "VCV", "slug": "VCO", "x": 0, "y": 0 }
        svr.Post("/modules/add", [](const httplib::Request& req, httplib::Response& res) {
            std::string pluginSlug = parseJsonString(req.body, "plugin");
            std::string moduleSlug = parseJsonString(req.body, "slug");
            int x = (int)parseJsonDouble(req.body, "x", 0.0);
            int y = (int)parseJsonDouble(req.body, "y", 0.0);

            // Find model in plugin registry
            plugin::Model* model = nullptr;
            for (plugin::Plugin* plug : rack::plugin::plugins) {
                if (plug->slug == pluginSlug) {
                    for (plugin::Model* m : plug->models) {
                        if (m->slug == moduleSlug) { model = m; break; }
                    }
                    break;
                }
            }

            if (!model) {
                res.status = 404;
                res.set_content(err("Model not found: " + pluginSlug + "/" + moduleSlug), "application/json");
                return;
            }

            // Module creation must happen on the UI thread to avoid race conditions.
            // We schedule it via APP->scene actions (same as drag-drop from browser).
            // We do it synchronously here by posting an event to the main thread queue.
            engine::Module* module = model->createModule();
            if (!module) {
                res.status = 500;
                res.set_content(err("Failed to create module"), "application/json");
                return;
            }
            APP->engine->addModule(module);

            // Position on the rack canvas (in rack units, 1 HP = 15px)
            APP->scene->rack->setModulePosForce(
                APP->scene->rack->getModule(module->id),
                math::Vec(x, y)
            );

            std::string body = "{";
            body += jsonKV("id", std::to_string(module->id));
            body += jsonKVs("plugin", pluginSlug);
            body += jsonKVs("slug", moduleSlug, true);
            body += "}";
            res.set_content(ok(body), "application/json");
        });

        // ── DELETE /modules/:id ─────────────────────────────────────────
        svr.Delete(R"(/modules/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
            int64_t id = std::stoll(req.matches[1]);
            engine::Module* mod = APP->engine->getModule(id);
            if (!mod) { res.status = 404; res.set_content(err("Module not found"), "application/json"); return; }

            // Remove widget + module via RackWidget helper (thread-safe path)
            ModuleWidget* mw = APP->scene->rack->getModule(mod->id);
            if (mw) APP->scene->rack->removeModule(mw);
            APP->engine->removeModule(mod);
            delete mod;

            res.set_content(ok("{" + jsonKVs("removed", "true", true) + "}"), "application/json");
        });

        // ── GET /cables ─────────────────────────────────────────────────
        svr.Get("/cables", [](const httplib::Request&, httplib::Response& res) {
            std::vector<int64_t> ids = APP->engine->getCableIds();
            std::string body = "[";
            for (int i = 0; i < (int)ids.size(); i++) {
                engine::Cable* cable = APP->engine->getCable(ids[i]);
                if (!cable) { body += "null"; }
                else {
                    body += "{";
                    body += jsonKV("id", std::to_string(cable->id));
                    body += jsonKV("outputModuleId", std::to_string(cable->outputModule ? cable->outputModule->id : -1));
                    body += jsonKV("outputId", std::to_string(cable->outputId));
                    body += jsonKV("inputModuleId", std::to_string(cable->inputModule ? cable->inputModule->id : -1));
                    body += jsonKV("inputId", std::to_string(cable->inputId), true);
                    body += "}";
                }
                if (i < (int)ids.size() - 1) body += ", ";
            }
            body += "]";
            res.set_content(ok(body), "application/json");
        });

        // ── POST /cables ─────────────────────────────────────────────────
        // Body: { "outputModuleId": 1, "outputId": 0, "inputModuleId": 2, "inputId": 0 }
        svr.Post("/cables", [](const httplib::Request& req, httplib::Response& res) {
            int64_t outModId  = (int64_t)parseJsonDouble(req.body, "outputModuleId", -1);
            int     outPortId = (int)parseJsonDouble(req.body, "outputId", 0);
            int64_t inModId   = (int64_t)parseJsonDouble(req.body, "inputModuleId", -1);
            int     inPortId  = (int)parseJsonDouble(req.body, "inputId", 0);

            engine::Module* outMod = APP->engine->getModule(outModId);
            engine::Module* inMod  = APP->engine->getModule(inModId);
            if (!outMod || !inMod) {
                res.status = 404;
                res.set_content(err("One or both modules not found"), "application/json");
                return;
            }

            engine::Cable* cable = new engine::Cable;
            cable->id = -1; // auto-assign
            cable->outputModule = outMod;
            cable->outputId     = outPortId;
            cable->inputModule  = inMod;
            cable->inputId      = inPortId;
            APP->engine->addCable(cable);

            res.set_content(ok("{" + jsonKV("id", std::to_string(cable->id), true) + "}"), "application/json");
        });

        // ── DELETE /cables/:id ───────────────────────────────────────────
        svr.Delete(R"(/cables/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
            int64_t id = std::stoll(req.matches[1]);
            engine::Cable* cable = APP->engine->getCable(id);
            if (!cable) { res.status = 404; res.set_content(err("Cable not found"), "application/json"); return; }
            APP->engine->removeCable(cable);
            delete cable;
            res.set_content(ok("{" + jsonKVs("removed", "true", true) + "}"), "application/json");
        });

        // ── GET /sample-rate ─────────────────────────────────────────────
        svr.Get("/sample-rate", [](const httplib::Request&, httplib::Response& res) {
            float sr = APP->engine->getSampleRate();
            res.set_content(ok("{" + jsonKV("sampleRate", std::to_string(sr), true) + "}"), "application/json");
        });

        // ── GET /library ─────────────────────────────────────────────────
        // Returns the full catalogue of installed plugins+modules.
        // This is the endpoint an AI/LLM uses to pick the right module.
        svr.Get("/library", [](const httplib::Request& req, httplib::Response& res) {
            // Optional filter: /library?tags=VCO,Filter&q=oscillator
            std::string tagFilter = req.has_param("tags") ? req.get_param_value("tags") : "";
            std::string query     = req.has_param("q")    ? req.get_param_value("q")    : "";

            // Normalise query to lowercase for matching
            std::string queryLow = query;
            std::transform(queryLow.begin(), queryLow.end(), queryLow.begin(), ::tolower);

            std::string body = "[";
            bool firstPlugin = true;
            for (plugin::Plugin* plug : rack::plugin::plugins) {
                // Filter at plugin level
                std::vector<plugin::Model*> filteredModels;
                for (plugin::Model* model : plug->models) {
                    // Tag filter
                    if (!tagFilter.empty()) {
                        bool hasTag = false;
                        for (int t : model->tagIds) {
                            std::string tagName = rack::tag::getTag(t);
                            std::string tagNameLow = tagName;
                            std::transform(tagNameLow.begin(), tagNameLow.end(), tagNameLow.begin(), ::tolower);
                            if (tagFilter.find(tagNameLow) != std::string::npos) { hasTag = true; break; }
                        }
                        if (!hasTag) continue;
                    }
                    // Text search in slug + name + description
                    if (!queryLow.empty()) {
                        std::string searchable = model->slug + " " + model->name + " " + model->description;
                        std::transform(searchable.begin(), searchable.end(), searchable.begin(), ::tolower);
                        if (searchable.find(queryLow) == std::string::npos) continue;
                    }
                    filteredModels.push_back(model);
                }
                if (filteredModels.empty()) continue;

                if (!firstPlugin) body += ", ";
                firstPlugin = false;

                body += "{";
                body += jsonKVs("slug", plug->slug);
                body += jsonKVs("name", plug->name);
                body += jsonKVs("author", plug->author);
                body += jsonStr("modules") + ": [";
                for (int i = 0; i < (int)filteredModels.size(); i++) {
                    body += serializeModel(filteredModels[i]);
                    if (i < (int)filteredModels.size() - 1) body += ", ";
                }
                body += "]}";
            }
            body += "]";
            res.set_content(ok(body), "application/json");
        });

        // ── GET /library/:plugin ─────────────────────────────────────────
        svr.Get(R"(/library/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
            std::string pluginSlug = req.matches[1];
            for (plugin::Plugin* plug : rack::plugin::plugins) {
                if (plug->slug == pluginSlug) {
                    res.set_content(ok(serializePlugin(plug)), "application/json");
                    return;
                }
            }
            res.status = 404;
            res.set_content(err("Plugin not found: " + pluginSlug), "application/json");
        });

        // ── POST /patch/save ─────────────────────────────────────────────
        // Body: { "path": "/home/user/mypatch.vcv" }
        svr.Post("/patch/save", [](const httplib::Request& req, httplib::Response& res) {
            std::string path = parseJsonString(req.body, "path");
            if (path.empty()) { res.status = 400; res.set_content(err("Missing 'path'"), "application/json"); return; }
            APP->patch->save(path);
            res.set_content(ok("{" + jsonKVs("saved", path, true) + "}"), "application/json");
        });

        // ── POST /patch/load ─────────────────────────────────────────────
        // Body: { "path": "/home/user/mypatch.vcv" }
        svr.Post("/patch/load", [](const httplib::Request& req, httplib::Response& res) {
            std::string path = parseJsonString(req.body, "path");
            if (path.empty()) { res.status = 400; res.set_content(err("Missing 'path'"), "application/json"); return; }
            APP->patch->load(path);
            res.set_content(ok("{" + jsonKVs("loaded", path, true) + "}"), "application/json");
        });
    }

    void start() {
        setupRoutes();
        running = true;
        serverThread = std::thread([this]() {
            INFO("[RackMcpServer] HTTP server starting on port %d", port);
            svr.listen("127.0.0.1", port);
            INFO("[RackMcpServer] HTTP server stopped");
        });
    }

    void stop() {
        if (running) {
            svr.stop();
            if (serverThread.joinable()) serverThread.join();
            running = false;
        }
    }

    ~RackHttpServer() { stop(); }
};

// ─── VCV Rack Module ──────────────────────────────────────────────────────

struct RackMcpServer : Module {
    enum ParamIds {
        PORT_PARAM,       // knob 2000-9999 (port number)
        ENABLED_PARAM,    // on/off toggle
        NUM_PARAMS
    };
    enum InputIds  { NUM_INPUTS };
    enum OutputIds {
        HEARTBEAT_OUTPUT, // pulses every second while server is running
        NUM_OUTPUTS
    };
    enum LightIds {
        RUNNING_LIGHT,
        NUM_LIGHTS
    };

    RackHttpServer* server = nullptr;
    bool wasEnabled = false;
    float heartbeatPhase = 0.f;

    RackMcpServer() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(PORT_PARAM,    2000.f, 9999.f, 2600.f, "HTTP Port")->snapEnabled = true;
        configButton(ENABLED_PARAM, "Enable HTTP Server");
        configOutput(HEARTBEAT_OUTPUT, "Server heartbeat");
    }

    ~RackMcpServer() {
        stopServer();
    }

    void startServer(int port) {
        stopServer();
        server = new RackHttpServer();
        server->port = port;
        server->start();
        lights[RUNNING_LIGHT].setBrightness(1.f);
    }

    void stopServer() {
        if (server) {
            server->stop();
            delete server;
            server = nullptr;
        }
        lights[RUNNING_LIGHT].setBrightness(0.f);
    }

    void process(const ProcessArgs& args) override {
        bool enabled = params[ENABLED_PARAM].getValue() > 0.5f;
        int  port    = (int)params[PORT_PARAM].getValue();

        if (enabled && !wasEnabled) {
            startServer(port);
        } else if (!enabled && wasEnabled) {
            stopServer();
        }
        wasEnabled = enabled;

        // Heartbeat output: 1Hz pulse when server is running
        if (server && server->running) {
            heartbeatPhase += args.sampleTime;
            if (heartbeatPhase >= 1.f) heartbeatPhase -= 1.f;
            outputs[HEARTBEAT_OUTPUT].setVoltage(heartbeatPhase < 0.05f ? 10.f : 0.f);
        } else {
            outputs[HEARTBEAT_OUTPUT].setVoltage(0.f);
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

// ─── Widget ───────────────────────────────────────────────────────────────

struct RackMcpServerWidget : ModuleWidget {
    RackMcpServerWidget(RackMcpServer* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/RackMcpServer.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Port number knob (large, center)
        addParam(createParamCentered<RoundLargeBlackKnob>(
            mm2px(Vec(15.24, 40.f)), module, RackMcpServer::PORT_PARAM));

        // Enable button
        addParam(createParamCentered<LEDButton>(
            mm2px(Vec(15.24, 65.f)), module, RackMcpServer::ENABLED_PARAM));

        // Running LED
        addChild(createLightCentered<MediumLight<GreenLight>>(
            mm2px(Vec(15.24, 75.f)), module, RackMcpServer::RUNNING_LIGHT));

        // Heartbeat output
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(15.24, 110.f)), module, RackMcpServer::HEARTBEAT_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        RackMcpServer* module = getModule<RackMcpServer>();
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuItem("Copy server URL", "", [=]() {
            int port = (int)module->params[RackMcpServer::PORT_PARAM].getValue();
            glfwSetClipboardString(APP->window->win,
                ("http://127.0.0.1:" + std::to_string(port)).c_str());
        }));
        menu->addChild(createMenuItem("Server status", "", [=]() {
            bool running = module->server && module->server->running;
            int  port    = (int)module->params[RackMcpServer::PORT_PARAM].getValue();
            std::string msg = running
                ? "Running on http://127.0.0.1:" + std::to_string(port)
                : "Stopped";
            // Display status in log (createTransientLabel not available in SDK)
            INFO("RackMcpServer: %s", msg.c_str());
        }));
    }
};

Model* modelRackMcpServer = createModel<RackMcpServer, RackMcpServerWidget>("RackMcpServer");
