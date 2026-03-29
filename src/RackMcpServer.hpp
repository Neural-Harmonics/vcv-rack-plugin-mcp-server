#pragma once

#include "plugin.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include <httplib.h>

#include <atomic>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

struct UITaskQueue {
    struct Task {
        std::function<void()>          fn;
        std::shared_ptr<std::promise<void>> promise;
        std::string                    label;
    };

    std::mutex       mutex;
    std::queue<Task> tasks;

    std::future<void> post(std::function<void()> fn, const std::string& label = "");
    // Returns false if the UI thread did not execute the task within timeoutSecs.
    bool postSync(std::function<void()> fn, const std::string& label = "", int timeoutSecs = 10);
    void drain();
};

struct RackMcpServer : Module {
    enum ParamIds { PORT_PARAM, ENABLED_PARAM, NUM_PARAMS };
    enum InputIds { NUM_INPUTS };
    enum OutputIds { HEARTBEAT_OUTPUT, NUM_OUTPUTS };
    enum LightIds { RUNNING_LIGHT, ACTIVITY_LIGHT, NUM_LIGHTS };

    UITaskQueue taskQueue;
    class RackHttpServer* server = nullptr;
    bool wasEnabled = false;
    float heartbeatPhase = 0.f;

    // Activity indicator — written by HTTP thread, read by audio thread
    std::atomic<bool> activityFlag{false};
    float activityTimer = 0.f;

    std::mutex pendingDeleteMutex;
    std::vector<uint64_t> pendingDeleteIds;

    // User-configurable layout bounds, anchored from the MCP module position.
    // cols_hp/rows set to 0 mean "unbounded".
    std::mutex layoutPrefsMutex;
    int layoutMatrixColsHp = 8;
    int layoutMatrixRows = 4;

    RackMcpServer();
    ~RackMcpServer() override;

    void startServer(int port);
    void stopServer();
    void process(const ProcessArgs& args) override;
    json_t* dataToJson() override;
    void dataFromJson(json_t* rootJ) override;
};

class RackHttpServer {
public:
    httplib::Server svr;
    std::thread serverThread;
    std::atomic<bool> running{false};
    int port = 2600;
    UITaskQueue* taskQueue = nullptr;
    RackMcpServer* parent = nullptr;
    rack::Context* rackApp = nullptr;

    RackHttpServer() = default;
    ~RackHttpServer();

    std::string getLayoutPrefsJson();
    rack::math::Vec computeAutoPosition(int64_t nearModuleId = -1, float moduleWidth = 0.f);
    std::string dispatchTool(const std::string& name, const std::string& args);
    void handleMcpPost(const httplib::Request& req, httplib::Response& res);
    void setupRoutes();
    void start();
    void stop();
};

struct PortTextField : LedDisplayTextField {
    RackMcpServer* module = nullptr;

    PortTextField();
    void step() override;
    void drawLayer(const DrawArgs& args, int layer) override;
    void onSelectKey(const SelectKeyEvent& e) override;
};

struct PanelLabelWidget : TransparentWidget {
    void drawLabel(const DrawArgs& args, float x, float y, std::string txt, float fontSize, NVGcolor col, int align = NVG_ALIGN_CENTER);
    void drawDivider(const DrawArgs& args, float y);
    void drawCard(const DrawArgs& args, float xMm, float yMm, float wMm, float hMm);
    void draw(const DrawArgs& args) override;
};

struct RackMcpServerWidget : ModuleWidget {
    PortTextField* portField = nullptr;

    explicit RackMcpServerWidget(RackMcpServer* module);
    void step() override;
    void appendContextMenu(Menu* menu) override;
};
