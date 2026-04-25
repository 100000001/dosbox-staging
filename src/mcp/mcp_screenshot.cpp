/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Implementation of the latest-frame cache and the `screenshot` MCP
 *  tool. PNG-encodes the cached frame to an in-memory buffer and
 *  returns it as base64.
 *
 *  Both the capture path and the tool handler run on the DOSBox main
 *  thread (the latter via the MCP request queue), so the cache slot
 *  needs no synchronisation.
 */

#include "mcp_screenshot.h"

#include <csetjmp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <png.h>

#include "mcp_third_party.h"
#include "render.h"
#include "video.h"

namespace {

RenderedImage g_latest    = {};
bool          g_has_frame = false;

void clear_latest()
{
	if (g_has_frame) {
		g_latest.free();
		g_latest    = {};
		g_has_frame = false;
	}
}

std::string base64_encode(const uint8_t* data, size_t n)
{
	static const char tbl[] =
	        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	        "abcdefghijklmnopqrstuvwxyz"
	        "0123456789+/";
	std::string out;
	out.reserve(((n + 2) / 3) * 4);
	int val  = 0;
	int valb = -6;
	for (size_t i = 0; i < n; ++i) {
		val = (val << 8) | data[i];
		valb += 8;
		while (valb >= 0) {
			out.push_back(tbl[(val >> valb) & 0x3f]);
			valb -= 6;
		}
	}
	if (valb > -6) {
		out.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3f]);
	}
	while (out.size() % 4 != 0) {
		out.push_back('=');
	}
	return out;
}

struct PngBuf {
	std::vector<uint8_t> bytes;
};

void png_write_cb(png_structp png, png_bytep data, png_size_t length)
{
	auto* buf = static_cast<PngBuf*>(png_get_io_ptr(png));
	buf->bytes.insert(buf->bytes.end(), data, data + length);
}

void png_flush_cb(png_structp /*png*/) {}

bool encode_png_rgb(const uint8_t* rgb, int w, int h,
                    std::vector<uint8_t>& out)
{
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
	                                          nullptr, nullptr, nullptr);
	if (!png) {
		return false;
	}
	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_write_struct(&png, nullptr);
		return false;
	}
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		return false;
	}
	PngBuf buf;
	png_set_write_fn(png, &buf, png_write_cb, png_flush_cb);
	png_set_IHDR(png, info, static_cast<png_uint_32>(w),
	             static_cast<png_uint_32>(h), 8, PNG_COLOR_TYPE_RGB,
	             PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
	             PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	for (int y = 0; y < h; ++y) {
		png_write_row(png,
		              const_cast<uint8_t*>(rgb + static_cast<size_t>(y)
		                                         * static_cast<size_t>(w) * 3));
	}
	png_write_end(png, nullptr);
	png_destroy_write_struct(&png, &info);
	out = std::move(buf.bytes);
	return true;
}

void convert_row(const RenderedImage& img, int w, const uint8_t* src,
                 uint8_t* dst)
{
	switch (img.params.pixel_format) {
	case PixelFormat::Indexed8: {
		// palette_data layout: R0,G0,B0,_,R1,G1,B1,_,...
		const uint8_t* pal = img.palette_data;
		for (int x = 0; x < w; ++x) {
			const uint8_t  idx = src[x];
			const uint8_t* p   = pal + idx * 4;
			dst[3 * x + 0]     = p[0];
			dst[3 * x + 1]     = p[1];
			dst[3 * x + 2]     = p[2];
		}
		break;
	}
	case PixelFormat::RGB555_Packed16: {
		const auto* px = reinterpret_cast<const uint16_t*>(src);
		for (int x = 0; x < w; ++x) {
			const uint16_t v  = px[x];
			const uint8_t  r5 = (v >> 10) & 0x1f;
			const uint8_t  g5 = (v >> 5) & 0x1f;
			const uint8_t  b5 = v & 0x1f;
			dst[3 * x + 0]    = (r5 << 3) | (r5 >> 2);
			dst[3 * x + 1]    = (g5 << 3) | (g5 >> 2);
			dst[3 * x + 2]    = (b5 << 3) | (b5 >> 2);
		}
		break;
	}
	case PixelFormat::RGB565_Packed16: {
		const auto* px = reinterpret_cast<const uint16_t*>(src);
		for (int x = 0; x < w; ++x) {
			const uint16_t v  = px[x];
			const uint8_t  r5 = (v >> 11) & 0x1f;
			const uint8_t  g6 = (v >> 5) & 0x3f;
			const uint8_t  b5 = v & 0x1f;
			dst[3 * x + 0]    = (r5 << 3) | (r5 >> 2);
			dst[3 * x + 1]    = (g6 << 2) | (g6 >> 4);
			dst[3 * x + 2]    = (b5 << 3) | (b5 >> 2);
		}
		break;
	}
	case PixelFormat::BGR24_ByteArray: {
		for (int x = 0; x < w; ++x) {
			dst[3 * x + 0] = src[3 * x + 2];
			dst[3 * x + 1] = src[3 * x + 1];
			dst[3 * x + 2] = src[3 * x + 0];
		}
		break;
	}
	case PixelFormat::BGRX32_ByteArray: {
		for (int x = 0; x < w; ++x) {
			dst[3 * x + 0] = src[4 * x + 2];
			dst[3 * x + 1] = src[4 * x + 1];
			dst[3 * x + 2] = src[4 * x + 0];
		}
		break;
	}
	}
}

bool frame_to_rgb(const RenderedImage& img, int& w, int& h,
                  std::vector<uint8_t>& out)
{
	w = img.params.width;
	h = img.params.height;
	if (w <= 0 || h <= 0 || !img.image_data) {
		return false;
	}
	if (img.params.pixel_format == PixelFormat::Indexed8 && !img.palette_data) {
		return false;
	}
	out.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
	for (int y = 0; y < h; ++y) {
		const int      src_y = img.is_flipped_vertically ? (h - 1 - y) : y;
		const uint8_t* src   = img.image_data
		                     + static_cast<size_t>(src_y) * img.pitch;
		uint8_t* dst = out.data() + static_cast<size_t>(y) * w * 3;
		convert_row(img, w, src, dst);
	}
	return true;
}

} // namespace

void MCP_CaptureLatestFrame(const RenderedImage& image)
{
	clear_latest();
	g_latest    = image.deep_copy();
	g_has_frame = true;
}

nlohmann::json tool_screenshot_main_thread(const nlohmann::json& /*args*/)
{
	if (!g_has_frame) {
		throw std::runtime_error(
		        "no frame captured yet — emulator hasn't drawn anything");
	}
	int                  w = 0;
	int                  h = 0;
	std::vector<uint8_t> rgb;
	if (!frame_to_rgb(g_latest, w, h, rgb)) {
		throw std::runtime_error("failed to convert frame to RGB");
	}
	std::vector<uint8_t> png;
	if (!encode_png_rgb(rgb.data(), w, h, png)) {
		throw std::runtime_error("PNG encode failed");
	}
	return {{"ok", true},
	        {"width", w},
	        {"height", h},
	        {"format", "png"},
	        {"image_b64", base64_encode(png.data(), png.size())}};
}
