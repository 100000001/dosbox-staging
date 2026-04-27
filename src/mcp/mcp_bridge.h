/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Public C-style entry points for the embedded MCP server. Designed to be
 *  callable from the DOSBox main thread (lifecycle + pump) without leaking
 *  C++ types into the rest of the codebase.
 */

#ifndef DOSBOX_MCP_BRIDGE_H
#define DOSBOX_MCP_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

// Register the [mcp] config section. Must be called once during config
// setup (before ParseConfigFiles), alongside config_add_sdl() and
// DOSBOX_Init().
void MCP_AddConfigSection(void);

// Lifecycle. Reads the [mcp] section registered by MCP_AddConfigSection
// and starts the server if enabled=true. Returns false on failure (e.g.
// port already in use) or if disabled (caller doesn't need to
// distinguish — MCP_Shutdown is safe in either case).
bool MCP_Init(void);
void MCP_Shutdown(void);

// Drain any pending tool requests posted by cpp-mcp worker threads onto
// the main thread. Cheap when idle; safe to call as often as the main
// loop iterates. MUST be called from the main loop both when emulation
// is running and when it is paused — otherwise paused-state tools
// (notably MCP_Resume) will deadlock waiting for the pump.
void MCP_PumpQueue(void);

// Pause/resume helpers, exposed to dosbox.cpp so Normal_Loop can early
// out when paused. The pause flag is owned by the MCP server; tools
// flip it via the main-thread queue, then DOSBOX_RunMachine reads it
// directly.
bool MCP_IsPaused(void);

#ifdef __cplusplus
}
#endif

#endif // DOSBOX_MCP_BRIDGE_H
