/*
 * BeBox emulator - the Be file system of the Hobbit prototypes (reading).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The 1993/94 Be file system (version 1.0, not the later BFS): 512 byte
 * sectors, all values big endian.
 *
 * Sector 0, the volume header: 0x00 version (0x00010000), 0x04 creation
 * time, 0x08 first bitmap sector, 0x0c bitmap sectors, 0x10 first sector
 * of the root directory, 0x14 total sectors, 0x18 bytes per sector, 0x2c
 * the volume name (32 bytes).
 *
 * Directories are chains of 4 KB blocks (8 sectors): a 64 byte header
 * whose first word is the sector of the next block (0: the last one), then
 * 63 entries of 64 bytes: name (32 bytes), first sector, sector count,
 * type (0xffffffff: a directory), creation and modification times,
 * logical and physical size, creator (times in seconds since 1970). An
 * entry whose first sector is 0 is free; one whose name starts with a 0 byte
 * is deleted (the kernel's Delete clears it and queues the entry).
 *
 * File data: a first sector with bit 31 set means contiguous sectors from
 * there; otherwise the first sector is a map sector: next map sector,
 * first logical sector, then up to 63 (sector, count) extents, ended by
 * a sector of 0xffffffff (empty extents are skipped).
 */
#ifndef BEBOX_HOBBIT_VOLUME_H
#define BEBOX_HOBBIT_VOLUME_H

#include "../Types.h"

#include <string>
#include <vector>

namespace bebox {

class DiskImage;

class HobbitVolume {
public:
	struct Entry {
		std::string	name;
		u32			first;
		u32			count;
		u32			type;
		u32			created;
		u32			modified;
		u32			size;
		u32			creator;
		bool		IsDirectory() const { return type == 0xffffffff; }
	};

	explicit					HobbitVolume(DiskImage& image);

			// Checks the volume header.
			bool				Mount(std::string& error);
			const std::string&	Name() const { return fName; }

			// The live entries of a directory (not the deleted ones).
			bool				ReadDirectory(u32 sector,
									std::vector<Entry>& entries);
			bool				Lookup(const std::string& path, Entry& entry,
									std::string& error);
			bool				ReadFile(const Entry& entry,
									std::vector<u8>& data,
									std::string& error);
			bool				ReadFile(const std::string& path,
									std::vector<u8>& data,
									std::string& error);

private:
			bool				_ReadSectors(u32 sector, u32 count,
									void* buffer);

			DiskImage&			fImage;
			std::string			fName;
			u32					fRoot;
			u32					fTotalSectors;
};

}	// namespace bebox

#endif	// BEBOX_HOBBIT_VOLUME_H
