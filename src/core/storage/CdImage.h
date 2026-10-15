/*
 * BeBox emulator - CD-ROM images.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * ISO images (2048 byte sectors, also Toast and raw BeOS volumes) and
 * BIN/CUE images with MODE1/2048, MODE1/2352, MODE2/2352 and AUDIO tracks,
 * possibly spread over several files.
 */
#ifndef BEBOX_CD_IMAGE_H
#define BEBOX_CD_IMAGE_H

#include "../Types.h"

#include <memory>
#include <string>
#include <vector>

namespace bebox {

class DiskImage;

class CdImage {
public:
	struct Track {
		int number = 1;
		bool audio = false;
		u32 start = 0;			// first LBA on the disc
		u32 length = 0;			// sectors
		int fileIndex = 0;
		u64 fileOffset = 0;		// byte offset of 'start' in the file
		u32 sectorSize = 2048;	// 2048 or 2352
		u32 dataOffset = 0;		// user data offset in a raw sector
	};

								~CdImage();

	static	std::unique_ptr<CdImage> Open(const std::string& path,
									std::string& error);

			u32					Sectors() const { return fSectors; }
			const std::vector<Track>& Tracks() const { return fTracks; }
			const Track*		TrackAt(u32 lba) const;
			std::string			Path() const { return fPath; }

			// 2048 bytes of user data (data tracks).
			bool				ReadSector(u32 lba, u8* buffer);
			// 2352 bytes of audio (audio tracks).
			bool				ReadAudio(u32 lba, u8* buffer);

private:
								CdImage() {}

			bool				_ParseCue(const std::string& path,
									std::string& error);

			std::vector<std::unique_ptr<DiskImage> > fFiles;
			std::vector<Track>	fTracks;
			u32					fSectors = 0;
			std::string			fPath;
};

}	// namespace bebox

#endif	// BEBOX_CD_IMAGE_H
