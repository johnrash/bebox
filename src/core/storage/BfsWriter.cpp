/*
 * BeBox emulator - writes Be File System volumes.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The on-disk format as BeOS R4.5 writes it (checked against volumes made
 * by its installer): superblock fields, inode layout (data stream at 72,
 * small data at 232), small data entries (8 byte header, name, 3 bytes,
 * data, 1 byte), B+tree nodes (24 byte links, count and key length, keys
 * padded to 8, key end offsets, values), directory modes with the string
 * index bit, attribute directories and attribute inodes.
 */
#include "BfsWriter.h"

#include <algorithm>
#include <cstring>

namespace bebox {

namespace {

const u32 kSuperMagic1 = 0x42465331;	// "BFS1"
const u32 kBigEndian = 0x42494745;		// "BIGE"
const u32 kSuperMagic2 = 0xdd121031;
const u32 kSuperMagic3 = 0x15b6830e;
const u32 kClean = 0x434c454e;			// "CLEN"
const u32 kInodeMagic = 0x3bbe0ad9;
const u32 kTreeMagic = 0x69f6c2e8;
const u32 kNodeSize = 1024;
const s64 kNullNode = -1;

const u32 kTypeDirectory = 0040000;
const u32 kTypeFile = 0100000;
const u32 kStringIndex = 0x01000000;	// directories: string keyed tree
const u32 kAttributeDirectory = 0x08000000;
const u32 kAttribute = 0x10000000;
const u32 kIndexDirectory = 0x20000000;
const u32 kInUse = 0x01;
const u32 kLogged = 0x08;

const u32 kBlockSize = BfsWriter::kBlockSize;
const u32 kAgShift = 14;				// one bitmap block per group
const u32 kAgBlocks = 1u << kAgShift;
const u32 kLogBlocks = 1024;
const u32 kArrayBlocks = 4;				// indirect run arrays
const u32 kInodeSmallData = 232;

struct Run {
	u32 group = 0;
	u16 start = 0;
	u16 length = 0;
};


void
Put16(u8* p, u16 value)
{
	p[0] = (u8)(value >> 8);
	p[1] = (u8)value;
}


void
Put32(u8* p, u32 value)
{
	p[0] = (u8)(value >> 24);
	p[1] = (u8)(value >> 16);
	p[2] = (u8)(value >> 8);
	p[3] = (u8)value;
}


void
Put64(u8* p, u64 value)
{
	Put32(p, (u32)(value >> 32));
	Put32(p + 4, (u32)value);
}


void
PutRun(u8* p, const Run& run)
{
	Put32(p, run.group);
	Put16(p + 4, run.start);
	Put16(p + 6, run.length);
}


Run
RunFor(u64 block, u32 length)
{
	Run run;
	run.group = (u32)(block >> kAgShift);
	run.start = (u16)(block & (kAgBlocks - 1));
	run.length = (u16)length;
	return run;
}


u64
BlockOf(const Run& run)
{
	return ((u64)run.group << kAgShift) + run.start;
}


// Keys of B+tree entries compare as unsigned bytes, a prefix first.
bool
KeyLess(const std::string& a, const std::string& b)
{
	int result = memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
	if (result != 0)
		return result < 0;
	return a.size() < b.size();
}


class Writer {
public:
	Writer(BfsWriter::WriteFunction write, u64 blocks)
		:
		fWrite(write),
		fBlocks(blocks),
		fBitmap((size_t)((blocks + 7) / 8), 0),
		fNext(0)
	{
	}

	bool Format(const std::string& name, BfsWriter::Node& root,
		std::string& error);

private:
	typedef std::vector<std::pair<std::string, u64> > Entries;

	bool _Allocate(u32 count, u64& block);
	bool _AllocateRuns(u64 blocks, std::vector<Run>& runs);
	void _Mark(u64 block, u64 count);
	bool _WriteAt(u64 block, const void* data, size_t bytes)
		{ return fWrite(block * kBlockSize, data, bytes); }
	bool _WriteStream(const std::vector<Run>& runs, const u8* data,
		size_t size);
	bool _FillStream(u8* inode, const std::vector<Run>& runs, u64 size);
	std::vector<u8> _BuildTree(Entries entries, bool sorted);
	bool _WriteTree(const Entries& entries, std::vector<Run>& runs,
		u64& size);
	bool _WriteNode(BfsWriter::Node& node, u64 parent, bool isRoot);
	bool _WriteAttributes(std::vector<BfsAttribute>& big, u64 owner,
		u64& directory);
	void _InitInode(u8* inode, u64 block, u32 mode, u32 flags, s64 created,
		s64 modified, u64 parent);

