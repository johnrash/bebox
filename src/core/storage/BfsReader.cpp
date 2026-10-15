/*
 * BeBox emulator - read only access to Be File System volumes.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "BfsReader.h"

#include "CdImage.h"
#include "DiskImage.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace bebox {

namespace {

const u32 kSuperMagic1 = 0x42465331;		// "BFS1"
const u32 kSuperMagic2 = 0xdd121031;
const u32 kInodeMagic = 0x3bbe0ad9;
const u32 kTreeMagic = 0x69f6c2e8;
const u64 kTreeNull = ~(u64)0;
const u32 kDirectoryMode = 0040000;
const u32 kTypeMask = 0170000;
const u32 kClean = 0x434c454e;			// "CLEN"
const size_t kSmallDataStart = 232;

}	// namespace


BfsReader::BfsReader(ReadFunction read)
	:
	fRead(read),
	fVolume(0),
	fBigEndian(true),
	fBlockSize(0),
	fAgShift(0),
	fInodeSize(0)
{
}


u16
BfsReader::_U16(const u8* p) const
{
	return fBigEndian ? LoadBE16(p) : (u16)(p[0] | (p[1] << 8));
}


u32
BfsReader::_U32(const u8* p) const
{
	return fBigEndian ? LoadBE32(p) : LoadLE32(p);
}


u64
BfsReader::_U64(const u8* p) const
{
	return fBigEndian ? LoadBE64(p)
		: (u64)LoadLE32(p) | ((u64)LoadLE32(p + 4) << 32);
}


BfsReader::BlockRun
BfsReader::_Run(const u8* p) const
{
	BlockRun run;
	run.group = _U32(p);
	run.start = _U16(p + 4);
	run.length = _U16(p + 6);
	return run;
}


// PowerPC volumes have the superblock at byte 0, x86 ones at byte 512.
bool
BfsReader::_TryVolume(u64 offset)
{
	u8 buffer[1024];
	if (!fRead(offset, buffer, sizeof(buffer)))
		return false;
	for (u32 at : { 0u, 512u }) {
		const u8* super = buffer + at;
		if (memcmp(super + 32, "BFS1", 4) == 0)
			fBigEndian = true;
		else if (memcmp(super + 32, "1SFB", 4) == 0)
			fBigEndian = false;
		else
			continue;
		if (_U32(super + 32) != kSuperMagic1
			|| _U32(super + 68) != kSuperMagic2)
			continue;
		fBlockSize = _U32(super + 40);
		fAgShift = _U32(super + 76);
		fInodeSize = _U32(super + 64);
		if (fBlockSize < 512 || fBlockSize > 65536
			|| (fBlockSize & (fBlockSize - 1)) != 0 || fAgShift > 31)
			continue;
		fRoot = _Run(super + 116);
		// clean: marked so, with nothing left in the log
		fClean = _U32(super + 84) == kClean
			&& _U64(super + 96) == _U64(super + 104);
		fName.assign((const char*)super,
			strnlen((const char*)super, 32));
		fVolume = offset;
		return true;
	}
	return false;
}


std::vector<u64>
BfsReader::FindVolumes(const std::vector<u64>& candidates)
{
	std::vector<u64> offsets;
	// Apple partition map (PowerPC install CDs)
	u8 entry[512];
	for (int i = 1; i < 64; i++) {
		if (!fRead((u64)i * 512, entry, sizeof(entry))
			|| entry[0] != 'P' || entry[1] != 'M')
			break;
		if (strncmp((const char*)entry + 48, "Be_BFS", 32) == 0)
			offsets.push_back((u64)LoadBE32(entry + 8) * 512);
	}
	offsets.push_back(0);
	for (u64 offset : candidates) {
		if (offset != 0)
			offsets.push_back(offset);
	}
	std::vector<u64> bigEndian, littleEndian;
	for (u64 offset : offsets) {
		if (!_TryVolume(offset))
			continue;
		std::vector<u64>& list = fBigEndian ? bigEndian : littleEndian;
		if (std::find(list.begin(), list.end(), offset) == list.end())
			list.push_back(offset);
	}
	bigEndian.insert(bigEndian.end(), littleEndian.begin(),
		littleEndian.end());
	return bigEndian;
}


bool
BfsReader::Mount(u64 volume)
{
	return _TryVolume(volume);
}


bool
BfsReader::_ReadBlocks(u64 block, u32 count, void* buffer)
{
	return fRead(fVolume + block * fBlockSize, buffer,
		(size_t)count * fBlockSize);
}


bool
BfsReader::_ReadInode(u64 block, Inode& inode)
{
	std::vector<u8> data(fBlockSize);
	if (!_ReadBlocks(block, 1, data.data()))
		return false;
	const u8* p = data.data();
	if (_U32(p) != kInodeMagic)
		return false;
	inode.mode = _U32(p + 20);
	inode.flags = _U32(p + 24);
	inode.created = (s64)_U64(p + 28);
	inode.modified = (s64)_U64(p + 36);
	inode.parent = _Run(p + 44);
	inode.attributes = _Run(p + 52);
	inode.type = _U32(p + 60);
	for (int i = 0; i < 12; i++)
		inode.direct[i] = _Run(p + 72 + i * 8);
	inode.maxDirectRange = _U64(p + 168);
	inode.indirect = _Run(p + 176);
	inode.maxIndirectRange = _U64(p + 184);
	inode.doubleIndirect = _Run(p + 192);
	inode.maxDoubleIndirectRange = _U64(p + 200);
	inode.size = _U64(p + 208);
	return true;
}


// The volume block holding stream offset 'offset'.
bool
BfsReader::_FindBlock(const Inode& inode, u64 offset, u64& block)
{
	if (offset < inode.maxDirectRange) {
		u64 position = 0;
		for (const BlockRun& run : inode.direct) {
			u64 runBytes = (u64)run.length * fBlockSize;
			if (offset < position + runBytes) {
				block = _BlockOf(run) + (offset - position) / fBlockSize;
				return true;
			}
			position += runBytes;
		}
		return false;
	}
	if (offset < inode.maxIndirectRange) {
		// the indirect run holds an array of block runs
		std::vector<u8> runs((size_t)inode.indirect.length * fBlockSize);
		if (!_ReadBlocks(_BlockOf(inode.indirect), inode.indirect.length,
				runs.data()))
			return false;
		u64 position = inode.maxDirectRange;
		for (size_t i = 0; i + 8 <= runs.size(); i += 8) {
			BlockRun run = _Run(&runs[i]);
			if (run.length == 0)
				break;
			u64 runBytes = (u64)run.length * fBlockSize;
			if (offset < position + runBytes) {
				block = _BlockOf(run) + (offset - position) / fBlockSize;
				return true;
			}
			position += runBytes;
		}
		return false;
	}
	if (offset < inode.maxDoubleIndirectRange) {
		// Each double indirect run points to an array (4 blocks) of runs
		// of 4 blocks each.
		const u32 kArrayBlocks = 4;
		u32 runsPerBlock = fBlockSize / 8;
		u64 directSize = (u64)kArrayBlocks * fBlockSize;
		u64 indirectSize = directSize * kArrayBlocks * runsPerBlock;
		u64 relative = offset - inode.maxIndirectRange;
		u64 index = relative / indirectSize;
		std::vector<u8> outer((size_t)kArrayBlocks * fBlockSize);
		if (!_ReadBlocks(_BlockOf(inode.doubleIndirect), kArrayBlocks,
				outer.data()) || index * 8 + 8 > outer.size())
			return false;
		BlockRun array = _Run(&outer[index * 8]);
		std::vector<u8> inner((size_t)kArrayBlocks * fBlockSize);
		if (!_ReadBlocks(_BlockOf(array), kArrayBlocks, inner.data()))
			return false;
		u64 within = relative % indirectSize;
		u64 runIndex = within / directSize;
		if (runIndex * 8 + 8 > inner.size())
			return false;
		BlockRun run = _Run(&inner[runIndex * 8]);
		block = _BlockOf(run) + (within % directSize) / fBlockSize;
		return true;
	}
	return false;
}


bool
BfsReader::_ReadStream(const Inode& inode, u64 offset, void* buffer,
	size_t length)
{
	u8* out = (u8*)buffer;
	std::vector<u8> data(fBlockSize);
	while (length > 0) {
		u64 block;
		if (!_FindBlock(inode, offset, block)
			|| !_ReadBlocks(block, 1, data.data()))
			return false;
		u32 inBlock = (u32)(offset % fBlockSize);
		size_t chunk = std::min<size_t>(length, fBlockSize - inBlock);
		memcpy(out, data.data() + inBlock, chunk);
		out += chunk;
		offset += chunk;
		length -= chunk;
	}
	return true;
}


// B+tree directory lookup: descend from the root (interior nodes lead to
// the first key not less than the name, else the overflow link) to a
// leaf holding the name's inode block.
bool
BfsReader::_Lookup(const Inode& directory, const std::string& name,
	u64& block)
{
	u8 header[32];
	if (!_ReadStream(directory, 0, header, sizeof(header))
		|| _U32(header) != kTreeMagic)
		return false;
	u32 nodeSize = _U32(header + 4);
	u64 node = _U64(header + 16);
	if (nodeSize < 64 || nodeSize > 65536)
		return false;
	std::vector<u8> data(nodeSize);
	for (int depth = 0; depth < 32; depth++) {
		if (node == kTreeNull || !_ReadStream(directory, node, data.data(),
				nodeSize))
			return false;
		const u8* p = data.data();
		u64 overflow = _U64(p + 16);
		u16 count = _U16(p + 24);
		u16 keyLength = _U16(p + 26);
		size_t lengthsAt = (28 + keyLength + 7) & ~(size_t)7;
		size_t valuesAt = lengthsAt + (size_t)count * 2;
		if (valuesAt + (size_t)count * 8 > nodeSize)
			return false;
		bool leaf = overflow == kTreeNull;
		u64 next = overflow;
		for (u16 i = 0; i < count; i++) {
			u16 start = i == 0 ? 0 : _U16(p + lengthsAt + (i - 1) * 2);
			u16 end = _U16(p + lengthsAt + i * 2);
			if (end < start || 28u + end > nodeSize)
				return false;
			std::string key((const char*)p + 28 + start, end - start);
			int order = key.compare(name);
			u64 value = _U64(p + valuesAt + (size_t)i * 8);
			if (leaf) {
				if (order == 0) {
					block = value;
					return true;
				}
				continue;
			}
			if (order >= 0) {
				next = value;
				break;
			}
		}
		if (leaf)
			return false;
		node = next;
	}
	return false;
}


bool
BfsReader::_Resolve(const std::string& path, Inode& inode,
	std::string& error, u64* resolvedBlock)
{
	u64 current = _BlockOf(fRoot);
	if (!_ReadInode(current, inode)) {
		error = "cannot read the root directory";
		return false;
	}
	size_t position = 0;
	while (position < path.size()) {
		size_t slash = path.find('/', position);
		if (slash == std::string::npos)
			slash = path.size();
		std::string name = path.substr(position, slash - position);
		position = slash + 1;
		if (name.empty())
			continue;
		if ((inode.mode & kTypeMask) != kDirectoryMode) {
			error = "not a directory before " + name;
			return false;
		}
		u64 block;
		if (!_Lookup(inode, name, block) || !_ReadInode(block, inode)) {
			error = name + " not found";
			return false;
		}
		current = block;
	}
	if (resolvedBlock != nullptr)
		*resolvedBlock = current;
	return true;
}


bool
BfsReader::LookupBlock(const std::string& path, u64& block,
	std::string& error)
{
	Inode inode;
	return _Resolve(path, inode, error, &block);
}


bool
BfsReader::ReadFile(const std::string& path, std::vector<u8>& data,
	std::string& error)
{
	Inode inode;
	if (!_Resolve(path, inode, error))
		return false;
	if ((inode.mode & kTypeMask) == kDirectoryMode) {
		error = path + " is a directory";
		return false;
	}
	if (inode.size > (64u << 20)) {
		error = path + " is too big";
		return false;
	}
	data.resize((size_t)inode.size);
	if (!_ReadStream(inode, 0, data.data(), data.size())) {
		error = "cannot read " + path;
		return false;
	}
	return true;
}


// Walks the leaves of a B+tree (directory or attribute directory) from the
// leftmost one along the right links: every key and value.
bool
BfsReader::_TreeEntries(const Inode& directory, RawEntries& entries)
{
	entries.clear();
	u8 header[32];
	if (!_ReadStream(directory, 0, header, sizeof(header))
		|| _U32(header) != kTreeMagic)
		return false;
	u32 nodeSize = _U32(header + 4);
	u64 node = _U64(header + 16);
	if (nodeSize < 64 || nodeSize > 65536)
		return false;
	std::vector<u8> data(nodeSize);
	// down to the leftmost leaf
	for (int depth = 0; ; depth++) {
		if (depth == 32 || node == kTreeNull
			|| !_ReadStream(directory, node, data.data(), nodeSize))
			return false;
		const u8* p = data.data();
		if (_U64(p + 16) == kTreeNull)
			break;
		u16 count = _U16(p + 24);
		u16 keyLength = _U16(p + 26);
		size_t valuesAt = ((28 + keyLength + 7) & ~(size_t)7)
			+ (size_t)count * 2;
		node = count > 0 && valuesAt + 8 <= nodeSize ? _U64(p + valuesAt)
			: _U64(p + 16);
	}
	for (int leaves = 0; leaves < 1000000; leaves++) {
		const u8* p = data.data();
		u16 count = _U16(p + 24);
		u16 keyLength = _U16(p + 26);
		size_t lengthsAt = (28 + keyLength + 7) & ~(size_t)7;
		size_t valuesAt = lengthsAt + (size_t)count * 2;
		if (valuesAt + (size_t)count * 8 > nodeSize)
			return false;
		for (u16 i = 0; i < count; i++) {
			u16 start = i == 0 ? 0 : _U16(p + lengthsAt + (i - 1) * 2);
			u16 end = _U16(p + lengthsAt + i * 2);
			if (end < start || 28u + end > nodeSize)
				return false;
			entries.push_back(std::make_pair(
				std::string((const char*)p + 28 + start, end - start),
				_U64(p + valuesAt + (size_t)i * 8)));
		}
		u64 right = _U64(p + 8);
		if (right == kTreeNull)
			return true;
		if (!_ReadStream(directory, right, data.data(), nodeSize))
			return false;
	}
	return false;
}


bool
BfsReader::ListDirectory(const std::string& path, std::vector<Entry>& entries,
	std::string& error)
{
	Inode directory;
	if (!_Resolve(path, directory, error))
		return false;
	if ((directory.mode & kTypeMask) != kDirectoryMode) {
		error = path + " is not a directory";
		return false;
	}
	RawEntries raw;
	if (!_TreeEntries(directory, raw)) {
		error = "bad directory tree in " + path;
		return false;
	}
	entries.clear();
	for (const auto& item : raw) {
		if (item.first == "." || item.first == "..")
			continue;
		Inode inode;
		if (!_ReadInode(item.second, inode))
			continue;
		Entry entry;
		entry.name = item.first;
		entry.directory = (inode.mode & kTypeMask) == kDirectoryMode;
		entry.size = inode.size;
		entries.push_back(entry);
	}
	return true;
}


bool
BfsReader::StatInode(u64 block, Stat& stat)
{
	Inode inode;
	if (!_ReadInode(block, inode))
		return false;
	stat.block = block;
	stat.mode = inode.mode;
	stat.directory = (inode.mode & kTypeMask) == kDirectoryMode;
	stat.size = inode.size;
	stat.created = inode.created;
	stat.modified = inode.modified;
	return true;
}


bool
BfsReader::ListBlock(u64 directoryBlock, RawEntries& entries)
{
	Inode directory;
	if (!_ReadInode(directoryBlock, directory)
		|| (directory.mode & kTypeMask) != kDirectoryMode
		|| !_TreeEntries(directory, entries))
		return false;
	entries.erase(std::remove_if(entries.begin(), entries.end(),
		[](const std::pair<std::string, u64>& item) {
			return item.first == "." || item.first == "..";
		}), entries.end());
	return true;
}


bool
BfsReader::ReadAt(u64 block, u64 offset, void* buffer, size_t length)
{
	Inode inode;
	return _ReadInode(block, inode) && offset + length <= inode.size
		&& _ReadStream(inode, offset, buffer, length);
}


// Attributes: small data entries in the inode (but the name), then the
// attribute directory's inodes.
bool
BfsReader::ReadAttributes(u64 block, std::vector<BfsAttribute>& attributes)
{
	attributes.clear();
	std::vector<u8> raw(fInodeSize);
	if (!_ReadBlocks(block, 1, raw.data()) || _U32(raw.data()) != kInodeMagic)
		return false;
	size_t position = kSmallDataStart;
	while (position + 8 <= raw.size()) {
		const u8* p = raw.data() + position;
		u32 type = _U32(p);
		u16 nameSize = _U16(p + 4);
		u16 dataSize = _U16(p + 6);
		if (type == 0 && nameSize == 0)
			break;
		size_t size = 8 + nameSize + 3 + dataSize + 1;
		if (nameSize == 0 || position + size > raw.size())
			break;
		std::string name((const char*)p + 8, nameSize);
		if (!(nameSize == 1 && name[0] == '\x13')) {
			BfsAttribute attribute;
			attribute.name = name;
			attribute.type = type;
			const u8* data = p + 8 + nameSize + 3;
			attribute.data.assign(data, data + dataSize);
			attributes.push_back(attribute);
		}
		position += size;
	}
	Inode inode;
	if (!_ReadInode(block, inode))
		return false;
	if (inode.attributes.length == 0)
		return true;
	Inode directory;
	RawEntries entries;
	if (!_ReadInode(_BlockOf(inode.attributes), directory)
		|| !_TreeEntries(directory, entries))
		return false;
	for (const auto& item : entries) {
		Inode attributeInode;
		if (!_ReadInode(item.second, attributeInode)
			|| attributeInode.size > (16u << 20))
			continue;
		BfsAttribute attribute;
		attribute.name = item.first;
		attribute.type = attributeInode.type;
		attribute.data.resize((size_t)attributeInode.size);
		if (!_ReadStream(attributeInode, 0, attribute.data.data(),
				attribute.data.size()))
			continue;
		attributes.push_back(attribute);
	}
	return true;
}


bool
BfsReader::_OpenImage(const std::string& image, ReadFunction& read,
	std::vector<u64>& candidates, std::string& error)
{
	std::shared_ptr<CdImage> cd;
	std::shared_ptr<DiskImage> disk;
	std::string lower = image;
	for (char& c : lower)
		c = (char)tolower((unsigned char)c);
	bool cdImage = lower.size() > 4
		&& (lower.compare(lower.size() - 4, 4, ".cue") == 0
			|| lower.compare(lower.size() - 4, 4, ".bin") == 0
			|| lower.compare(lower.size() - 4, 4, ".iso") == 0
			|| lower.compare(lower.size() - 4, 4, ".toast") == 0);
	if (cdImage) {
		cd = CdImage::Open(image, error);
		if (!cd)
			return false;
		for (const CdImage::Track& track : cd->Tracks()) {
			if (!track.audio)
				candidates.push_back((u64)track.start * 2048);
		}
		read = [cd](u64 offset, void* buffer, size_t length) {
			u8 sector[2048];
			u8* out = (u8*)buffer;
			while (length > 0) {
				u32 lba = (u32)(offset / 2048);
				u32 within = (u32)(offset % 2048);
				size_t chunk = std::min<size_t>(length, 2048 - within);
				if (!cd->ReadSector(lba, sector))
					return false;
				memcpy(out, sector + within, chunk);
				out += chunk;
				offset += chunk;
				length -= chunk;
			}
			return true;
		};
	} else {
		disk = DiskImage::Open(image, true, error);
		if (!disk)
			return false;
		read = [disk](u64 offset, void* buffer, size_t length) {
			return disk->Read(offset, buffer, length);
		};
	}
	return true;
}


bool
BfsReader::ExtractFromImage(const std::string& image, const std::string& path,
	std::vector<u8>& data, std::string& error)
{
	ReadFunction read;
	std::vector<u64> candidates;
	if (!_OpenImage(image, read, candidates, error))
		return false;
	BfsReader reader(read);
	std::vector<u64> volumes = reader.FindVolumes(candidates);
	if (volumes.empty()) {
		error = "no BFS volume found";
		return false;
	}
	for (u64 volume : volumes) {
		if (reader.Mount(volume) && reader.ReadFile(path, data, error))
			return true;
	}
	return false;
}



bool
BfsReader::ListFromImage(const std::string& image, const std::string& path,
	std::vector<Entry>& entries, std::string& error)
{
	ReadFunction read;
	std::vector<u64> candidates;
	if (!_OpenImage(image, read, candidates, error))
		return false;
	BfsReader reader(read);
	std::vector<u64> volumes = reader.FindVolumes(candidates);
	if (volumes.empty()) {
		error = "no BFS volume found";
		return false;
	}
	for (u64 volume : volumes) {
		if (reader.Mount(volume)
			&& reader.ListDirectory(path, entries, error))
			return true;
	}
	return false;
}

}	// namespace bebox
