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

#include "third_party/cpp-mcp/include/mcp_server.h"
#include "third_party/cpp-mcp/include/mcp_tool.h"

#include "logging.h"

// Forward declarations of tool implementations defined in mcp_tools.cpp.
// External linkage so the link-time symbols match.
nlohmann::json tool_pause_main_thread(const nlohmann::json& args);
nlohmann::json tool_resume_main_thread(const nlohmann::json& args);
nlohmann::json tool_get_status_main_thread(const nlohmann::json& args);
nlohmann::json tool_mem_read_main_thread(const nlohmann::json& args);
nlohmann::json tool_mem_write_main_thread(const nlohmann::json& args);
nlohmann::json tool_send_key_main_thread(const nlohmann::json& args);
nlohmann::json tool_send_keys_main_thread(const nlohmann::json& args);

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
		nlohmann::json result;
		try {
			result = g_queue.submit_and_wait(
			        [fn, args]() { return fn(args); });
		} catch (const std::exception& e) {
			// Re-throw so cpp-mcp marks isError=true with the message.
			throw;
		}
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

extern "C" bool MCP_Init(const char* host, int port)
{
	if (g_server) {
		LOG_WARNING("MCP: already initialized");
		return true;
	}

	mcp::server::configuration conf;
	conf.host    = host ? host : "127.0.0.1";
	conf.port    = port;
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
}

extern "C" void MCP_PumpQueue(void)
{
	g_queue.pump();
}

extern "C" bool MCP_IsPaused(void)
{
	return g_paused.load(std::memory_order_acquire);
}
