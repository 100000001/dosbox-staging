/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Teletype tap: a long-lived ring buffer of bytes that pass through
 *  DOSBox's `teletype_output_attr` template (INT 21h stdout, INT 29h,
 *  BIOS teletype, read-echo). Consumers (the run_command MCP tool, plus
 *  any future "wait_for_text" / "console_tail" tools) capture the
 *  current write count via Mark(), let the emulator run, then ReadSince
 *  to retrieve the UTF-8 of bytes that landed in between.
 *
 *  Boundaries this tap does NOT cover:
 *  - ANSI escapes consumed by device_CON before reaching teletype.
 *  - BIOS scroll/cursor (CLS uses INT 10h AH=06h, not teletype).
 *  - Direct VRAM writers (TUI apps, games at 0xB8000 / 0xA0000).
 *  Callers that need those should pair with `screenshot`.
 */

#ifndef DOSBOX_MCP_CONSOLE_H
#define DOSBOX_MCP_CONSOLE_H

#include <cstddef>
#include <cstdint>
#include <string>

// Called from inside emulator code paths whenever a byte is about to
// become visible on the emulated text console. Cheap; safe to call
// before MCP_Init has finished.
void MCP_ConsoleTap_Notify(uint8_t byte);

// Snapshot of the running write counter. Pass to ReadSince later to get
// everything that arrived in between.
uint64_t MCP_ConsoleTap_Mark(void);

// Convert the bytes captured since `cursor` from the active DOS code
// page to UTF-8 (preserving control codes). Sets `truncated = true` if
// the ring rolled over and the head of the requested window was lost;
// in that case the returned bytes are the most recent (the tail), not
// the start.
std::string MCP_ConsoleTap_ReadSince(uint64_t cursor, bool& truncated);

// Reallocates the ring buffer and resets the write counter. Intended
// to be called once during MCP_Init from the [mcp] config. Capacity is
// clamped to [16 KB, 4 MB].
void MCP_ConsoleTap_SetCapacity(size_t bytes);

#endif // DOSBOX_MCP_CONSOLE_H
