/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Tool implementations. Every tool_*_main_thread function is invoked
 *  on the DOSBox main thread by the queue pump in mcp_server.cpp, so
 *  it may freely touch CPU/memory/keyboard state.
 */

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "mcp_third_party.h"
#include "mcp_queue.h"

#include "bios.h"
#include "mem.h"

namespace mcp_bridge {
std::atomic<bool>& paused_flag();
RequestQueue&      queue();
} // namespace mcp_bridge

namespace {

bool hex_decode(const std::string& s, std::string& out)
{
	if (s.size() % 2 != 0) {
		return false;
	}
	out.clear();
	out.reserve(s.size() / 2);
	for (size_t i = 0; i < s.size(); i += 2) {
		auto nybble = [](char c, int& v) -> bool {
			if (c >= '0' && c <= '9') {
				v = c - '0';
				return true;
			}
			if (c >= 'a' && c <= 'f') {
				v = 10 + (c - 'a');
				return true;
			}
			if (c >= 'A' && c <= 'F') {
				v = 10 + (c - 'A');
				return true;
			}
			return false;
		};
		int hi = 0, lo = 0;
		if (!nybble(s[i], hi) || !nybble(s[i + 1], lo)) {
			return false;
		}
		out.push_back(static_cast<char>((hi << 4) | lo));
	}
	return true;
}

std::string hex_encode(const uint8_t* data, size_t n)
{
	static const char* digits = "0123456789abcdef";
	std::string out;
	out.resize(n * 2);
	for (size_t i = 0; i < n; ++i) {
		out[2 * i]     = digits[(data[i] >> 4) & 0xf];
		out[2 * i + 1] = digits[data[i] & 0xf];
	}
	return out;
}

uint32_t resolve_address(const nlohmann::json& args)
{
	if (args.contains("address") && !args["address"].is_null()) {
		return args["address"].get<uint32_t>();
	}
	if (args.contains("segment") && args.contains("offset")) {
		uint32_t seg = args["segment"].get<uint32_t>() & 0xffff;
		uint32_t off = args["offset"].get<uint32_t>() & 0xffff;
		return (seg << 4) + off;
	}
	throw std::invalid_argument(
	        "must provide either 'address' or both 'segment' and 'offset'");
}

// Minimal key-name → BIOS (scancode<<8 | ascii) map. Extend as needed.
struct KeyEntry {
	uint8_t scancode;
	uint8_t ascii;
};
const std::unordered_map<std::string, KeyEntry>& key_table()
{
	static const std::unordered_map<std::string, KeyEntry> t = {
	        {"enter", {0x1c, 0x0d}},   {"return", {0x1c, 0x0d}},
	        {"esc", {0x01, 0x1b}},     {"escape", {0x01, 0x1b}},
	        {"space", {0x39, 0x20}},   {"tab", {0x0f, 0x09}},
	        {"backspace", {0x0e, 0x08}},
	        {"up", {0x48, 0x00}},      {"down", {0x50, 0x00}},
	        {"left", {0x4b, 0x00}},    {"right", {0x4d, 0x00}},
	        {"home", {0x47, 0x00}},    {"end", {0x4f, 0x00}},
	        {"pageup", {0x49, 0x00}},  {"pagedown", {0x51, 0x00}},
	        {"insert", {0x52, 0x00}},  {"delete", {0x53, 0x00}},
	        {"f1", {0x3b, 0x00}},      {"f2", {0x3c, 0x00}},
	        {"f3", {0x3d, 0x00}},      {"f4", {0x3e, 0x00}},
	        {"f5", {0x3f, 0x00}},      {"f6", {0x40, 0x00}},
	        {"f7", {0x41, 0x00}},      {"f8", {0x42, 0x00}},
	        {"f9", {0x43, 0x00}},      {"f10", {0x44, 0x00}},
	        {"f11", {0x57, 0x00}},     {"f12", {0x58, 0x00}},
	};
	return t;
}

uint16_t key_name_to_code(const std::string& key)
{
	std::string lower;
	lower.reserve(key.size());
	for (char c : key) {
		lower.push_back(static_cast<char>(std::tolower(
		        static_cast<unsigned char>(c))));
	}
	const auto& tbl = key_table();
	auto it         = tbl.find(lower);
	if (it != tbl.end()) {
		return (static_cast<uint16_t>(it->second.scancode) << 8)
		       | it->second.ascii;
	}
	if (key.size() == 1) {
		// Single ASCII char. Scancode of 0 is good enough for INT 16h
		// AH=0 callers; games that look at scancode may need more but
		// Civ I doesn't.
		return static_cast<uint16_t>(static_cast<unsigned char>(key[0]));
	}
	// Numeric scancode|ascii passed as decimal or hex.
	try {
		size_t pos = 0;
		auto v     = static_cast<unsigned long>(
                        std::stoul(key, &pos, 0));
		if (pos != key.size() || v > 0xffff) {
			throw std::invalid_argument("bad numeric");
		}
		return static_cast<uint16_t>(v);
	} catch (...) {
		throw std::invalid_argument("unknown key: " + key);
	}
}

} // namespace