	BfsWriter::WriteFunction fWrite;
	u64 fBlocks;
	std::vector<u8> fBitmap;
	u64 fNext;
	u32 fTimeCounter = 0;
	std::string fError;
};


void
Writer::_Mark(u64 block, u64 count)
{
	for (u64 i = block; i < block + count && i < fBlocks; i++)
		fBitmap[i / 8] |= (u8)(1 << (i % 8));
}


// A contiguous run inside one allocation group.
bool
Writer::_Allocate(u32 count, u64& block)
{
	if ((fNext & (kAgBlocks - 1)) + count > kAgBlocks)
		fNext = (fNext + kAgBlocks) & ~(u64)(kAgBlocks - 1);
	if (fNext + count > fBlocks) {
		fError = "the shared folder does not fit the volume";
		return false;
	}
	block = fNext;
	fNext += count;
	_Mark(block, count);
	return true;
}


bool
Writer::_AllocateRuns(u64 blocks, std::vector<Run>& runs)
{
	runs.clear();
	while (blocks > 0) {
		u64 room = kAgBlocks - (fNext & (kAgBlocks - 1));
		u32 take = (u32)std::min<u64>(std::min<u64>(blocks, room), 65535);
		u64 block;
		if (!_Allocate(take, block))
			return false;
		runs.push_back(RunFor(block, take));
		blocks -= take;
	}
	return true;
}


bool
Writer::_WriteStream(const std::vector<Run>& runs, const u8* data,
	size_t size)
{
	size_t done = 0;
	for (const Run& run : runs) {
		size_t bytes = std::min<size_t>(size - done,
			(size_t)run.length * kBlockSize);
		if (bytes == 0)
			break;
		if (!_WriteAt(BlockOf(run), data + done, bytes))
			return false;
		done += bytes;
	}
	return true;
}


// The data stream: twelve direct runs, then an indirect array of runs.
bool
Writer::_FillStream(u8* inode, const std::vector<Run>& runs, u64 size)
{
	u64 direct = 0;
	size_t i = 0;
	for (; i < runs.size() && i < 12; i++) {
		PutRun(inode + 72 + i * 8, runs[i]);
		direct += (u64)runs[i].length * kBlockSize;
	}
	Put64(inode + 168, direct);
	if (i < runs.size()) {
		size_t capacity = kArrayBlocks * kBlockSize / 8;
		if (runs.size() - i > capacity) {
			fError = "a file is too fragmented for the volume";
			return false;
		}
		u64 arrayBlock;
		if (!_Allocate(kArrayBlocks, arrayBlock))
			return false;
		std::vector<u8> array(kArrayBlocks * kBlockSize, 0);
		u64 indirect = direct;
		for (size_t j = 0; i < runs.size(); i++, j++) {
			PutRun(&array[j * 8], runs[i]);
			indirect += (u64)runs[i].length * kBlockSize;
		}
		if (!_WriteAt(arrayBlock, array.data(), array.size()))
			return false;
		PutRun(inode + 176, RunFor(arrayBlock, kArrayBlocks));
		Put64(inode + 184, indirect);
	}
	Put64(inode + 208, size);
	return true;
}


void
Writer::_InitInode(u8* inode, u64 block, u32 mode, u32 flags, s64 created,
	s64 modified, u64 parent)
{
	memset(inode, 0, kBlockSize);
	Put32(inode, kInodeMagic);
	PutRun(inode + 4, RunFor(block, 1));
	Put32(inode + 20, mode);
	Put32(inode + 24, flags);
	Put64(inode + 28, (u64)BfsWriter::ToBfsTime(created, fTimeCounter++));
	Put64(inode + 36, (u64)modified);
	PutRun(inode + 44, RunFor(parent, 1));
	Put32(inode + 64, kBlockSize);			// inode size
}


// A B+tree of 1 KB nodes: the header node, the leaves (linked both ways),
// interior levels whose entries hold each child's last key and whose
// overflow link is the last child.
std::vector<u8>
Writer::_BuildTree(Entries entries, bool sorted)
{
	if (!sorted) {
		std::sort(entries.begin(), entries.end(),
			[](const std::pair<std::string, u64>& a,
				const std::pair<std::string, u64>& b) {
				return KeyLess(a.first, b.first);
			});
	}
	struct TreeNode {
		Entries entries;
		s64 overflow = kNullNode;
		std::string lastKey;
		u32 offset = 0;
	};
	auto fits = [](size_t count, size_t keyBytes) {
		return ((28 + keyBytes + 7) & ~(size_t)7) + count * 10 <= kNodeSize;
	};

	std::vector<std::vector<TreeNode> > levels(1);
	levels[0].push_back(TreeNode());
	size_t keyBytes = 0;
	for (const auto& entry : entries) {
		TreeNode* leaf = &levels[0].back();
		if (!fits(leaf->entries.size() + 1, keyBytes + entry.first.size())
			&& !leaf->entries.empty()) {
			levels[0].push_back(TreeNode());
			leaf = &levels[0].back();
			keyBytes = 0;
		}
		leaf->entries.push_back(entry);
		leaf->lastKey = entry.first;
		keyBytes += entry.first.size();
	}

	// node offsets: leaves first, then each level above
	u32 offset = kNodeSize;
	for (TreeNode& node : levels[0]) {
		node.offset = offset;
		offset += kNodeSize;
	}
	while (levels.back().size() > 1) {
		std::vector<TreeNode>& children = levels.back();
		// group the children: each group's last child is the overflow
		// link, the others become entries
		std::vector<std::vector<const TreeNode*> > groups(1);
		size_t groupKeys = 0;
		for (const TreeNode& child : children) {
			std::vector<const TreeNode*>& group = groups.back();
			if (!group.empty() && !fits(group.size(),
					groupKeys + group.back()->lastKey.size())) {
				groups.push_back(std::vector<const TreeNode*>());
				groupKeys = 0;
			} else if (!group.empty())
				groupKeys += group.back()->lastKey.size();
			groups.back().push_back(&child);
		}
		// no interior node with only an overflow link
		if (groups.size() > 1 && groups.back().size() == 1) {
			std::vector<const TreeNode*>& previous
				= groups[groups.size() - 2];
			groups.back().insert(groups.back().begin(), previous.back());
			previous.pop_back();
		}
		std::vector<TreeNode> parents;
		for (const std::vector<const TreeNode*>& group : groups) {
			TreeNode parent;
			for (size_t i = 0; i + 1 < group.size(); i++) {
				parent.entries.push_back(std::make_pair(group[i]->lastKey,
					(u64)group[i]->offset));
			}
			parent.overflow = group.back()->offset;
			parent.lastKey = group.back()->lastKey;
			parent.offset = offset;
			offset += kNodeSize;
			parents.push_back(parent);
		}
		levels.push_back(parents);
	}

	std::vector<u8> data(offset, 0);
	u8* header = data.data();
	Put32(header, kTreeMagic);
	Put32(header + 4, kNodeSize);
	Put32(header + 8, (u32)levels.size());
	Put32(header + 12, 0);					// string keys
	Put64(header + 16, levels.back()[0].offset);
	Put64(header + 24, (u64)kNullNode);		// no free nodes
	Put64(header + 32, offset);
	for (std::vector<TreeNode>& level : levels) {
		for (size_t i = 0; i < level.size(); i++) {
			TreeNode& node = level[i];
			u8* p = &data[node.offset];
			Put64(p, i > 0 ? level[i - 1].offset : (u64)kNullNode);
			Put64(p + 8, i + 1 < level.size() ? level[i + 1].offset
				: (u64)kNullNode);
			Put64(p + 16, (u64)node.overflow);
			size_t keys = 0;
			for (const auto& entry : node.entries)
				keys += entry.first.size();
			Put16(p + 24, (u16)node.entries.size());
			Put16(p + 26, (u16)keys);
			size_t position = 28;
			size_t lengths = (28 + keys + 7) & ~(size_t)7;
			size_t values = lengths + node.entries.size() * 2;
			u16 end = 0;
			for (size_t j = 0; j < node.entries.size(); j++) {
				const auto& entry = node.entries[j];
				memcpy(p + position, entry.first.data(), entry.first.size());
				position += entry.first.size();
				end = (u16)(end + entry.first.size());
				Put16(p + lengths + j * 2, end);
				Put64(p + values + j * 8, entry.second);
			}
		}
	}
	return data;
}


bool
Writer::_WriteTree(const Entries& entries, std::vector<Run>& runs, u64& size)
{
	std::vector<u8> tree = _BuildTree(entries, false);
	size = tree.size();
	if (!_AllocateRuns((tree.size() + kBlockSize - 1) / kBlockSize, runs))
		return false;
	tree.resize(((tree.size() + kBlockSize - 1) / kBlockSize) * kBlockSize,
		0);
	return _WriteStream(runs, tree.data(), tree.size());
}


// Attributes that do not fit in the inode: an attribute directory of
// attribute inodes.
bool
Writer::_WriteAttributes(std::vector<BfsAttribute>& big, u64 owner,
	u64& directory)
{
	if (!_Allocate(1, directory))
		return false;
	Entries entries;
	for (BfsAttribute& attribute : big) {
		u64 block;
		if (!_Allocate(1, block))
			return false;
		std::vector<Run> runs;
		if (!_AllocateRuns((attribute.data.size() + kBlockSize - 1)
				/ kBlockSize, runs)
			|| !_WriteStream(runs, attribute.data.data(),
				attribute.data.size()))
			return false;
		std::vector<u8> inode(kBlockSize);
		s64 now = BfsWriter::ToBfsTime(0);
		_InitInode(inode.data(), block, kAttribute | kTypeFile | 0666, kInUse,
			0, now, directory);
		Put32(inode.data() + 60, attribute.type);
		if (!_FillStream(inode.data(), runs, attribute.data.size())
			|| !_WriteAt(block, inode.data(), kBlockSize))
			return false;
		entries.push_back(std::make_pair(attribute.name, block));
	}
	std::vector<Run> runs;
	u64 size;
	if (!_WriteTree(entries, runs, size))
		return false;
	std::vector<u8> inode(kBlockSize);
	_InitInode(inode.data(), directory, kAttributeDirectory | kStringIndex
		| kTypeDirectory | 0666, kInUse | kLogged, 0, BfsWriter::ToBfsTime(0),
		owner);
	return _FillStream(inode.data(), runs, size)
		&& _WriteAt(directory, inode.data(), kBlockSize);
}


bool
Writer::_WriteNode(BfsWriter::Node& node, u64 parent, bool isRoot)
{
	u64 block;
	if (!_Allocate(1, block))
		return false;
	node.inodeBlock = block;
	if (isRoot)
		parent = block;

	std::vector<Run> runs;
	u64 size = 0;
	if (node.directory) {
		Entries entries;
		entries.push_back(std::make_pair(std::string("."), block));
		entries.push_back(std::make_pair(std::string(".."), parent));
		for (BfsWriter::Node& child : node.children) {
			if (!_WriteNode(child, block, false))
				return false;
			entries.push_back(std::make_pair(child.name, child.inodeBlock));
		}
		if (!_WriteTree(entries, runs, size))
			return false;
	} else {
		size = node.size;
		if (!_AllocateRuns((size + kBlockSize - 1) / kBlockSize, runs))
			return false;
		// copy the content a run at a time, in pieces of 1 MB
		const size_t kPiece = 1 << 20;
		std::vector<u8> buffer(kPiece);
		u64 done = 0;
		for (const Run& run : runs) {
			u64 runBytes = std::min<u64>(size - done,
				(u64)run.length * kBlockSize);
			for (u64 at = 0; at < runBytes; at += kPiece) {
				size_t piece = (size_t)std::min<u64>(kPiece, runBytes - at);
				if (!node.read || !node.read(done + at, buffer.data(), piece)) {
					fError = "cannot read " + node.name;
					return false;
				}
				// whole blocks (the tail of the last one zeroed)
				size_t padded = (piece + kBlockSize - 1) / kBlockSize
					* kBlockSize;
				memset(buffer.data() + piece, 0, padded - piece);
				if (!_WriteAt(BlockOf(run) + at / kBlockSize, buffer.data(),
						padded))
					return false;
			}
			done += runBytes;
		}
	}

	std::vector<u8> inode(kBlockSize);
	u32 mode = node.directory
		? kTypeDirectory | kStringIndex | (node.permissions & 07777)
		: kTypeFile | (node.permissions & 07777);
	node.storedModified = BfsWriter::ToBfsTime(node.modified,
		fTimeCounter++);
	_InitInode(inode.data(), block, mode,
		node.directory ? kInUse | kLogged : kInUse, node.created,
		node.storedModified, parent);
	if (!_FillStream(inode.data(), runs, size))
		return false;

	// small data: the name, then the attributes that fit
	size_t position = kInodeSmallData;
	auto add = [&inode, &position](u32 type, const std::string& name,
			const u8* data, size_t length) {
		size_t need = 8 + name.size() + 3 + length + 1;
		// keep room for the terminating empty entry
		if (position + need + 8 > kBlockSize || name.size() > 0xffff
			|| length > 0xffff)
			return false;
		u8* p = inode.data() + position;
		Put32(p, type);
		Put16(p + 4, (u16)name.size());
		Put16(p + 6, (u16)length);
		memcpy(p + 8, name.data(), name.size());
		memcpy(p + 8 + name.size() + 3, data, length);
		position += need;
		return true;
	};
	if (!isRoot) {
		add(0x43535452, "\x13", (const u8*)node.name.data(),	// 'CSTR'
			node.name.size());
	}
	std::vector<BfsAttribute> big;
	for (BfsAttribute& attribute : node.attributes) {
		if (!add(attribute.type, attribute.name, attribute.data.data(),
				attribute.data.size()))
			big.push_back(attribute);
	}
	if (!big.empty()) {
		u64 directory;
		if (!_WriteAttributes(big, block, directory))
			return false;
		PutRun(inode.data() + 52, RunFor(directory, 1));
	}
	return _WriteAt(block, inode.data(), kBlockSize);
}


bool
Writer::Format(const std::string& name, BfsWriter::Node& root,
	std::string& error)
{
	u64 bitmapBlocks = (fBlocks + kAgBlocks - 1) / kAgBlocks;
	u64 groups = bitmapBlocks;
	u64 logStart = 1 + bitmapBlocks;
	if (fBlocks < 4096 || logStart + kLogBlocks + 64 > kAgBlocks) {
		error = "unsupported volume size";
		return false;
	}
	_Mark(0, logStart + kLogBlocks);
	fNext = logStart + kLogBlocks;

	// an empty index directory (no indices: no queries on this volume)
	u64 indexBlock;
	if (!_Allocate(1, indexBlock)) {
		error = fError;
		return false;
	}
	std::vector<Run> indexRuns;
	u64 indexSize;
	if (!_WriteTree(Entries(), indexRuns, indexSize)) {
		error = fError;
		return false;
	}
	std::vector<u8> indexInode(kBlockSize);
	s64 now = BfsWriter::ToBfsTime(root.modified);
	_InitInode(indexInode.data(), indexBlock, kIndexDirectory | kStringIndex
		| kTypeDirectory | 0700, kInUse | kLogged, root.modified, now,
		indexBlock);
	if (!_FillStream(indexInode.data(), indexRuns, indexSize)
		|| !_WriteAt(indexBlock, indexInode.data(), kBlockSize)) {
		error = fError.empty() ? "cannot write the volume" : fError;
		return false;
	}

	if (!_WriteNode(root, 0, true)) {
		error = fError.empty() ? "cannot write the volume" : fError;
		return false;
	}

	// block bitmap: 32-bit words in volume byte order, bit n = block n % 32
	std::vector<u8> bitmap(bitmapBlocks * kBlockSize, 0);
	u64 used = 0;
	for (u64 block = 0; block < fBlocks; block++) {
		if ((fBitmap[block / 8] & (1 << (block % 8))) == 0)
			continue;
		used++;
		u8* word = &bitmap[(block / 32) * 4];
		u32 bit = (u32)(block % 32);
		word[3 - bit / 8] |= (u8)(1 << (bit % 8));
	}
	if (!_WriteAt(1, bitmap.data(), bitmap.size())) {
		error = "cannot write the volume";
		return false;
	}

	std::vector<u8> super(kBlockSize, 0);
	u8* s = super.data();
	strncpy((char*)s, name.c_str(), 31);
	Put32(s + 32, kSuperMagic1);
	Put32(s + 36, kBigEndian);
	Put32(s + 40, kBlockSize);
	Put32(s + 44, 11);						// block shift
	Put64(s + 48, fBlocks);
	Put64(s + 56, used);
	Put32(s + 64, kBlockSize);				// inode size
	Put32(s + 68, kSuperMagic2);
	Put32(s + 72, 1);						// bitmap blocks per group
	Put32(s + 76, kAgShift);
	Put32(s + 80, (u32)groups);
	Put32(s + 84, kClean);
	PutRun(s + 88, RunFor(logStart, kLogBlocks));
	Put64(s + 96, 0);						// log start, end: empty
	Put64(s + 104, 0);
	Put32(s + 112, kSuperMagic3);
	PutRun(s + 116, RunFor(root.inodeBlock, 1));
	PutRun(s + 124, RunFor(indexBlock, 1));
	if (!_WriteAt(0, super.data(), super.size())) {
		error = "cannot write the volume";
		return false;
	}
	return true;
}

}	// namespace


bool
BfsWriter::Write(WriteFunction write, u64 volumeSize, const std::string& name,
	Node& root, std::string& error)
{
	Writer writer(write, volumeSize / kBlockSize);
	return writer.Format(name, root, error);
}

}	// namespace bebox
