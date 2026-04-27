/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Single-slot latest-frame cache used by the MCP `screenshot` tool.
 *  CAPTURE_AddFrame populates the slot from the main thread; the
 *  screenshot tool reads it from the same thread (via the MCP request
 *  queue), so no locking is required.
 */

#ifndef DOSBOX_MCP_SCREENSHOT_H
#define DOSBOX_MCP_SCREENSHOT_H

#include "render.h"

// Called from CAPTURE_AddFrame on the main thread. Replaces the
// cached frame with a deep copy of `image`.
void MCP_CaptureLatestFrame(const RenderedImage& image);

// Called from MCP_Shutdown so the cached frame is released alongside
// the rest of the server.
void MCP_ReleaseLatestFrame(void);

#endif // DOSBOX_MCP_SCREENSHOT_H
