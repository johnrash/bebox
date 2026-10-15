/*
 * BeBox emulator - minimal PNG writer.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "PngWriter.h"

#include <cstdio>
#include <vector>

namespace bebox {

static u32
Crc32(const u8* data, size_t length, u32 crc = 0xffffffff)
{
	static u32 table[256];
	static bool initialized = false;
	if (!initialized) {
		for (u32 n = 0; n < 256; n++) {
			u32 c = n;
			for (int k = 0; k < 8; k++)
				c = (c & 1) != 0 ? 0xedb88320 ^ (c >> 1) : c >> 1;
			table[n] = c;
		}
		initialized = true;
	}
	for (size_t i = 0; i < length; i++)
		crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
	return crc;
}


static void
Put32(std::vector<u8>& out, u32 value)
{
	out.push_back(value >> 24);
	out.push_back(value >> 16);
	out.push_back(value >> 8);
	out.push_back(value);
}


static void
Chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data)
{
	Put32(out, (u32)data.size());
	size_t start = out.size();
	out.insert(out.end(), type, type + 4);
	out.insert(out.end(), data.begin(), data.end());
	u32 crc = Crc32(&out[start], out.size() - start) ^ 0xffffffff;
	Put32(out, crc);
}


bool
WritePng(const std::string& path, const u32* pixels, int width, int height,
	int stride)
{
	std::vector<u8> raw;
	raw.reserve((size_t)(width * 3 + 1) * height);
	for (int y = 0; y < height; y++) {
		raw.push_back(0);	// filter: none
		for (int x = 0; x < width; x++) {
			u32 p = pixels[(size_t)y * stride + x];
			raw.push_back(p >> 16);
			raw.push_back(p >> 8);
			raw.push_back(p);
		}
	}
	// zlib stream with stored deflate blocks
	std::vector<u8> z;
	z.push_back(0x78);
	z.push_back(0x01);
	size_t position = 0;
	while (position < raw.size() || raw.empty()) {
		size_t length = raw.size() - position;
		if (length > 65535)
			length = 65535;
		bool last = position + length >= raw.size();
		z.push_back(last ? 1 : 0);
		z.push_back(length & 0xff);
		z.push_back(length >> 8);
		z.push_back(~length & 0xff);
		z.push_back((~length >> 8) & 0xff);
		z.insert(z.end(), raw.begin() + position,
			raw.begin() + position + length);
		position += length;
		if (raw.empty())
			break;
	}
	u32 a = 1, b = 0;
	for (u8 byte : raw) {
		a = (a + byte) % 65521;
		b = (b + a) % 65521;
	}
	Put32(z, (b << 16) | a);

	std::vector<u8> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	std::vector<u8> header;
	Put32(header, width);
	Put32(header, height);
	header.push_back(8);	// bit depth
	header.push_back(2);	// truecolor
	header.push_back(0);
	header.push_back(0);
	header.push_back(0);
	Chunk(png, "IHDR", header);
	Chunk(png, "IDAT", z);
	Chunk(png, "IEND", std::vector<u8>());

	FILE* file = fopen(path.c_str(), "wb");
	if (file == nullptr)
		return false;
	bool ok = fwrite(png.data(), 1, png.size(), file) == png.size();
	return fclose(file) == 0 && ok;
}

}	// namespace bebox
