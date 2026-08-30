# Changelog

All notable changes to this project are documented in this file.

## 2.2.0

- added `vcvrack_menu_list` — a module's right-click context menu as label paths, built inside Rack and never drawn
- added `vcvrack_menu_invoke` — click a context-menu item by label path; scheduled onto the next UI frame (disabled items are refused)
- added `GET/POST /modules/{id}/menu`
- added a Scene-level FIFO of deferred UI tasks, one per frame, for actions that must run after the tool has replied
- UI tasks that time out are cancelled instead of running later against a dead request
- JSON string arguments are unescaped, so Windows paths and quoted labels survive parsing

## 2.1.0

- bumped plugin metadata for the `2.1.0` release
- added MCP prompts and improved tool guidance for patch-building flows
- added smarter module placement with `nearModuleId`
- improved module deletion safety
- added JSON helper unit tests and integration tests for the HTTP server
- improved build and packaging configuration across supported platforms

## 2.0.0

- initial VCV Rack 2 plugin release
- embedded local HTTP and MCP server inside the `MCP Server` Rack module
- added REST endpoints for patch inspection, module management, cable management, and patch save/load
- added CLI helper scripts and usage examples for automating Rack patches
