/*
 * BeBox emulator - read only access to Be File System volumes.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Enough of BFS (big endian PowerPC volumes as well as little endian
 * ones) to read files by path from BeOS install CDs and disk images,
 * e.g. the boot ROM update /beos/etc/bebox_bootmain.image. Written from
 * the published description of the on-disk format (superblock, inodes,
 * data streams with direct, indirect and double indirect block runs,
 * B+tree directories).
 */
#ifndef BEBOX_BFS_READER_H
#define BEBOX_BFS_READER_H

#include "../Types.h"
#include "BfsWriter.h"

#include <functional>
#include <string>
#include <vector>

namespace bebox {

class BfsReader {
public:
	struct Entry {
		std::string	name;
		bool		directory;
		u64			size;
	};

	struct Stat {
		u64			block;
		bool		directory;
		u32			mode;
		u64			size;
		s64			created;		// BFS time (seconds << 16)
		s64			modified;
	};

	typedef std::vector<std::pair<std::string, u64> > RawEntries;

	// Reads 'length' bytes at a byte offset of the medium.
	typedef std::function<bool(u64 offset, void* buffer, size_t length)>
		ReadFunction;

								BfsReader(ReadFunction read);

			// Finds the BFS volumes: Apple partition map Be_BFS
			// partitions, a superblock at the start of the medium or of
			// one of the 'candidates' offsets (CD track starts). PowerPC
			// (big endian) volumes come first.
			std::vector<u64>	FindVolumes(const std::vector<u64>& candidates);
			bool				Mount(u64 volume);
			std::string			VolumeName() const { return fName; }

			bool				ReadFile(const std::string& path,
									std::vector<u8>& data,
									std::string& error);
			bool				ListDirectory(const std::string& path,
									std::vector<Entry>& entries,
									std::string& error);

			// Access by inode block (walking a whole volume).
			bool				IsClean() const { return fClean; }
			u64					RootBlock() const
									{ return _BlockOf(fRoot); }
			bool				StatInode(u64 block, Stat& stat);
			bool				LookupBlock(const std::string& path,
									u64& block, std::string& error);
			// Names and inode blocks, without "." and "..".
			bool				ListBlock(u64 directoryBlock,
									RawEntries& entries);
			bool				ReadAt(u64 block, u64 offset, void* buffer,
									size_t length);
			bool				ReadAttributes(u64 block,
									std::vector<BfsAttribute>& attributes);

	// Read from the BFS volumes of a CD or disk image file (ISO, BIN/CUE,
	// raw, QCOW2), the first volume that has the path.
	static	bool				ExtractFromImage(const std::string& image,
									const std::string& path,
									std::vector<u8>& data,
									std::string& error);
	static	bool				ListFromImage(const std::string& image,
									const std::string& path,
									std::vector<Entry>& entries,
									std::string& error);

private:
	struct BlockRun {
		u32 group;
		u16 start;
		u16 length;
	};

	struct Inode {
		u32 mode;
		u32 flags;
		s64 created;
		s64 modified;
		BlockRun parent;
		BlockRun attributes;
		u32 type;
		u64 size;
		BlockRun direct[12];
		u64 maxDirectRange;
		BlockRun indirect;
		u64 maxIndirectRange;
		BlockRun doubleIndirect;
		u64 maxDoubleIndirectRange;
	};

			bool				_TryVolume(u64 offset);
			u16					_U16(const u8* p) const;
			u32					_U32(const u8* p) const;
			u64					_U64(const u8* p) const;
			BlockRun			_Run(const u8* p) const;
			u64					_BlockOf(const BlockRun& run) const
									{ return ((u64)run.group << fAgShift)
										+ run.start; }
			bool				_ReadBlocks(u64 block, u32 count,
									void* buffer);
			bool				_ReadInode(u64 block, Inode& inode);
			bool				_ReadStream(const Inode& inode, u64 offset,
									void* buffer, size_t length);
			bool				_FindBlock(const Inode& inode, u64 offset,
									u64& block);
			bool				_TreeEntries(const Inode& directory,
									RawEntries& entries);
			bool				_Lookup(const Inode& directory,
									const std::string& name, u64& block);
			bool				_Resolve(const std::string& path,
									Inode& inode, std::string& error,
									u64* block = nullptr);
	static	bool				_OpenImage(const std::string& image,
									ReadFunction& read,
									std::vector<u64>& candidates,
									std::string& error);

			ReadFunction		fRead;
			u64					fVolume;
			bool				fBigEndian;
			u32					fBlockSize;
			u32					fAgShift;
			u32					fInodeSize;
			BlockRun			fRoot;
			std::string			fName;
			bool				fClean = false;
};

}	// namespace bebox

#endif	// BEBOX_BFS_READER_H
