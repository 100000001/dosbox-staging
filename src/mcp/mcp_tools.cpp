/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Tool implementations. Every tool_*_main_thread function is invoked
 *  on the DOSBox main thread by the queue pump in mcp_server.cpp, so
 *  it may freely touch CPU/memory/keyboard state.
 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "paging.h" // mem_readb<MemOpMode>(addr) template lives here

#include "mcp_third_party.h"
#include "mcp_queue.h"

#include "bios.h"
#include "cpu.h"
#include "../ints/int10.h"
#include "keyboard.h"
#include "mem.h"
#include "render.h"

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

// Map of key name → (KBD_KEYS scancode, BIOS ascii, needs_shift). Used by
// both BIOS-buffer injection (legacy path) and the hardware-emulation
// path via KEYBOARD_AddKey.
struct KeyEntry {
	KBD_KEYS kbd;
	uint8_t  bios_scancode;
	uint8_t  ascii;
	bool     shift;
};

const std::unordered_map<std::string, KeyEntry>& key_table()
{
	static const std::unordered_map<std::string, KeyEntry> t = {
	        {"enter",     {KBD_enter,     0x1c, 0x0d, false}},
	        {"return",    {KBD_enter,     0x1c, 0x0d, false}},
	        {"esc",       {KBD_esc,       0x01, 0x1b, false}},
	        {"escape",    {KBD_esc,       0x01, 0x1b, false}},
	        {"space",     {KBD_space,     0x39, 0x20, false}},
	        {"tab",       {KBD_tab,       0x0f, 0x09, false}},
	        {"backspace", {KBD_backspace, 0x0e, 0x08, false}},
	        {"up",        {KBD_up,        0x48, 0x00, false}},
	        {"down",      {KBD_down,      0x50, 0x00, false}},
	        {"left",      {KBD_left,      0x4b, 0x00, false}},
	        {"right",     {KBD_right,     0x4d, 0x00, false}},
	        {"home",      {KBD_home,      0x47, 0x00, false}},
	        {"end",       {KBD_end,       0x4f, 0x00, false}},
	        {"pageup",    {KBD_pageup,    0x49, 0x00, false}},
	        {"pagedown",  {KBD_pagedown,  0x51, 0x00, false}},
	        {"insert",    {KBD_insert,    0x52, 0x00, false}},
	        {"delete",    {KBD_delete,    0x53, 0x00, false}},
	        {"f1",  {KBD_f1,  0x3b, 0x00, false}}, {"f2",  {KBD_f2,  0x3c, 0x00, false}},
	        {"f3",  {KBD_f3,  0x3d, 0x00, false}}, {"f4",  {KBD_f4,  0x3e, 0x00, false}},
	        {"f5",  {KBD_f5,  0x3f, 0x00, false}}, {"f6",  {KBD_f6,  0x40, 0x00, false}},
	        {"f7",  {KBD_f7,  0x41, 0x00, false}}, {"f8",  {KBD_f8,  0x42, 0x00, false}},
	        {"f9",  {KBD_f9,  0x43, 0x00, false}}, {"f10", {KBD_f10, 0x44, 0x00, false}},
	        {"f11", {KBD_f11, 0x57, 0x00, false}}, {"f12", {KBD_f12, 0x58, 0x00, false}},
	};
	return t;
}

// Map an ASCII char to a KBD key, with shift flag for uppercase / shifted
// symbols. Returns nullopt for chars we don't have a hardware mapping for
// (caller can fall back to BIOS-buffer injection).
struct AsciiKey {
	KBD_KEYS kbd;
	bool     shift;
	uint8_t  bios_scancode;
};

