/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Embedded MCP server for DOSBox Staging. Runs cpp-mcp's HTTP server on a
 *  background thread; tool handlers post jobs onto a main-thread queue
 *  (see mcp_queue.h) and block on a future. Normal_Loop / pause-loop on
 *  the main thread drain the queue at frame boundaries.
 */

#include "mcp_bridge.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "mcp_third_party.h"
#include "mcp_queue.h"
#include "mcp_console.h"
#include "mcp_screenshot.h"

#include "third_party/cpp-mcp/include/mcp_server.h"
#include "third_party/cpp-mcp/include/mcp_tool.h"

#include "control.h"
#include "logging.h"
#include "setup.h"

// Forward declarations of tool implementations defined in mcp_tools.cpp.
// External linkage so the link-time symbols match.
nlohmann::json tool_pause_main_thread(const nlohmann::json& args);
nlohmann::json tool_resume_main_thread(const nlohmann::json& args);
nlohmann::json tool_get_status_main_thread(const nlohmann::json& args);
nlohmann::json tool_mem_read_main_thread(const nlohmann::json& args);
nlohmann::json tool_mem_write_main_thread(const nlohmann::json& args);
nlohmann::json tool_memory_search_main_thread(const nlohmann::json& args);
nlohmann::json tool_run_command_main_thread(const nlohmann::json& args);
nlohmann::json tool_send_key_main_thread(const nlohmann::json& args);
nlohmann::json tool_send_keys_main_thread(const nlohmann::json& args);
nlohmann::json tool_set_speed_main_thread(const nlohmann::json& args);
nlohmann::json tool_screenshot_main_thread(const nlohmann::json& args);