// ===== Tool implementations (main thread only) =====

nlohmann::json tool_pause_main_thread(const nlohmann::json& /*args*/)
{
	bool was = mcp_bridge::paused_flag().exchange(true);
	return {{"ok", true}, {"was_running", !was}};
}

nlohmann::json tool_resume_main_thread(const nlohmann::json& /*args*/)
{
	bool was = mcp_bridge::paused_flag().exchange(false);
	return {{"ok", true}, {"was_paused", was}};
}

nlohmann::json tool_get_status_main_thread(const nlohmann::json& /*args*/)
{
	return {{"ok", true},
	        {"paused", mcp_bridge::paused_flag().load()},
	        {"server", "dosbox-mcp"},
	        {"version", "0.1.0"}};
}

nlohmann::json tool_mem_read_main_thread(const nlohmann::json& args)
{
	if (!args.contains("length")) {
		throw std::invalid_argument("missing 'length'");
	}
	const auto length = args["length"].get<uint32_t>();
	if (length == 0 || length > 65536) {
		throw std::invalid_argument("'length' must be 1..65536");
	}
	const PhysPt addr = resolve_address(args);
	std::string bytes;
	bytes.resize(length);
	for (uint32_t i = 0; i < length; ++i) {
		bytes[i] = static_cast<char>(mem_readb(addr + i));
	}
	return {{"ok", true},
	        {"address", static_cast<uint32_t>(addr)},
	        {"length", length},
	        {"hex", hex_encode(reinterpret_cast<const uint8_t*>(bytes.data()),
	                           bytes.size())}};
}

nlohmann::json tool_mem_write_main_thread(const nlohmann::json& args)
{
	if (!args.contains("hex")) {
		throw std::invalid_argument("missing 'hex'");
	}
	std::string raw;
	if (!hex_decode(args["hex"].get<std::string>(), raw)) {
		throw std::invalid_argument("'hex' is not a valid hex string");
	}
	const PhysPt addr = resolve_address(args);
	for (size_t i = 0; i < raw.size(); ++i) {
		mem_writeb(addr + static_cast<uint32_t>(i),
		           static_cast<uint8_t>(raw[i]));
	}
	return {{"ok", true},
	        {"address", static_cast<uint32_t>(addr)},
	        {"written", raw.size()}};
}

nlohmann::json tool_send_key_main_thread(const nlohmann::json& args)
{
	if (!args.contains("key")) {
		throw std::invalid_argument("missing 'key'");
	}
	uint16_t code = key_name_to_code(args["key"].get<std::string>());
	bool ok       = BIOS_AddKeyToBuffer(code);
	return {{"ok", ok},
	        {"code", static_cast<uint32_t>(code)}};
}

nlohmann::json tool_send_keys_main_thread(const nlohmann::json& args)
{
	if (!args.contains("text")) {
		throw std::invalid_argument("missing 'text'");
	}
	const auto text = args["text"].get<std::string>();
	uint32_t accepted = 0;
	for (char c : text) {
		uint16_t code = static_cast<uint16_t>(
		        static_cast<unsigned char>(c));
		// Translate \n into Enter (Civ menus expect 0x1c0d, not 0x000a).
		if (c == '\n' || c == '\r') {
			code = (0x1c << 8) | 0x0d;
		}
		if (BIOS_AddKeyToBuffer(code)) {
			++accepted;
		} else {
			break; // buffer full
		}
	}
	return {{"ok", accepted == text.size()},
	        {"count", accepted},
	        {"requested", text.size()}};
}
