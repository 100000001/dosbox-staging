/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Thin shim that pulls in cpp-mcp's bundled nlohmann::json under a stable
 *  include path so the rest of src/mcp/*.{h,cpp} doesn't need to know
 *  about the vendor layout.
 */

#ifndef DOSBOX_MCP_THIRD_PARTY_H
#define DOSBOX_MCP_THIRD_PARTY_H

#include "json.hpp"

#endif