std::optional<AsciiKey> ascii_to_kbd(char c)
{
	auto digit = [](KBD_KEYS k, uint8_t sc) {
		return AsciiKey{k, false, sc};
	};
	auto letter = [](KBD_KEYS k, uint8_t sc, bool sh) {
		return AsciiKey{k, sh, sc};
	};
	switch (c) {
	case '\r': case '\n': return AsciiKey{KBD_enter,     false, 0x1c};
	case ' ':  return AsciiKey{KBD_space,     false, 0x39};
	case '\t': return AsciiKey{KBD_tab,       false, 0x0f};
	case '\b': return AsciiKey{KBD_backspace, false, 0x0e};
	case '0':  return digit(KBD_0, 0x0b);
	case '1':  return digit(KBD_1, 0x02);
	case '2':  return digit(KBD_2, 0x03);
	case '3':  return digit(KBD_3, 0x04);
	case '4':  return digit(KBD_4, 0x05);
	case '5':  return digit(KBD_5, 0x06);
	case '6':  return digit(KBD_6, 0x07);
	case '7':  return digit(KBD_7, 0x08);
	case '8':  return digit(KBD_8, 0x09);
	case '9':  return digit(KBD_9, 0x0a);
	}
	if (c >= 'a' && c <= 'z') {
		static const KBD_KEYS letters[] = {
		    KBD_a, KBD_b, KBD_c, KBD_d, KBD_e, KBD_f, KBD_g, KBD_h,
		    KBD_i, KBD_j, KBD_k, KBD_l, KBD_m, KBD_n, KBD_o, KBD_p,
		    KBD_q, KBD_r, KBD_s, KBD_t, KBD_u, KBD_v, KBD_w, KBD_x,
		    KBD_y, KBD_z,
		};
		static const uint8_t scancodes[] = {
		    0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23,
		    0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
		    0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d,
		    0x15, 0x2c,
		};
		const int idx = c - 'a';
		return letter(letters[idx], scancodes[idx], false);
	}
	if (c >= 'A' && c <= 'Z') {
		auto k = ascii_to_kbd(static_cast<char>(c - 'A' + 'a'));
		if (k) k->shift = true;
		return k;
	}
	return std::nullopt;
}

void press_kbd_key(KBD_KEYS k, bool shift)
{
	if (shift) KEYBOARD_AddKey(KBD_leftshift, true);
	KEYBOARD_AddKey(k, true);
	KEYBOARD_AddKey(k, false);
	if (shift) KEYBOARD_AddKey(KBD_leftshift, false);
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
		return (static_cast<uint16_t>(it->second.bios_scancode) << 8)
		       | it->second.ascii;
	}
	if (key.size() == 1) {
		return static_cast<uint16_t>(static_cast<unsigned char>(key[0]));
	}
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
	nlohmann::json out = {{"ok", true},
	                      {"paused", mcp_bridge::paused_flag().load()},
	                      {"server", "dosbox-mcp"},
	                      {"version", "0.1.0"},
	                      {"fps", render.fps}};

	// CurMode is set by INT 10h; on a freshly booted machine it points
	// at a valid VGA mode entry. Report the BIOS mode number plus the
	// pixel dimensions so callers can sanity-check resolution before
	// asking for a screenshot.
	if (CurMode != ModeList_VGA.end()) {
		out["video_mode"]    = CurMode->mode;
		out["screen_width"]  = CurMode->swidth;
		out["screen_height"] = CurMode->sheight;
	}
	return out;
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

nlohmann::json tool_memory_search_main_thread(const nlohmann::json& args)
{
	if (!args.contains("hex")) {
		throw std::invalid_argument("missing 'hex'");
	}
	std::string needle;
	if (!hex_decode(args["hex"].get<std::string>(), needle)) {
		throw std::invalid_argument("'hex' is not a valid hex string");
	}
	if (needle.empty() || needle.size() > 256) {
		throw std::invalid_argument(
		        "needle length must be 1..256 bytes");
	}

	std::string mask(needle.size(), static_cast<char>(0xff));
	if (args.contains("mask_hex") && !args["mask_hex"].is_null()) {
		std::string m;
		if (!hex_decode(args["mask_hex"].get<std::string>(), m)) {
			throw std::invalid_argument(
			        "'mask_hex' is not a valid hex string");
		}
		if (m.size() != needle.size()) {
			throw std::invalid_argument(
			        "'mask_hex' length must match 'hex' length");
		}
		mask = std::move(m);
	}

	// Pre-mask the needle so the inner loop is a single AND+compare per
	// byte instead of two ANDs.
	for (size_t i = 0; i < needle.size(); ++i) {
		needle[i] = static_cast<char>(needle[i] & mask[i]);
	}

	const uint64_t ram_end = static_cast<uint64_t>(MEM_TotalPages())
	                         * static_cast<uint64_t>(MemPageSize);

	uint64_t start = args.value("start", uint64_t{0});
	uint64_t end   = args.value("end", ram_end);
	if (start > ram_end) start = ram_end;
	if (end   > ram_end) end   = ram_end;
	if (end < start)     end   = start;

	const uint32_t max_results = args.value(
	        "max_results", uint32_t{1024});

	std::vector<uint32_t> hits;
	hits.reserve(std::min<uint32_t>(max_results, 4096));
	bool truncated   = false;
	uint64_t scanned = 0;

	const size_t n = needle.size();
	if (end >= start + n) {
		const uint64_t last = end - n;
		for (uint64_t addr = start; addr <= last; ++addr) {
			bool match = true;
			for (size_t i = 0; i < n; ++i) {
				const uint8_t b = mem_readb<MemOpMode::SkipBreakpoints>(
				        static_cast<PhysPt>(addr + i));
				if ((b & static_cast<uint8_t>(mask[i]))
				    != static_cast<uint8_t>(needle[i])) {
					match = false;
					break;
				}
			}
			if (match) {
				hits.push_back(static_cast<uint32_t>(addr));
				if (hits.size() >= max_results) {
					truncated = true;
					scanned   = (addr + 1) - start;
					break;
				}
			}
		}
		if (!truncated) {
			scanned = (last + 1) - start;
		}
	}

	return {{"ok", true},
	        {"count", hits.size()},
	        {"addresses", hits},
	        {"truncated", truncated},
	        {"scanned", scanned}};
}