namespace {

mcp_bridge::RequestQueue g_queue;
std::unique_ptr<mcp::server> g_server;
std::atomic<bool> g_paused{false};

// Helper: wrap a main-thread job into the cpp-mcp tool_handler signature.
// cpp-mcp passes (params, session_id) to handlers and expects a JSON
// result object containing a "content" array per the MCP tool spec.
// cpp-mcp's tools/call dispatcher passes the args object straight in (already
// extracted from params["arguments"]) and expects the handler to return the
// MCP content *array*, which cpp-mcp then wraps into the final
// {isError, content} response. So we return a json::array of one text part
// containing the JSON-stringified tool result — matches what most MCP
// clients expect ("read result.content[0].text and JSON-parse it").
mcp::tool_handler make_handler(nlohmann::json (*fn)(const nlohmann::json&))
{
	return [fn](const nlohmann::json& args,
	            const std::string& /*session_id*/) -> nlohmann::json {
		// Exceptions from `fn` propagate out of submit_and_wait via
		// future.get() and bubble up to cpp-mcp, which translates
		// them into isError=true responses.
		auto result = g_queue.submit_and_wait(
		        [fn, args]() { return fn(args); });
		return nlohmann::json::array(
		        {{{"type", "text"}, {"text", result.dump()}}});
	};
}

void register_all_tools(mcp::server& s)
{
	using mcp::tool_builder;

	s.register_tool(
	        tool_builder("pause")
	                .with_description("Pause emulation. While paused, the "
	                                  "CPU does not advance but MCP "
	                                  "tools remain serviceable.")
	                .build(),
	        make_handler(&tool_pause_main_thread));

	s.register_tool(
	        tool_builder("resume")
	                .with_description("Resume emulation if paused.")
	                .build(),
	        make_handler(&tool_resume_main_thread));

	s.register_tool(
	        tool_builder("get_status")
	                .with_description(
	                        "Return basic emulator status: paused flag, "
	                        "queue depth, server info.")
	                .build(),
	        make_handler(&tool_get_status_main_thread));

	s.register_tool(
	        tool_builder("mem_read")
	                .with_description(
	                        "Read DOS memory. Provide either 'address' "
	                        "(linear) or 'segment'+'offset'. Length in "
	                        "bytes (1..65536). Returns hex string.")
	                .with_number_param("address", "Linear physical address",
	                                   false)
	                .with_number_param("segment", "Segment (16-bit)", false)
	                .with_number_param("offset", "Offset within segment",
	                                   false)
	                .with_number_param("length", "Bytes to read", true)
	                .build(),
	        make_handler(&tool_mem_read_main_thread));

	s.register_tool(
	        tool_builder("mem_write")
	                .with_description(
	                        "Write bytes to DOS memory. Either 'address' "
	                        "or 'segment'+'offset'. 'hex' is a hex "
	                        "string of the bytes to write.")
	                .with_number_param("address", "Linear physical address",
	                                   false)
	                .with_number_param("segment", "Segment (16-bit)", false)
	                .with_number_param("offset", "Offset within segment",
	                                   false)
	                .with_string_param("hex",
	                                   "Hex-encoded bytes to write", true)
	                .build(),
	        make_handler(&tool_mem_write_main_thread));

	s.register_tool(
	        tool_builder("memory_search")
	                .with_description(
	                        "Scan emulated DOS memory for a byte pattern. "
	                        "Inputs: 'hex' (needle bytes, 1..256 bytes), "
	                        "optional 'mask_hex' same length (ff=strict, "
	                        "00=wildcard), optional 'start'/'end' linear "
	                        "addresses (default: full RAM), optional "
	                        "'max_results' (default 1024). Returns matching "
	                        "addresses, sorted ascending; matches may "
	                        "overlap. Designed for play-and-search "
	                        "reverse-engineering of game state structs.")
	                .with_string_param("hex", "Needle bytes, hex-encoded",
	                                   true)
	                .with_string_param("mask_hex",
	                                   "Per-byte mask, hex-encoded "
	                                   "(same length as 'hex')",
	                                   false)
	                .with_number_param("start",
	                                   "Linear start address (inclusive)",
	                                   false)
	                .with_number_param("end",
	                                   "Linear end address (exclusive)",
	                                   false)
	                .with_number_param("max_results",
	                                   "Cap on returned matches", false)
	                .build(),
	        make_handler(&tool_memory_search_main_thread));

	s.register_tool(
	        tool_builder("send_key")
	                .with_description(
	                        "Inject one keystroke into the BIOS keyboard "
	                        "buffer. 'key' is a key name (e.g. 'enter', "
	                        "'esc', 'f1', 'a') or numeric scancode.")
	                .with_string_param("key", "Key name or scancode", true)
	                .build(),
	        make_handler(&tool_send_key_main_thread));

	s.register_tool(
	        tool_builder("send_keys")
	                .with_description(
	                        "Type an ASCII string by injecting each "
	                        "character into the BIOS keyboard buffer.")
	                .with_string_param("text", "ASCII text to type", true)
	                .build(),
	        make_handler(&tool_send_keys_main_thread));

	s.register_tool(
	        tool_builder("run_command")
	                .with_description(
	                        "Execute a DOS command line at the user's "
	                        "shell prompt and return its visible "
	                        "teletype output. Synchronous: blocks until "
	                        "the command (.bat included) returns. "
	                        "Captures output via INT 21h stdout / INT "
	                        "29h / BIOS teletype; ANSI escapes "
	                        "(consumed by CON before teletype), BIOS "
	                        "scroll/cursor (CLS), and direct VRAM "
	                        "writers (TUI apps, games) do NOT show — "
	                        "pair with screenshot for those. Refused "
	                        "if paused or while a program is active.")
	                .with_string_param("command",
	                                   "DOS command line (printable "
	                                   "ASCII, ≤4095 chars, no NUL/CR/LF)",
	                                   true)
	                .build(),
	        make_handler(&tool_run_command_main_thread));

	s.register_tool(
	        tool_builder("set_speed")
	                .with_description(
	                        "Scale the emulated CPU rate. 'multiplier' is "
	                        "in [0.1, 100]; 1.0 == the rate the agent first "
	                        "saw (captured lazily). Disables auto-adjust.")
	                .with_number_param("multiplier",
	                                   "Speed multiplier (1.0=baseline)",
	                                   true)
	                .build(),
	        make_handler(&tool_set_speed_main_thread));

	s.register_tool(
	        tool_builder("screenshot")
	                .with_description(
	                        "Capture the most recent emulated frame as "
	                        "a PNG. Returns {width, height, format, "
	                        "image_b64} where image_b64 is base64-encoded "
	                        "PNG data. Resolution is the game's native "
	                        "render resolution (e.g. 320x200 for VGA "
	                        "mode 13h), pre-scaler.")
	                .build(),
	        make_handler(&tool_screenshot_main_thread));
}

} // namespace

namespace mcp_bridge {

// Internal accessors used by mcp_tools.cpp.
std::atomic<bool>& paused_flag()
{
	return g_paused;
}
RequestQueue& queue()
{
	return g_queue;
}

} // namespace mcp_bridge

