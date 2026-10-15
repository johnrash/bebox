/*
 * QCOW2 tests: random writes through Qcow2Image mirrored to a raw image,
 * checked against each other and, when available, by qemu-img (check,
 * compare) and on images qemu-img made (plain, compressed, overlay).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * usage: qcow2-tests WORKDIR
 */
#include "core/storage/DiskImage.h"
#include "core/storage/Qcow2Image.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using namespace bebox;

namespace {

int sFailures = 0;

void
Fail(const std::string& what)
{
	printf("FAIL: %s\n", what.c_str());
	sFailures++;
}


bool
HaveQemuImg()
{
	return system("qemu-img --version >/dev/null 2>&1") == 0;
}


bool
Run(const std::string& command)
{
	int result = system((command + " >/dev/null 2>&1").c_str());
	return result == 0;
}


bool
SameContents(DiskImage& a, DiskImage& b)
{
	if (a.Size() != b.Size())
		return false;
	std::vector<u8> x(1 << 20), y(1 << 20);
	for (u64 offset = 0; offset < a.Size(); offset += x.size()) {
		size_t length = (size_t)std::min<u64>(x.size(), a.Size() - offset);
		if (!a.Read(offset, x.data(), length)
			|| !b.Read(offset, y.data(), length)
			|| memcmp(x.data(), y.data(), length) != 0)
			return false;
	}
	return true;
}


// Random writes of random sizes (sector aligned and not), mirrored.
void
RandomWrites(DiskImage& image, DiskImage& mirror, int count, u32 seed)
{
	std::mt19937 random(seed);
	std::vector<u8> data;
	for (int i = 0; i < count; i++) {
		u64 size = image.Size();
		u64 length = 1 + random() % (random() % 4 == 0 ? 300000 : 9000);
		u64 offset = random() % (size - length);
		if (random() % 2 == 0) {
			offset &= ~511ULL;
			length = (length + 511) & ~511ULL;
			if (offset + length > size)
				continue;
		}
		data.resize(length);
		u8 pattern = (u8)random();
		for (u64 j = 0; j < length; j++)
			data[j] = (u8)(pattern + j * 7 + (j >> 9));
		if (!image.Write(offset, data.data(), length)
			|| !mirror.Write(offset, data.data(), length)) {
			Fail("write");
			return;
		}
	}
}


void
TestOwnImages(const std::string& dir, bool qemu)
{
	std::string qcowPath = dir + "/test.qcow2";
	std::string rawPath = dir + "/test.raw";
	unlink(qcowPath.c_str());
	unlink(rawPath.c_str());
	std::string error;
	const u64 kSize = 200ULL << 20;
	if (!Qcow2Image::Create(qcowPath, kSize, error)
		|| !RawImage::Create(rawPath, kSize, error)) {
		Fail("create: " + error);
		return;
	}
	{
		std::unique_ptr<DiskImage> qcow = DiskImage::Open(qcowPath, false,
			error);
		std::unique_ptr<DiskImage> raw = DiskImage::Open(rawPath, false,
			error);
		if (!qcow || !raw || strcmp(qcow->Format(), "qcow2") != 0) {
			Fail("open: " + error);
			return;
		}
		RandomWrites(*qcow, *raw, 2000, 1);
		if (!SameContents(*qcow, *raw))
			Fail("own image: contents differ from the raw mirror");
		std::string report;
		int problems = static_cast<Qcow2Image*>(qcow.get())->Check(report);
		if (problems != 0)
			Fail("own image: check: " + report);
	}
	// reopened
	{
		std::unique_ptr<DiskImage> qcow = DiskImage::Open(qcowPath, true,
			error);
		std::unique_ptr<DiskImage> raw = DiskImage::Open(rawPath, true,
			error);
		if (!qcow || !SameContents(*qcow, *raw))
			Fail("own image: differs after reopening");
	}
	if (qemu) {
		if (!Run("qemu-img check " + qcowPath))
			Fail("qemu-img check of our image");
		if (!Run("qemu-img compare -f qcow2 -F raw " + qcowPath + " "
				+ rawPath))
			Fail("qemu-img compare of our image");
	}
	printf("ok: own qcow2 image, 2000 random writes\n");

	// 512 byte clusters: the reference count table has to grow
	std::string small = dir + "/small.qcow2";
	std::string smallRaw = dir + "/small.raw";
	unlink(small.c_str());
	unlink(smallRaw.c_str());
	if (!Qcow2Image::Create(small, 40ULL << 20, error, "", 9)
		|| !RawImage::Create(smallRaw, 40ULL << 20, error)) {
		Fail("create small: " + error);
		return;
	}
	{
		std::unique_ptr<DiskImage> qcow = DiskImage::Open(small, false,
			error);
		std::unique_ptr<DiskImage> raw = DiskImage::Open(smallRaw, false,
			error);
		RandomWrites(*qcow, *raw, 1500, 3);
		if (!SameContents(*qcow, *raw))
			Fail("small clusters: contents differ");
		std::string report;
		if (static_cast<Qcow2Image*>(qcow.get())->Check(report) != 0)
			Fail("small clusters: check: " + report);
	}
	if (qemu && !Run("qemu-img check " + small))
		Fail("qemu-img check of the small cluster image");
	printf("ok: 512 byte clusters (reference count table growth)\n");
}


void
TestQemuImages(const std::string& dir)
{
	std::string rawPath = dir + "/test.raw";
	struct Case {
		const char* name;
		const char* options;
	} cases[] = {
		{ "plain", "-O qcow2" },
		{ "compressed", "-O qcow2 -c" },
		{ "v2 small clusters", "-O qcow2 -o compat=0.10,cluster_size=4096" },
		{ "8-bit refcounts", "-O qcow2 -o refcount_bits=8" },
	};
	for (const Case& test : cases) {
		std::string path = dir + "/qemu.qcow2";
		unlink(path.c_str());
		if (!Run(std::string("qemu-img convert -f raw ") + test.options + " "
				+ rawPath + " " + path)) {
			Fail(std::string("qemu-img convert ") + test.name);
			continue;
		}
		std::string error;
		std::unique_ptr<DiskImage> qcow = DiskImage::Open(path, false, error);
		std::unique_ptr<DiskImage> raw = DiskImage::Open(rawPath, false,
			error);
		if (!qcow || !raw) {
			Fail(std::string("open qemu image ") + test.name + ": " + error);
			continue;
		}
		if (!SameContents(*qcow, *raw)) {
			Fail(std::string("qemu image differs: ") + test.name);
			continue;
		}
		// write over it (compressed clusters get replaced)
		RandomWrites(*qcow, *raw, 500, 7);
		qcow.reset();
		raw.reset();
		if (!Run("qemu-img check " + path))
			Fail(std::string("qemu-img check after writes: ") + test.name);
		if (!Run("qemu-img compare -f qcow2 -F raw " + path + " " + rawPath))
			Fail(std::string("compare after writes: ") + test.name);
		printf("ok: qemu-img image (%s), read and 500 writes\n", test.name);
	}

	// overlay on a backing file
	std::string base = dir + "/base.qcow2";
	std::string overlay = dir + "/overlay.qcow2";
	std::string copy = dir + "/copy.raw";
	unlink(base.c_str());
	unlink(overlay.c_str());
	unlink(copy.c_str());
	if (!Run("qemu-img convert -f raw -O qcow2 " + rawPath + " " + base)
		|| !Run("cp " + rawPath + " " + copy)) {
		Fail("prepare backing test");
		return;
	}
	std::string error;
	if (!Qcow2Image::Create(overlay, (200ULL << 20), error, "base.qcow2")) {
		Fail("create overlay: " + error);
		return;
	}
	{
		std::unique_ptr<DiskImage> image = DiskImage::Open(overlay, false,
			error);
		std::unique_ptr<DiskImage> raw = DiskImage::Open(copy, false, error);
		if (!image || !raw) {
			Fail("open overlay: " + error);
			return;
		}
		if (!SameContents(*image, *raw))
			Fail("overlay does not show the backing file");
		RandomWrites(*image, *raw, 300, 11);
		if (!SameContents(*image, *raw))
			Fail("overlay differs after writes");
	}
	if (!Run("qemu-img check " + overlay))
		Fail("qemu-img check of the overlay");
	if (!Run("qemu-img compare -F raw " + overlay + " " + copy))
		Fail("qemu-img compare of the overlay");
	printf("ok: overlay on a backing file\n");
}

}	// namespace


int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: qcow2-tests WORKDIR\n");
		return 2;
	}
	std::string dir = argv[1];
	bool qemu = HaveQemuImg();
	if (!qemu)
		printf("qemu-img not found: only self checks\n");
	TestOwnImages(dir, qemu);
	if (qemu)
		TestQemuImages(dir);
	printf("%d failures\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
