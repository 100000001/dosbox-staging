/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Implementation of the teletype tap. Single-writer (DOSBox main
 *  thread, via MCP_ConsoleTap_Notify) / single-reader (also main
 *  thread, via the MCP request queue) in practice, but the lock makes
 *  the contract robust if a future tool reads from a worker.
 */

#include "mcp_console.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "unicode.h"

namespace {

constexpr size_t MinCapacity     = 16 * 1024;
constexpr size_t MaxCapacity     = 4 * 1024 * 1024;

// Off until SetCapacity is called from MCP_Init. The atomic lets us
// short-circuit Notify on the hot path (every emulated teletype byte)
// without touching the mutex when MCP is config-disabled or shut down.
std::atomic<bool>     g_enabled{false};
std::mutex            g_mu;
std::vector<uint8_t>  g_buf;
uint64_t              g_total_written = 0;
size_t                g_head          = 0;

} // namespace

void MCP_ConsoleTap_Notify(uint8_t byte)
{
	if (!g_enabled.load(std::memory_order_relaxed)) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_mu);
	if (g_buf.empty()) {
		return;
	}
	g_buf[g_head] = byte;
	g_head        = (g_head + 1) % g_buf.size();
	++g_total_written;
}

uint64_t MCP_ConsoleTap_Mark(void)
{
	std::lock_guard<std::mutex> lock(g_mu);
	return g_total_written;
}

std::string MCP_ConsoleTap_ReadSince(uint64_t cursor, bool& truncated)
{
	std::string raw;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		const size_t   capacity = g_buf.size();
		const uint64_t total    = g_total_written;
		// Saturating subtraction so we never underflow the unsigned
		// counter when total < capacity (i.e. just after init or
		// SetCapacity).
		const uint64_t oldest = (total > capacity)
		                              ? total - static_cast<uint64_t>(capacity)
		                              : 0;
		if (cursor > total) {
			// Caller passed a mark from a future they haven't reached
			// yet — defensive, shouldn't happen.
			cursor    = total;
			truncated = false;
		} else if (cursor < oldest) {
			cursor    = oldest;
			truncated = true;
		} else {
			truncated = false;
		}

		const size_t available = static_cast<size_t>(total - cursor);
		raw.resize(available);

		// The byte at index `g_head` is the oldest; (g_head - 1) mod
		// capacity is the newest. The byte written `available` ago is
		// at (g_head - available) mod capacity.
		const size_t start = (capacity + g_head - available) % capacity;
		if (start + available <= capacity) {
			std::copy_n(g_buf.begin() + start, available, raw.begin());
		} else {
			const size_t first_chunk = capacity - start;
			std::copy_n(g_buf.begin() + start, first_chunk, raw.begin());
			std::copy_n(g_buf.begin(),
			            available - first_chunk,
			            raw.begin() + first_chunk);
		}
	}
	return dos_to_utf8(raw, DosStringConvertMode::WithControlCodes);
}

void MCP_ConsoleTap_SetCapacity(size_t bytes)
{
	const size_t clamped = std::clamp(bytes, MinCapacity, MaxCapacity);
	{
		std::lock_guard<std::mutex> lock(g_mu);
		g_buf.assign(clamped, 0);
		g_head          = 0;
		g_total_written = 0;
	}
	g_enabled.store(true, std::memory_order_relaxed);
}

void MCP_ConsoleTap_Disable(void)
{
	g_enabled.store(false, std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(g_mu);
	std::vector<uint8_t>().swap(g_buf);
	g_head          = 0;
	g_total_written = 0;
}
