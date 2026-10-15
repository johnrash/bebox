/*
 * BeBox emulator - the Be file system of the Hobbit prototypes (reading).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "HobbitVolume.h"

#include "../storage/DiskImage.h"

#include <algorithm>

namespace bebox {

static const u32 kSectorSize = 512;
static const u32 kDirectoryBlockSectors = 8;
static const u32 kDirectoryEntries = 63;
static const u32 kExtentsPerMapSector = 63;


HobbitVolume::HobbitVolume(DiskImage& image)
	:
	fImage(image),
	fRoot(0),
	fTotalSectors(0)
{
}


bool
HobbitVolume::_ReadSectors(u32 sector, u32 count, void* buffer)
{
	if (sector == 0 || (u64)sector + count > fTotalSectors)
		return false;
	return fImage.Read((u64)sector * kSectorSize, buffer,
		(size_t)count * kSectorSize);
}


bool
HobbitVolume::Mount(std::string& error)
{
	u8 header[kSectorSize];
	if (!fImage.Read(0, header, sizeof(header))) {
		error = "cannot read the volume header";
		return false;
	}
	u32 version = LoadBE32(header);
	u32 bytesPerSector = LoadBE32(header + 0x18);
	fRoot = LoadBE32(header + 0x10);
	fTotalSectors = LoadBE32(header + 0x14);
	if (version != 0x00010000 || bytesPerSector != kSectorSize
		|| fTotalSectors == 0
		|| (u64)fTotalSectors * kSectorSize > fImage.Size() + kSectorSize
		|| fRoot == 0 || fRoot >= fTotalSectors) {
		error = "not a Hobbit Be file system volume";
		return false;
	}
	char name[33];
	memcpy(name, header + 0x2c, 32);
	name[32] = '\0';
	fName = name;
	return true;
}


bool
HobbitVolume::ReadDirectory(u32 sector, std::vector<Entry>& entries)
{
	entries.clear();
	u8 block[kDirectoryBlockSectors * kSectorSize];
	for (int blocks = 0; sector != 0; blocks++) {
		if (blocks > 4096 || !_ReadSectors(sector, kDirectoryBlockSectors,
				block))
			return false;
		for (u32 i = 0; i < kDirectoryEntries; i++) {
			const u8* raw = block + 64 + i * 64;
			Entry entry;
			entry.first = LoadBE32(raw + 32);
			if (entry.first == 0 || raw[0] == 0)
				continue;
			char name[33];
			memcpy(name, raw, 32);
			name[32] = '\0';
			entry.name = name;
			entry.count = LoadBE32(raw + 36);
			entry.type = LoadBE32(raw + 40);
			entry.created = LoadBE32(raw + 44);
			entry.modified = LoadBE32(raw + 48);
			entry.size = LoadBE32(raw + 52);
			entry.creator = LoadBE32(raw + 60);
			entries.push_back(entry);
		}
		sector = LoadBE32(block);
	}
	return true;
}


bool
HobbitVolume::Lookup(const std::string& path, Entry& entry,
	std::string& error)
{
	entry = Entry();
	entry.first = fRoot;
	entry.type = 0xffffffff;
	size_t position = 0;
	while (position < path.size()) {
		size_t slash = path.find('/', position);
		if (slash == std::string::npos)
			slash = path.size();
		std::string name = path.substr(position, slash - position);
		position = slash + 1;
		if (name.empty())
			continue;
		if (!entry.IsDirectory()) {
			error = path + ": not a directory";
			return false;
		}
		std::vector<Entry> entries;
		if (!ReadDirectory(entry.first, entries)) {
			error = path + ": cannot read a directory";
			return false;
		}
		auto found = std::find_if(entries.begin(), entries.end(),
			[&](const Entry& candidate) { return candidate.name == name; });
		if (found == entries.end()) {
			error = path + ": not found on " + fName;
			return false;
		}
		entry = *found;
	}
	return true;
}


bool
HobbitVolume::ReadFile(const Entry& entry, std::vector<u8>& data,
	std::string& error)
{
	data.clear();
	if (entry.IsDirectory()) {
		error = entry.name + ": a directory";
		return false;
	}
	u32 sectors = (entry.size + kSectorSize - 1) / kSectorSize;
	std::vector<u8> buffer((size_t)sectors * kSectorSize);
	if ((entry.first & 0x80000000) != 0) {
		if (sectors > 0 && !_ReadSectors(entry.first & 0x7fffffff, sectors,
				buffer.data())) {
			error = entry.name + ": cannot read the data";
			return false;
		}
	} else {
		// map sectors: next, first logical sector, extents
		u32 map = entry.first;
		u32 done = 0;
		for (int maps = 0; map != 0 && done < sectors; maps++) {
			u8 raw[kSectorSize];
			if (maps > 65536 || !_ReadSectors(map, 1, raw)) {
				error = entry.name + ": cannot read the block map";
				return false;
			}
			for (u32 i = 0; i < kExtentsPerMapSector && done < sectors; i++) {
				u32 start = LoadBE32(raw + 8 + i * 8);
				u32 count = LoadBE32(raw + 12 + i * 8);
				if (start == 0xffffffff)
					break;
				// (older volumes start with an empty extent: the map)
				if (count == 0)
					continue;
				count = std::min(count, sectors - done);
				if (!_ReadSectors(start, count,
						buffer.data() + (size_t)done * kSectorSize)) {
					error = entry.name + ": cannot read the data";
					return false;
				}
				done += count;
			}
			map = LoadBE32(raw);
		}
		if (done < sectors) {
			error = entry.name + ": the block map is short";
			return false;
		}
	}
	buffer.resize(entry.size);
	data.swap(buffer);
	return true;
}


bool
HobbitVolume::ReadFile(const std::string& path, std::vector<u8>& data,
	std::string& error)
{
	Entry entry;
	return Lookup(path, entry, error) && ReadFile(entry, data, error);
}

}	// namespace bebox
