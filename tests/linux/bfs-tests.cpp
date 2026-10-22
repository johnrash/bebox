/*
 * BeBox emulator - BFS writer tests: volumes written by BfsWriter are read
 * back with BfsReader (big directories with multi-level B+trees, nested
 * directories, small and attribute directory attributes, a file large
 * enough for indirect runs); shared folders synced back, also from a
 * damaged volume and to a folder that refuses writes.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 *   bfs-tests WORKDIR
 */
#include "core/storage/BfsReader.h"
#include "core/storage/BfsWriter.h"
#include "core/storage/DiskImage.h"
#include "core/storage/SharedFolder.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace bebox;

namespace {

int sFailures = 0;

void
Check(bool ok, const std::string& what)
{
	printf("%s: %s\n", ok ? "ok" : "FAILED", what.c_str());
	if (!ok)
		sFailures++;
}


u8
Pattern(const std::string& name, u64 offset)
{
	u32 hash = 2166136261u;
	for (char c : name)
		hash = (hash ^ (u8)c) * 16777619u;
	return (u8)(hash + offset * 7 + (offset >> 9));
}


BfsWriter::Node
File(const std::string& name, u64 size)
{
	BfsWriter::Node node;
	node.name = name;
	node.size = size;
	node.modified = 1700000000 + (s64)size;
	node.created = 1600000000;
	node.read = [name](u64 offset, void* buffer, size_t length) {
		u8* bytes = (u8*)buffer;
		for (size_t i = 0; i < length; i++)
			bytes[i] = Pattern(name, offset + i);
		return true;
	};
	return node;
}


BfsWriter::Node
Directory(const std::string& name)
{
	BfsWriter::Node node;
	node.name = name;
	node.directory = true;
	node.permissions = 0755;
	node.modified = 1700000000;
	return node;
}


bool
SameContent(BfsReader& reader, u64 block, const std::string& name, u64 size)
{
	std::vector<u8> buffer(1 << 20);
	// whole files when small, samples of large ones
	for (u64 offset = 0; offset < size; ) {
		size_t length = (size_t)std::min<u64>(buffer.size(), size - offset);
		if (!reader.ReadAt(block, offset, buffer.data(), length))
			return false;
		for (size_t i = 0; i < length; i++) {
			if (buffer[i] != Pattern(name, offset + i))
				return false;
		}
		offset += size > (64u << 20) ? (37u << 20) : length;
	}
	return true;
}

// Attributes kept in memory, little endian like an x86 Haiku's.
class FakeAttributes : public HostAttributes {
public:
	virtual bool Read(const std::string& path,
		std::vector<BfsAttribute>& attributes)
	{
		attributes = store[path];
		return true;
	}

	virtual bool Write(const std::string& path,
		const std::vector<BfsAttribute>& attributes)
	{
		for (const BfsAttribute& attribute : attributes) {
			std::vector<BfsAttribute>& list = store[path];
			bool replaced = false;
			for (BfsAttribute& existing : list) {
				if (existing.name == attribute.name) {
					existing = attribute;
					replaced = true;
				}
			}
			if (!replaced)
				list.push_back(attribute);
		}
		return true;
	}

