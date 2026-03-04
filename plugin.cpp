#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
	pluginInstance = p;

	// Register modules
	p->addModel(modelRackMcpServer);
	// Commenting out MCPBridge until it's added to plugin.json
	// p->addModel(modelMCPBridge);
}
