/*
 * BeBox emulator - writes Be File System volumes.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Creates a big endian (PowerPC) BFS volume holding a tree of files,
 * directories and attributes, in the layout BeOS's own mkbfs uses: the
 * superblock in block 0, the block bitmap, the log, then inodes and data
 * in allocation groups; directories and attribute directories are B+trees
 * of 1 KB nodes; small attributes live in the inode, the others in an
 * attribute directory. The volume has no index directory (no queries).
 */
#ifndef BEBOX_BFS_WRITER_H
#define BEBOX_BFS_WRITER_H

#include "../Types.h"

#include <functional>
#include <string>
#include <vector>

namespace bebox {

struct BfsAttribute {
	std::string			name;
	u32					type;
	std::vector<u8>		data;		// in the volume's (big endian) layout
};

class BfsWriter {
public:
	struct Node {
		std::string			name;
		bool				directory = false;
		u32					permissions = 0644;
		s64					created = 0;		// seconds since 1970
		s64					modified = 0;
		u64					size = 0;			// files
		// Reads file content (files).
		std::function<bool(u64 offset, void* buffer, size_t length)> read;
		std::vector<BfsAttribute> attributes;
		std::vector<Node>	children;			// directories

		// set by the writer
		u64					inodeBlock = 0;
		s64					storedModified = 0;	// as written (BFS time)
	};

	typedef std::function<bool(u64 offset, const void* buffer,
		size_t length)> WriteFunction;

	static const u32 kBlockSize = 2048;

	// Writes a volume of 'volumeSize' bytes named 'name' with the children
	// of 'root' at its root. Fills in each node's inode block.
	static	bool				Write(WriteFunction write, u64 volumeSize,
									const std::string& name, Node& root,
									std::string& error);

	// BFS stores times as seconds << 16 plus a counter.
	static	s64					ToBfsTime(s64 seconds, u32 counter = 0)
									{ return (seconds << 16)
										| (counter & 0xffff); }
	static	s64					FromBfsTime(s64 time)
									{ return time >> 16; }
};

}	// namespace bebox

#endif	// BEBOX_BFS_WRITER_H
