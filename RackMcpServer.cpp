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