extern "C" void MCP_AddConfigSection(void)
{
	constexpr auto only_at_start = Property::Changeable::OnlyAtStart;

	Section_prop* sec = control->AddSection_prop("mcp", nullptr,
	                                             /*changeable_at_runtime=*/false);

	auto pbool = sec->Add_bool("mcp_enabled", only_at_start, true);
	pbool->Set_help(
	        "Enable the embedded Model Context Protocol server, which lets an external\n"
	        "AI agent drive the emulator (pause, peek memory, push keystrokes, take\n"
	        "screenshots) over HTTP (enabled by default).");

	auto pstring = sec->Add_string("mcp_host", only_at_start, "127.0.0.1");
	pstring->Set_help(
	        "Address to bind the MCP server to ('127.0.0.1' by default).\n"
	        "Use '0.0.0.0' to accept connections from other machines on the network.\n"
	        "Note: the server has no authentication; only bind to non-loopback\n"
	        "addresses on trusted networks.");

	auto pint = sec->Add_int("mcp_port", only_at_start, 4747);
	pint->SetMinMax(1, 65535);
	pint->Set_help("TCP port to listen on (4747 by default).");

	pint = sec->Add_int("mcp_session_timeout", only_at_start, 600);
	pint->SetMinMax(0, 86400);
	pint->Set_help(
	        "Idle session timeout in seconds (600 by default; 0 disables the timeout).\n"
	        "Long-running agent sessions may need a higher value to avoid having to\n"
	        "re-issue the 'initialize' handshake.");

	pint = sec->Add_int("mcp_console_buffer_kb", only_at_start, 256);
	pint->SetMinMax(16, 4096);
	pint->Set_help(
	        "Size in KB of the rolling teletype buffer the MCP server retains for\n"
	        "'run_command' (and any future console-reading tools). Larger values let\n"
	        "long DIR/MEM/TYPE listings come back without truncation; smaller values\n"
	        "save memory.");
}

extern "C" bool MCP_Init(void)
{
	if (g_server) {
		LOG_WARNING("MCP: already initialized");
		return true;
	}

	auto* sec = static_cast<Section_prop*>(control->GetSection("mcp"));
	if (!sec) {
		LOG_WARNING("MCP: [mcp] config section not registered");
		return false;
	}

	if (!sec->Get_bool("mcp_enabled")) {
		LOG_MSG("MCP: disabled via [mcp] mcp_enabled=false");
		return false;
	}

	const auto buffer_kb = sec->Get_int("mcp_console_buffer_kb");
	MCP_ConsoleTap_SetCapacity(static_cast<size_t>(buffer_kb) * 1024);

	mcp::server::configuration conf;
	conf.host            = sec->Get_string("mcp_host");
	conf.port            = sec->Get_int("mcp_port");
	conf.session_timeout = static_cast<unsigned int>(
	        sec->Get_int("mcp_session_timeout"));
	conf.name    = "dosbox-mcp";
	conf.version = "0.1.0";

	try {
		g_server = std::make_unique<mcp::server>(conf);
		g_server->set_capabilities({{"tools", nlohmann::json::object()}});
		register_all_tools(*g_server);

		// Non-blocking start so DOSBox's main loop keeps the show
		// running.
		if (!g_server->start(/*blocking=*/false)) {
			LOG_WARNING("MCP: server failed to start on %s:%d",
			            conf.host.c_str(), conf.port);
			g_server.reset();
			return false;
		}
		LOG_MSG("MCP: listening on %s:%d (sse=/sse, mcp=/mcp)",
		        conf.host.c_str(), conf.port);
		return true;
	} catch (const std::exception& e) {
		LOG_WARNING("MCP: init failed: %s", e.what());
		g_server.reset();
		return false;
	}
}

extern "C" void MCP_Shutdown(void)
{
	if (!g_server) {
		return;
	}
	try {
		g_server->stop();
	} catch (...) {
	}
	g_server.reset();
	MCP_ReleaseLatestFrame();
	MCP_ConsoleTap_Disable();
}

extern "C" void MCP_PumpQueue(void)
{
	g_queue.pump();
}

extern "C" bool MCP_IsPaused(void)
{
	return g_paused.load(std::memory_order_acquire);
}

extern "C" bool MCP_IsActive(void)
{
	return g_server != nullptr;
}