	std::map<std::string, std::vector<BfsAttribute> > store;
};


void
WriteText(const std::string& path, const std::string& text)
{
	std::ofstream(path) << text;
}


std::string
ReadText(const std::string& path)
{
	std::ifstream file(path);
	std::stringstream text;
	text << file.rdbuf();
	return text.str();
}


bool
Exists(const std::string& path)
{
	struct stat st;
	return lstat(path.c_str(), &st) == 0;
}


BfsWriter::Node
TextNode(const std::string& name, const std::string& text)
{
	BfsWriter::Node node;
	node.name = name;
	node.size = text.size();
	node.modified = 1800000000;
	node.read = [text](u64 offset, void* buffer, size_t length) {
		memcpy(buffer, text.data() + offset, length);
		return true;
	};
	return node;
}


// A folder shared, changed "by BeOS" (the volume rewritten), synced back.
void
TestSharedFolder(const std::string& work)
{
	std::string folder = work + "/shared";
	std::string image = work + "/shared.qcow2";
	system(("rm -rf '" + folder + "' '" + image + "' '" + image
		+ ".snapshot'").c_str());
	mkdir(folder.c_str(), 0755);
	mkdir((folder + "/sub").c_str(), 0755);
	WriteText(folder + "/a.txt", "alpha");
	WriteText(folder + "/sub/b.txt", "bravo");
	WriteText(folder + "/c.txt", "charlie");
	WriteText(folder + "/d.txt", "delta");

	FakeAttributes attributes;
	attributes.store[folder + "/a.txt"] = {
		{ "BEOS:TYPE", 0x4d494d53, { 't', 'e', 'x', 't', '/', 'p', 'l', 'a',
			'i', 'n', 0 } },
		{ "test:int", 0x4c4f4e47, { 0x04, 0x03, 0x02, 0x01 } } };	// 'LONG'

	SharedFolder share(folder, image, &attributes);
	std::string report, error;
	Check(share.Prepare(report, error), "shared folder volume built"
		+ (error.empty() ? std::string() : ": " + error));

	{
		std::unique_ptr<DiskImage> disk = DiskImage::Open(image, true, error);
		BfsReader reader([&disk](u64 offset, void* buffer, size_t length) {
			return disk->Read(offset, buffer, length);
		});
		std::vector<u64> volumes = reader.FindVolumes(std::vector<u64>());
		bool mounted = !volumes.empty() && reader.Mount(volumes[0]);
		std::vector<u8> data;
		Check(mounted && reader.VolumeName() == "shared"
			&& reader.ReadFile("/sub/b.txt", data, error)
			&& std::string(data.begin(), data.end()) == "bravo",
			"shared folder content");
		BfsReader::RawEntries entries;
		reader.ListBlock(reader.RootBlock(), entries);
		std::vector<BfsAttribute> list;
		for (const auto& item : entries) {
			if (item.first == "a.txt")
				reader.ReadAttributes(item.second, list);
		}
		bool swapped = false;
		for (const BfsAttribute& attribute : list) {
			if (attribute.name == "test:int")
				swapped = attribute.data == std::vector<u8>({ 1, 2, 3, 4 });
		}
		Check(list.size() == 2 && swapped,
			"attributes on the volume (big endian)");
	}

	// "BeOS" changes the volume: b changed, c deleted, e new, a's integer
	// attribute changed, d changed (and on the host meanwhile)
	BfsWriter::Node root;
	root.directory = true;
	root.name = "shared";
	BfsWriter::Node a = TextNode("a.txt", "alpha");
	a.attributes = { { "BEOS:TYPE", 0x4d494d53, { 't', 'e', 'x', 't', '/',
			'p', 'l', 'a', 'i', 'n', 0 } },
		{ "test:int", 0x4c4f4e47, { 0x0a, 0x0b, 0x0c, 0x0d } } };
	BfsWriter::Node sub;
	sub.directory = true;
	sub.name = "sub";
	sub.children.push_back(TextNode("b.txt", "bravo from BeOS"));
	root.children.push_back(a);
	root.children.push_back(sub);
	root.children.push_back(TextNode("d.txt", "delta from BeOS"));
	root.children.push_back(TextNode("e.txt", "echo"));
	{
		std::unique_ptr<DiskImage> disk = DiskImage::Open(image, false,
			error);
		Check(disk && BfsWriter::Write([&disk](u64 offset, const void* buffer,
				size_t length) {
				return disk->Write(offset, buffer, length);
			}, disk->Size(), "shared", root, error), "volume changed");
	}
	sleep(1);
	WriteText(folder + "/d.txt", "delta changed on the host");

	Check(share.Sync(report, error), "sync" + (error.empty() ? std::string()
		: ": " + error));
	printf("  report: %s\n", report.c_str());
	Check(ReadText(folder + "/sub/b.txt") == "bravo from BeOS",
		"changed file synced");
	Check(!Exists(folder + "/c.txt"), "deleted file removed");
	Check(ReadText(folder + "/e.txt") == "echo", "new file synced");
	Check(ReadText(folder + "/d.txt") == "delta changed on the host"
		&& ReadText(folder + "/d.txt (BeBox)") == "delta from BeOS",
		"conflict keeps both versions");
	bool attributeBack = false;
	for (const BfsAttribute& attribute : attributes.store[folder + "/a.txt"]) {
		if (attribute.name == "test:int")
			attributeBack = attribute.data
				== std::vector<u8>({ 0x0d, 0x0c, 0x0b, 0x0a });
	}
	Check(attributeBack, "attribute synced (host byte order)");
	Check(!Exists(image) && !Exists(image + ".snapshot"),
		"volume and snapshot removed after the sync");
}


// Zeroes 'length' bytes of the shared volume: at the start of the inode of
// 'path', or of its directory tree (B+tree header) with 'tree'.
bool
Damage(const std::string& image, const std::string& path, bool tree)
{
	std::string error;
	std::unique_ptr<DiskImage> disk = DiskImage::Open(image, false, error);
	if (!disk)
		return false;
	std::vector<u64> reads;
	BfsReader reader([&disk, &reads](u64 offset, void* buffer,
			size_t length) {
		reads.push_back(offset);
		return disk->Read(offset, buffer, length);
	});
	std::vector<u64> volumes = reader.FindVolumes(std::vector<u64>());
	u64 block;
	if (volumes.empty() || !reader.Mount(volumes[0])
		|| !reader.LookupBlock(path, block, error))
		return false;
	// the inode is read first, then the tree header
	reads.clear();
	BfsReader::RawEntries entries;
	BfsReader::Stat stat;
	if (tree ? !reader.ListBlock(block, entries)
			: !reader.StatInode(block, stat))
		return false;
	if (reads.size() < (tree ? 2u : 1u))
		return false;
	u64 offset = reads[tree ? 1 : 0];
	const u8 zeros[4] = { 0, 0, 0, 0 };
	return disk->Write(offset, zeros, sizeof(zeros));
}


// Syncing never deletes what it could not see on the volume, keeps the
// volume when something failed, and does the rest at the next try without
// taking its own copies for conflicts.
void
TestSharedFolderFailures(const std::string& work)
{
	std::string folder = work + "/failing";
	std::string image = work + "/failing.qcow2";
	auto fresh = [&](SharedFolder& share) {
		system(("chmod -R u+w '" + folder + "' 2>/dev/null; rm -rf '"
			+ folder + "' '" + image + "' '" + image + ".snapshot'").c_str());
		mkdir(folder.c_str(), 0755);
		mkdir((folder + "/sub").c_str(), 0755);
		WriteText(folder + "/a.txt", "alpha");
		WriteText(folder + "/keep.txt", "keep");
		WriteText(folder + "/sub/b.txt", "bravo");
		WriteText(folder + "/sub/c.txt", "charlie");
		std::string report, error;
		return share.Prepare(report, error);
	};
	std::string report, error;

	// a directory BeOS left unchanged whose tree cannot be read
	{
		SharedFolder share(folder, image);
		Check(fresh(share) && Damage(image, "/sub", true),
			"sync failures: volume with an unreadable directory");
		bool synced = share.Sync(report, error);
		Check(!synced, "sync failures: unreadable directory fails ("
			+ error + ")");
		Check(ReadText(folder + "/sub/b.txt") == "bravo"
			&& ReadText(folder + "/sub/c.txt") == "charlie",
			"sync failures: its files stay on the host");
		Check(Exists(image) && Exists(image + ".snapshot"),
			"sync failures: volume and snapshot kept");
	}
	// a file whose inode cannot be read
	{
		SharedFolder share(folder, image);
		Check(fresh(share) && Damage(image, "/sub/c.txt", false),
			"sync failures: volume with an unreadable inode");
		bool synced = share.Sync(report, error);
		Check(!synced, "sync failures: unreadable inode fails (" + error
			+ ")");
		Check(ReadText(folder + "/sub/c.txt") == "charlie",
			"sync failures: the file stays on the host");
	}

	// BeOS moved a.txt into sub, added sub/new.txt and changed keep.txt;
	// sub on the host refuses writes
	SharedFolder share(folder, image);
	Check(fresh(share), "sync failures: folder shared");
	BfsWriter::Node root;
	root.directory = true;
	root.name = "failing";
	BfsWriter::Node sub;
	sub.directory = true;
	sub.name = "sub";
	sub.children.push_back(TextNode("a.txt", "alpha"));
	sub.children.push_back(TextNode("b.txt", "bravo"));
	sub.children.push_back(TextNode("c.txt", "charlie"));
	sub.children.push_back(TextNode("new.txt", "new"));
	root.children.push_back(TextNode("keep.txt", "keep from BeOS"));
	root.children.push_back(sub);
	{
		std::unique_ptr<DiskImage> disk = DiskImage::Open(image, false,
			error);
		Check(disk && BfsWriter::Write([&disk](u64 offset, const void* buffer,
				size_t length) {
				return disk->Write(offset, buffer, length);
			}, disk->Size(), "failing", root, error),
			"sync failures: volume changed");
	}
	chmod((folder + "/sub").c_str(), 0555);
	if (access((folder + "/sub").c_str(), W_OK) == 0) {
		printf("skip: sync failures with a read-only folder (running as "
			"root)\n");
		chmod((folder + "/sub").c_str(), 0755);
		return;
	}
	bool synced = share.Sync(report, error);
	Check(!synced, "sync failures: a file that cannot be written fails ("
		+ error + ")");
	Check(ReadText(folder + "/a.txt") == "alpha",
		"sync failures: no deletions (the moved file stays)");
	Check(ReadText(folder + "/keep.txt") == "keep from BeOS",
		"sync failures: what could be written is");
	Check(Exists(image) && Exists(image + ".snapshot"),
		"sync failures: volume and snapshot kept");

	// The next power on syncs the volume left behind (while it still
	// fails, BeOS gets it back), then builds a new one.
	SharedFolder again(folder, image);
	error.clear();
	Check(again.Prepare(report, error) && !report.empty() && Exists(image)
		&& ReadText(folder + "/a.txt") == "alpha",
		"sync failures: still failing, the volume goes back to BeOS");
	chmod((folder + "/sub").c_str(), 0755);
	error.clear();
	bool prepared = again.Prepare(report, error);
	Check(prepared, "sync failures: synced at the next power on"
		+ (error.empty() ? std::string() : ": " + error));
	printf("  report: %s\n", report.c_str());
	Check(ReadText(folder + "/sub/a.txt") == "alpha"
		&& ReadText(folder + "/sub/new.txt") == "new"
		&& !Exists(folder + "/a.txt"),
		"sync failures: the move done");
	Check(!Exists(folder + "/keep.txt (BeBox)")
		&& !Exists(folder + "/sub/b.txt (BeBox)"),
		"sync failures: no conflict copies of what was synced before");
	synced = again.Sync(report, error);
	Check(synced && !Exists(image) && !Exists(image + ".snapshot"),
		"sync failures: the new volume synced and removed");
}

}	// namespace


