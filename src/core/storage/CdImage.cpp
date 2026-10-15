/*
 * BeBox emulator - CD-ROM images.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "CdImage.h"

#include "DiskImage.h"
#include "../Log.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <strings.h>

namespace bebox {

CdImage::~CdImage()
{
}


static bool
EndsWith(const std::string& text, const char* suffix)
{
	size_t length = strlen(suffix);
	return text.size() >= length
		&& strcasecmp(text.c_str() + text.size() - length, suffix) == 0;
}


std::unique_ptr<CdImage>
CdImage::Open(const std::string& path, std::string& error)
{
	std::unique_ptr<CdImage> image(new CdImage);
	image->fPath = path;
	if (EndsWith(path, ".cue")) {
		if (!image->_ParseCue(path, error))
			return nullptr;
		return image;
	}

	std::unique_ptr<DiskImage> file = DiskImage::Open(path, true, error);
	if (!file)
		return nullptr;
	Track track;
	track.number = 1;
	track.start = 0;
	u64 size = file->Size();
	// A .bin without cue sheet: guess raw sectors from the sync pattern.
	if (EndsWith(path, ".bin") && size % 2352 == 0) {
		u8 sync[12];
		static const u8 kSync[12] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0xff, 0 };
		if (file->Read(0, sync, 12) && memcmp(sync, kSync, 12) == 0) {
			track.sectorSize = 2352;
			track.dataOffset = 16;
		}
	}
	track.length = (u32)(size / track.sectorSize);
	image->fSectors = track.length;
	image->fFiles.push_back(std::move(file));
	image->fTracks.push_back(track);
	return image;
}


bool
CdImage::_ParseCue(const std::string& path, std::string& error)
{
	FILE* cue = fopen(path.c_str(), "r");
	if (cue == nullptr) {
		error = "cannot open " + path;
		return false;
	}
	std::string directory;
	size_t slash = path.rfind('/');
	if (slash != std::string::npos)
		directory = path.substr(0, slash + 1);

	struct Pending {
		Track track;
		u32 index1 = 0;			// in sectors relative to the file
		bool hasIndex = false;
	};
	std::vector<Pending> pending;
	char line[1024];
	while (fgets(line, sizeof(line), cue) != nullptr) {
		std::istringstream stream(line);
		std::string keyword;
		stream >> keyword;
		for (char& c : keyword)
			c = (char)toupper((unsigned char)c);
		if (keyword == "FILE") {
			std::string rest;
			std::getline(stream, rest);
			size_t first = rest.find('"');
			size_t last = rest.rfind('"');
			std::string name;
			if (first != std::string::npos && last > first)
				name = rest.substr(first + 1, last - first - 1);
			else {
				std::istringstream words(rest);
				words >> name;
			}
			std::string filePath = name[0] == '/' ? name : directory + name;
			std::unique_ptr<DiskImage> file = DiskImage::Open(filePath, true,
				error);
			if (!file) {
				fclose(cue);
				return false;
			}
			fFiles.push_back(std::move(file));
		} else if (keyword == "TRACK") {
			Pending entry;
			std::string mode;
			stream >> entry.track.number >> mode;
			for (char& c : mode)
				c = (char)toupper((unsigned char)c);
			entry.track.fileIndex = (int)fFiles.size() - 1;
			if (mode == "AUDIO") {
				entry.track.audio = true;
				entry.track.sectorSize = 2352;
			} else if (mode == "MODE1/2048") {
				entry.track.sectorSize = 2048;
			} else if (mode == "MODE1/2352") {
				entry.track.sectorSize = 2352;
				entry.track.dataOffset = 16;
			} else if (mode == "MODE2/2352") {
				entry.track.sectorSize = 2352;
				entry.track.dataOffset = 24;	// XA form 1
			} else if (mode == "MODE2/2336") {
				entry.track.sectorSize = 2336;
				entry.track.dataOffset = 8;
			} else {
				error = "unsupported track mode " + mode;
				fclose(cue);
				return false;
			}
			pending.push_back(entry);
		} else if (keyword == "INDEX" && !pending.empty()) {
			int number;
			std::string time;
			stream >> number >> time;
			int minutes = 0, seconds = 0, frames = 0;
			sscanf(time.c_str(), "%d:%d:%d", &minutes, &seconds, &frames);
			u32 sector = (u32)((minutes * 60 + seconds) * 75 + frames);
			if (number == 1) {
				pending.back().index1 = sector;
				pending.back().hasIndex = true;
			}
		}
	}
	fclose(cue);
	if (fFiles.empty() || pending.empty()) {
		error = "empty cue sheet";
		return false;
	}

	// Lay the tracks out: within a file by their INDEX 01, files one after
	// the other.
	u32 discPosition = 0;
	for (size_t i = 0; i < pending.size(); i++) {
		Pending& entry = pending[i];
		Track& track = entry.track;
		DiskImage* file = fFiles[track.fileIndex].get();
		bool firstInFile = i == 0
			|| pending[i - 1].track.fileIndex != track.fileIndex;
		if (firstInFile)
			track.fileOffset = (u64)entry.index1 * track.sectorSize;
		else {
			// relative to the previous track's file offset
			const Pending& previous = pending[i - 1];
			track.fileOffset = previous.track.fileOffset
				+ (u64)(entry.index1 - previous.index1)
					* previous.track.sectorSize;
		}
		bool lastInFile = i + 1 == pending.size()
			|| pending[i + 1].track.fileIndex != track.fileIndex;
		u64 end = lastInFile ? file->Size()
			: (u64)pending[i + 1].index1 * track.sectorSize;
		if (!lastInFile) {
			end = track.fileOffset + (u64)(pending[i + 1].index1
				- entry.index1) * track.sectorSize;
		}
		track.length = (u32)((end - track.fileOffset) / track.sectorSize);
		track.start = discPosition;
		if (!firstInFile)
			track.start = fTracks.back().start + fTracks.back().length;
		discPosition = track.start + track.length;
		fTracks.push_back(track);
	}
	fSectors = discPosition;
	BB_LOG(kLogStorage, kLogInfo, "%s: %zu tracks, %u sectors", path.c_str(),
		fTracks.size(), fSectors);
	return true;
}


const CdImage::Track*
CdImage::TrackAt(u32 lba) const
{
	for (const Track& track : fTracks) {
		if (lba >= track.start && lba < track.start + track.length)
			return &track;
	}
	return nullptr;
}


bool
CdImage::ReadSector(u32 lba, u8* buffer)
{
	const Track* track = TrackAt(lba);
	if (track == nullptr || track->audio)
		return false;
	u64 offset = track->fileOffset + (u64)(lba - track->start)
		* track->sectorSize + track->dataOffset;
	return fFiles[track->fileIndex]->Read(offset, buffer, 2048);
}


bool
CdImage::ReadAudio(u32 lba, u8* buffer)
{
	const Track* track = TrackAt(lba);
	if (track == nullptr || !track->audio)
		return false;
	u64 offset = track->fileOffset + (u64)(lba - track->start) * 2352;
	return fFiles[track->fileIndex]->Read(offset, buffer, 2352);
}

}	// namespace bebox
