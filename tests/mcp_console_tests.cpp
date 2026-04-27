/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Unit tests for the MCP teletype tap's ring buffer math.
 *
 *  The tap's CP437 -> UTF-8 conversion is provided by `dos_to_utf8` in
 *  unicode.cpp; pulling that into a unit-test build would drag in the
 *  whole DOSBox codepage subsystem. We stub it as identity for the
 *  test, which is faithful for the ASCII inputs the tests use.
 */

#include "mcp_console.h"

#include <gtest/gtest.h>

#include <string>

#include "unicode.h"

// --- stub for dos_to_utf8 -----------------------------------------
// mcp_console.cpp calls `dos_to_utf8(raw, WithControlCodes)`.
// Identity is correct for the ASCII bytes we feed in the tests.
std::string dos_to_utf8(const std::string& s, const DosStringConvertMode)
{
	return s;
}
std::string dos_to_utf8(const std::string& s, const DosStringConvertMode,
                        const uint16_t)
{
	return s;
}
// Unused but referenced by unicode.h's full surface — provide stubs so
// the linker is happy even if mcp_console.cpp grows new calls later.
std::string utf8_to_dos(const std::string& s, const DosStringConvertMode,
                        const UnicodeFallback)
{
	return s;
}
std::string utf8_to_dos(const std::string& s, const DosStringConvertMode,
                        const UnicodeFallback, const uint16_t)
{
	return s;
}
std::string lowercase_dos(const std::string& s)
{
	return s;
}
std::string lowercase_dos(const std::string& s, const uint16_t)
{
	return s;
}
std::string uppercase_dos(const std::string& s)
{
	return s;
}
std::string uppercase_dos(const std::string& s, const uint16_t)
{
	return s;
}
uint16_t get_utf8_code_page()
{
	return 437;
}

namespace {

void feed(const std::string& s)
{
	for (auto c : s) {
		MCP_ConsoleTap_Notify(static_cast<uint8_t>(c));
	}
}

class TapTest : public ::testing::Test {
protected:
	void TearDown() override
	{
		// Reset tap state between tests so they don't leak.
		MCP_ConsoleTap_Disable();
	}
};

} // namespace

// Pre-init / post-Disable: Notify is a no-op, ReadSince returns empty,
// and the modulo-by-zero path inside the slice math is not reached.
TEST_F(TapTest, EmptyBufferReturnsEmpty)
{
	MCP_ConsoleTap_Notify('x'); // dropped — tap not enabled
	const auto mark = MCP_ConsoleTap_Mark();
	EXPECT_EQ(mark, 0u);
	bool truncated = true;
	const auto out = MCP_ConsoleTap_ReadSince(0, truncated);
	EXPECT_TRUE(out.empty());
	EXPECT_FALSE(truncated);
}

TEST_F(TapTest, NoWrap)
{
	MCP_ConsoleTap_SetCapacity(64);
	const auto mark = MCP_ConsoleTap_Mark();
	feed("hello");
	bool truncated = true;
	EXPECT_EQ(MCP_ConsoleTap_ReadSince(mark, truncated), "hello");
	EXPECT_FALSE(truncated);
}

TEST_F(TapTest, ExactCapacity)
{
	MCP_ConsoleTap_SetCapacity(16); // SetCapacity floor is 16 KB
	const auto mark = MCP_ConsoleTap_Mark();
	// SetCapacity is clamped to >=16384, so write exactly that many bytes
	// to hit the no-wrap-but-full corner.
	std::string payload(16 * 1024, 'A');
	feed(payload);
	bool truncated = true;
	EXPECT_EQ(MCP_ConsoleTap_ReadSince(mark, truncated), payload);
	EXPECT_FALSE(truncated);
}

TEST_F(TapTest, WrappedTailReturnsLastNBytes)
{
	MCP_ConsoleTap_SetCapacity(16); // → 16 KB ring
	const auto mark = MCP_ConsoleTap_Mark();
	std::string payload;
	const size_t total = 20 * 1024; // 4 KB more than capacity
	payload.reserve(total);
	for (size_t i = 0; i < total; ++i) {
		payload.push_back(static_cast<char>('A' + (i % 26)));
	}
	feed(payload);
	bool truncated = false;
	const auto out = MCP_ConsoleTap_ReadSince(mark, truncated);
	EXPECT_TRUE(truncated);
	EXPECT_EQ(out.size(), 16u * 1024u);
	// The returned slice should be the *tail* — last 16 KB of payload.
	EXPECT_EQ(out, payload.substr(total - 16 * 1024));
}

TEST_F(TapTest, FutureCursorYieldsEmpty)
{
	MCP_ConsoleTap_SetCapacity(16);
	feed("abc");
	const auto far_future = MCP_ConsoleTap_Mark() + 1'000'000;
	bool truncated = true;
	const auto out = MCP_ConsoleTap_ReadSince(far_future, truncated);
	EXPECT_TRUE(out.empty());
	EXPECT_FALSE(truncated);
}

TEST_F(TapTest, MarkClampsAroundDisable)
{
	MCP_ConsoleTap_SetCapacity(16);
	feed("first");
	MCP_ConsoleTap_Disable();
	// After Disable, the buffer is empty and total_written reset.
	EXPECT_EQ(MCP_ConsoleTap_Mark(), 0u);
	bool truncated = true;
	const auto out = MCP_ConsoleTap_ReadSince(0, truncated);
	EXPECT_TRUE(out.empty());
	EXPECT_FALSE(truncated);

	// Re-enabling resumes capture.
	MCP_ConsoleTap_SetCapacity(16);
	const auto mark = MCP_ConsoleTap_Mark();
	feed("second");
	bool t2 = true;
	EXPECT_EQ(MCP_ConsoleTap_ReadSince(mark, t2), "second");
	EXPECT_FALSE(t2);
}

TEST_F(TapTest, OldCursorBeforeWindowFlagsTruncated)
{
	MCP_ConsoleTap_SetCapacity(16);
	const auto mark = MCP_ConsoleTap_Mark();
	// Write much more than capacity so the original `mark` is older
	// than the oldest retained byte.
	std::string payload(64 * 1024, 'B');
	feed(payload);
	bool truncated = false;
	const auto out = MCP_ConsoleTap_ReadSince(mark, truncated);
	EXPECT_TRUE(truncated);
	EXPECT_EQ(out.size(), 16u * 1024u);
}