nlohmann::json tool_send_key_main_thread(const nlohmann::json& args)
{
	if (!args.contains("key")) {
		throw std::invalid_argument("missing 'key'");
	}
	const auto key = args["key"].get<std::string>();
	std::string lower;
	lower.reserve(key.size());
	for (char c : key) {
		lower.push_back(static_cast<char>(std::tolower(
		        static_cast<unsigned char>(c))));
	}

	const auto& tbl = key_table();
	const auto it   = tbl.find(lower);
	if (it != tbl.end()) {
		// Hardware-emulation path: simulate a real key press at the
		// keyboard controller. Reaches games that read scancodes
		// directly via port 0x60 (most graphics-mode DOS games do
		// this); the BIOS keyboard buffer also gets populated as a
		// side effect of the keyboard ISR.
		press_kbd_key(it->second.kbd, it->second.shift);
		return {{"ok", true}, {"key", lower}};
	}
	if (key.size() == 1) {
		auto ak = ascii_to_kbd(key[0]);
		if (ak) {
			press_kbd_key(ak->kbd, ak->shift);
			return {{"ok", true}, {"key", key}};
		}
	}
	// Unknown name: fall back to the legacy BIOS-buffer injection so
	// numeric scancode|ascii values still work.
	uint16_t code = key_name_to_code(key);
	bool ok       = BIOS_AddKeyToBuffer(code);
	return {{"ok", ok}, {"code", static_cast<uint32_t>(code)},
	        {"via", "bios"}};
}

nlohmann::json tool_set_speed_main_thread(const nlohmann::json& args)
{
	if (!args.contains("multiplier")) {
		throw std::invalid_argument("missing 'multiplier'");
	}
	const double mult = args["multiplier"].get<double>();
	if (!(mult >= 0.1 && mult <= 100.0)) {
		throw std::invalid_argument("'multiplier' must be in [0.1, 100]");
	}

	// Captured lazily on the first call so the baseline reflects whatever
	// the user (or auto-adjust) settled on by the time the agent first
	// touches speed. After that, multiplier=1.0 always restores that
	// reference rate. Lives on the main thread, so no synchronisation.
	static int baseline_cycles = 0;
	if (baseline_cycles == 0) {
		baseline_cycles = CPU_CycleMax > 0 ? CPU_CycleMax : 3000;
	}

	const int prev_cycles  = CPU_CycleMax;
	const bool prev_auto   = CPU_CycleAutoAdjust;
	const long requested   = std::lround(
	        static_cast<double>(baseline_cycles) * mult);
	const int new_cycles = static_cast<int>(std::max<long>(1, requested));

	CPU_CycleAutoAdjust = false;
	CPU_CycleMax        = new_cycles;

	return {{"ok", true},
	        {"multiplier", mult},
	        {"baseline_cycles", baseline_cycles},
	        {"prev_cycles", prev_cycles},
	        {"current_cycles", new_cycles},
	        {"prev_auto_adjust", prev_auto}};
}

nlohmann::json tool_send_keys_main_thread(const nlohmann::json& args)
{
	if (!args.contains("text")) {
		throw std::invalid_argument("missing 'text'");
	}
	const auto text = args["text"].get<std::string>();
	uint32_t accepted = 0;
	for (char c : text) {
		auto ak = ascii_to_kbd(c);
		if (ak) {
			press_kbd_key(ak->kbd, ak->shift);
			++accepted;
			continue;
		}
		// Fallback: BIOS-buffer for chars without a hardware mapping.
		uint16_t code = static_cast<uint16_t>(
		        static_cast<unsigned char>(c));
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