int
main(int argc, char** argv)
{
	std::string work = argc > 1 ? argv[1] : "/tmp";
	std::string path = work + "/bfs-test.img";
	unlink(path.c_str());
	const u64 kVolume = 2ull << 30;
	std::string error;
	if (!RawImage::Create(path, kVolume, error)) {
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	std::unique_ptr<DiskImage> image = DiskImage::Open(path, false, error);

	BfsWriter::Node root = Directory("root");
	BfsWriter::Node many = Directory("many");
	for (int i = 0; i < 3000; i++) {
		char name[32];
		snprintf(name, sizeof(name), "file %04d.txt", i);
		many.children.push_back(File(name, (u64)(i * 37) % 5000));
	}
	root.children.push_back(many);
	BfsWriter::Node nested = Directory("nested");
	BfsWriter::Node inner = Directory("inner");
	BfsWriter::Node attributed = File("attributed", 100);
	attributed.attributes.push_back({ "BEOS:TYPE", 0x4d494d53,	// 'MIMS'
		std::vector<u8>({ 't', 'e', 'x', 't', '/', 'p', 'l', 'a', 'i', 'n',
			0 }) });
	std::vector<u8> icon(5000);
	for (size_t i = 0; i < icon.size(); i++)
		icon[i] = (u8)(i * 13);
	attributed.attributes.push_back({ "BEOS:L:STD_ICON", 0x49434f4e,
		icon });
	inner.children.push_back(attributed);
	inner.children.push_back(File("empty", 0));
	nested.children.push_back(inner);
	root.children.push_back(nested);
	root.children.push_back(File("large.bin", 420ull << 20));
	root.children.push_back(File("odd.bin", 2049));

	bool written = BfsWriter::Write(
		[&image](u64 offset, const void* buffer, size_t length) {
			return image->Write(offset, buffer, length);
		}, kVolume, "Shared", root, error);
	Check(written, "write volume" + (written ? std::string() : ": " + error));
	image->Flush();

	BfsReader reader([&image](u64 offset, void* buffer, size_t length) {
		return image->Read(offset, buffer, length);
	});
	std::vector<u64> volumes = reader.FindVolumes(std::vector<u64>());
	Check(volumes.size() == 1 && reader.Mount(volumes[0]), "mount");
	Check(reader.IsClean(), "clean volume");
	Check(reader.VolumeName() == "Shared", "volume name");

	BfsReader::RawEntries top;
	Check(reader.ListBlock(reader.RootBlock(), top) && top.size() == 4,
		"root directory");
	std::map<std::string, u64> topMap(top.begin(), top.end());

	BfsReader::RawEntries manyEntries;
	bool manyOk = reader.ListBlock(topMap["many"], manyEntries)
		&& manyEntries.size() == 3000;
	int bad = 0;
	for (const auto& item : manyEntries) {
		BfsReader::Stat stat;
		int index = atoi(item.first.c_str() + 5);
		u64 size = (u64)(index * 37) % 5000;
		if (!reader.StatInode(item.second, stat) || stat.size != size
			|| BfsWriter::FromBfsTime(stat.modified)
				!= 1700000000 + (s64)size
			|| !SameContent(reader, item.second, item.first, size))
			bad++;
	}
	Check(manyOk && bad == 0, "3000 files (multi-level directory)");
	std::vector<BfsReader::Entry> listed;
	Check(reader.ListDirectory("/many", listed, error)
		&& listed.size() == 3000, "list by path");

	std::vector<u8> data;
	Check(reader.ReadFile("/nested/inner/attributed", data, error)
		&& data.size() == 100, "nested file by path");
	BfsReader::RawEntries innerEntries;
	std::vector<u8> unused;
	u64 attributedBlock = 0;
	{
		BfsReader::RawEntries nestedEntries;
		reader.ListBlock(topMap["nested"], nestedEntries);
		reader.ListBlock(nestedEntries.at(0).second, innerEntries);
		for (const auto& item : innerEntries) {
			if (item.first == "attributed")
				attributedBlock = item.second;
		}
	}
	std::vector<BfsAttribute> attributes;
	Check(reader.ReadAttributes(attributedBlock, attributes)
		&& attributes.size() == 2, "attributes read");
	bool attributesOk = attributes.size() == 2;
	for (const BfsAttribute& attribute : attributes) {
		if (attribute.name == "BEOS:TYPE")
			attributesOk &= attribute.type == 0x4d494d53
				&& attribute.data.size() == 11;
		else
			attributesOk &= attribute.name == "BEOS:L:STD_ICON"
				&& attribute.data == icon;
	}
	Check(attributesOk, "small data and attribute directory attributes");

	BfsReader::Stat largeStat;
	Check(reader.StatInode(topMap["large.bin"], largeStat)
		&& largeStat.size == (420ull << 20)
		&& SameContent(reader, topMap["large.bin"], "large.bin",
			largeStat.size), "420 MB file (indirect runs)");
	BfsReader::Stat oddStat;
	Check(reader.StatInode(topMap["odd.bin"], oddStat)
		&& SameContent(reader, topMap["odd.bin"], "odd.bin", 2049),
		"odd sized file");

	image.reset();
	unlink(path.c_str());

	TestSharedFolder(work);
	TestSharedFolderFailures(work);

	printf("%d failures\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
