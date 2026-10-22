/*
 * BeBox emulator - SCSI target devices (disk and CD-ROM).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "ScsiDevice.h"

#include "../Log.h"
#include "../storage/CdImage.h"
#include "../storage/DiskImage.h"

#include <algorithm>

namespace bebox {

enum {
	kSenseNone = 0x0, kSenseNotReady = 0x2, kSenseMediumError = 0x3,
	kSenseIllegalRequest = 0x5, kSenseUnitAttention = 0x6,
	kSenseDataProtect = 0x7
};


static void
PutString(std::vector<u8>& data, size_t offset, size_t length,
	const std::string& text)
{
	for (size_t i = 0; i < length; i++)
		data[offset + i] = i < text.size() ? (u8)text[i] : ' ';
}


// #pragma mark - common


ScsiDevice::ScsiDevice()
	:
	fSenseKey(0),
	fSenseAsc(0),
	fSenseAscq(0),
	fUnitAttention(false)
{
}


void
ScsiDevice::Reset()
{
	// No unit attention after a reset: the R3 boot ROM gives up on a
	// device whose TEST UNIT READY reports one (it does not retry).
	// A medium change still raises it.
	fUnitAttention = false;
	SetSense(kSenseNone, 0);
}


void
ScsiDevice::SetSense(u8 key, u8 asc, u8 ascq)
{
	fSenseKey = key;
	fSenseAsc = asc;
	fSenseAscq = ascq;
}


void
ScsiDevice::CheckCondition(ScsiCommand& command, u8 key, u8 asc, u8 ascq)
{
	SetSense(key, asc, ascq);
	command.status = kScsiStatusCheckCondition;
	command.dataIn.clear();
	command.dataOutLength = 0;
}


bool
ScsiDevice::ModeSense(u8 page, std::vector<u8>& data)
{
	switch (page) {
		case 0x01:	// read/write error recovery
			data.insert(data.end(), { 0x01, 0x0a, 0x80, 0x08, 0, 0, 0, 0, 0,
				0, 0, 0 });
			return true;
		case 0x08:	// caching
			data.insert(data.end(), { 0x08, 0x0a, 0x04, 0, 0xff, 0xff, 0, 0,
				0xff, 0xff, 0xff, 0xff });
			return true;
	}
	return false;
}


void
ScsiDevice::Execute(ScsiCommand& command)
{
	command.status = kScsiStatusGood;
	command.dataIn.clear();
	command.dataOutLength = 0;
	const u8* cdb = command.cdb;
	u8 opcode = cdb[0];

	if (command.lun != 0) {
		if (opcode == 0x12) {
			std::vector<u8> data(36, 0);
			data[0] = 0x7f;		// no device at this LUN
			data.resize(std::min<size_t>(data.size(), cdb[4]));
			command.dataIn = data;
			return;
		}
		if (opcode != 0x03) {
			CheckCondition(command, kSenseIllegalRequest, 0x25);
			return;
		}
	}

	// A unit attention is reported once after a medium change, except to
	// INQUIRY and REQUEST SENSE.
	if (fUnitAttention && opcode != 0x12 && opcode != 0x03) {
		fUnitAttention = false;
		// not ready to ready change, medium may have changed
		CheckCondition(command, kSenseUnitAttention, 0x28);
		return;
	}

	switch (opcode) {
		case 0x03:		// REQUEST SENSE
		{
			std::vector<u8> data(18, 0);
			data[0] = 0x70;
			data[2] = fSenseKey;
			data[7] = 10;
			data[12] = fSenseAsc;
			data[13] = fSenseAscq;
			size_t length = cdb[4] == 0 ? 4 : cdb[4];
			data.resize(std::min(data.size(), length));
			command.dataIn = data;
			SetSense(kSenseNone, 0);
			return;
		}
		case 0x12:		// INQUIRY
		{
			if ((cdb[1] & 1) != 0) {
				CheckCondition(command, kSenseIllegalRequest, 0x24);
				return;
			}
			std::vector<u8> data(36, 0);
			Inquiry(data);
			data.resize(std::min<size_t>(data.size(), cdb[4]));
			command.dataIn = data;
			SetSense(kSenseNone, 0);
			return;
		}
		case 0x1a:		// MODE SENSE(6)
		case 0x5a:		// MODE SENSE(10)
		{
			bool ten = opcode == 0x5a;
			bool noBlockDescriptor = (cdb[1] & 0x08) != 0;
			u8 page = cdb[2] & 0x3f;
			std::vector<u8> pages;
			if (page == 0x3f) {
				for (u8 p = 1; p < 0x3f; p++)
					ModeSense(p, pages);
			} else if (!ModeSense(page, pages)) {
				CheckCondition(command, kSenseIllegalRequest, 0x24);
				return;
			}
			std::vector<u8> data(ten ? 8 : 4, 0);
			std::vector<u8> descriptor;
			if (!noBlockDescriptor) {
				u64 blocks = BlockCount();
				u32 size = BlockSize();
				descriptor = { 0, (u8)(blocks >> 16), (u8)(blocks >> 8),
					(u8)blocks, 0, (u8)(size >> 16), (u8)(size >> 8),
					(u8)size };
				if (blocks > 0xffffff)
					descriptor[1] = descriptor[2] = descriptor[3] = 0xff;
			}
			if (ten) {
				data[7] = (u8)descriptor.size();
			} else {
				data[3] = (u8)descriptor.size();
			}
			data.insert(data.end(), descriptor.begin(), descriptor.end());
			data.insert(data.end(), pages.begin(), pages.end());
			if (ten) {
				StoreBE16(&data[0], (u16)(data.size() - 2));
				size_t length = LoadBE16(&cdb[7]);
				data.resize(std::min(data.size(), length));
			} else {
				data[0] = (u8)(data.size() - 1);
				data.resize(std::min<size_t>(data.size(), cdb[4]));
			}
			command.dataIn = data;
			return;
		}
		case 0x15:		// MODE SELECT(6)
			command.dataOutLength = cdb[4];
			return;
		case 0x55:		// MODE SELECT(10)
			command.dataOutLength = LoadBE16(&cdb[7]);
			return;
		case 0x1d:		// SEND DIAGNOSTIC
		case 0x2f:		// VERIFY(10)
		case 0x35:		// SYNCHRONIZE CACHE
		case 0x0b:		// SEEK(6)
		case 0x2b:		// SEEK(10)
		case 0x01:		// REZERO UNIT
		case 0x16:		// RESERVE
		case 0x17:		// RELEASE
			return;
	}
	if (!ExecuteSpecific(command)) {
		BB_LOG(kLogScsi, kLogInfo, "%s: unsupported command %02x", Kind(),
			opcode);
		CheckCondition(command, kSenseIllegalRequest, 0x20);
	}
}


void
ScsiDevice::DataOut(ScsiCommand& command, const std::vector<u8>& data)
{
	DataOutSpecific(command, data);
}


// #pragma mark - disk


ScsiDisk::ScsiDisk(std::unique_ptr<DiskImage> image,
	const std::string& vendor, const std::string& product)
	:
	fImage(std::move(image)),
	fVendor(vendor),
	fProduct(product),
	fWriteLba(0),
	fWriteFailing(false)
{
}


u64
ScsiDisk::BlockCount()
{
	return fImage->Size() / 512;
}


void
ScsiDisk::Inquiry(std::vector<u8>& data)
{
	data[0] = 0x00;			// direct access device
	data[1] = 0x00;			// not removable
	data[2] = 0x02;			// SCSI-2
	data[3] = 0x02;			// response data format
	data[4] = 31;
	data[7] = 0x10;			// synchronous transfers
	PutString(data, 8, 8, fVendor);
	PutString(data, 16, 16, fProduct);
	PutString(data, 32, 4, "1.0");
}


bool
ScsiDisk::ExecuteSpecific(ScsiCommand& command)
{
	const u8* cdb = command.cdb;
	u64 blocks = BlockCount();
	switch (cdb[0]) {
		case 0x00:		// TEST UNIT READY
		case 0x04:		// FORMAT UNIT
		case 0x1b:		// START STOP UNIT
		case 0x1e:		// PREVENT ALLOW MEDIUM REMOVAL
			return true;
		case 0x25:		// READ CAPACITY
		{
			u32 last = (u32)std::min<u64>(blocks - 1, 0xffffffff);
			command.dataIn.assign(8, 0);
			StoreBE32(&command.dataIn[0], last);
			StoreBE32(&command.dataIn[4], 512);
			return true;
		}
		case 0x08:		// READ(6)
		case 0x28:		// READ(10)
		case 0xa8:		// READ(12)
		{
			u64 lba;
			u32 count;
			if (cdb[0] == 0x08) {
				lba = ((cdb[1] & 0x1f) << 16) | (cdb[2] << 8) | cdb[3];
				count = cdb[4] == 0 ? 256 : cdb[4];
			} else if (cdb[0] == 0x28) {
				lba = LoadBE32(&cdb[2]);
				count = LoadBE16(&cdb[7]);
			} else {
				lba = LoadBE32(&cdb[2]);
				count = LoadBE32(&cdb[6]);
			}
			if (lba + count > blocks) {
				CheckCondition(command, kSenseIllegalRequest, 0x21);
				return true;
			}
			if (fActivity)
				fActivity(true);
			command.dataIn.resize((size_t)count * 512);
			if (!fImage->Read(lba * 512, command.dataIn.data(),
					command.dataIn.size()))
				CheckCondition(command, kSenseMediumError, 0x11);
			return true;
		}
		case 0x0a:		// WRITE(6)
		case 0x2a:		// WRITE(10)
		case 0x2e:		// WRITE AND VERIFY(10)
		case 0xaa:		// WRITE(12)
		{
			u64 lba;
			u32 count;
			if (cdb[0] == 0x0a) {
				lba = ((cdb[1] & 0x1f) << 16) | (cdb[2] << 8) | cdb[3];
				count = cdb[4] == 0 ? 256 : cdb[4];
			} else if (cdb[0] == 0xaa) {
				lba = LoadBE32(&cdb[2]);
				count = LoadBE32(&cdb[6]);
			} else {
				lba = LoadBE32(&cdb[2]);
				count = LoadBE16(&cdb[7]);
			}
			if (lba + count > blocks) {
				CheckCondition(command, kSenseIllegalRequest, 0x21);
				return true;
			}
			if (fImage->IsReadOnly()) {
				CheckCondition(command, kSenseDataProtect, 0x27);
				return true;
			}
			fWriteLba = lba;
			command.dataOutLength = count * 512;
			return true;
		}
		case 0x07:		// REASSIGN BLOCKS
			command.dataOutLength = 4;
			return true;
	}
	return false;
}


void
ScsiDisk::DataOutSpecific(ScsiCommand& command, const std::vector<u8>& data)
{
	u8 opcode = command.cdb[0];
	if (opcode == 0x0a || opcode == 0x2a || opcode == 0x2e
		|| opcode == 0xaa) {
		if (fActivity)
			fActivity(true);
		if (fImage->Write(fWriteLba * 512, data.data(), data.size()))
			fWriteFailing = false;
		else {
			if (!fWriteFailing) {
				BB_LOG(kLogStorage, kLogError, "cannot write to the disk "
					"image %s: BeOS gets a write error",
					fImage->Path().c_str());
			}
			fWriteFailing = true;
			CheckCondition(command, kSenseMediumError, 0x0c);
		}
	}
}


// #pragma mark - CD-ROM


ScsiCd::ScsiCd(Scheduler& scheduler, u64 ticksPerSecond,
	const std::string& vendor, const std::string& product)
	:
	fVendor(vendor),
	fProduct(product),
	fBlockSize(2048),
	fLocked(false),
	fTicksPerSecond(ticksPerSecond),
	fChangeTimer(scheduler, [this]() { Insert(std::move(fNextImage)); },
		"cd change")
{
}


void
ScsiCd::Reset()
{
	// a power cycle in the middle of a change: the new disc is in
	if (fChangeTimer.IsArmed()) {
		fChangeTimer.Cancel();
		fImage = std::move(fNextImage);
	}
	ScsiDevice::Reset();
	fBlockSize = 2048;
	fLocked = false;
}


void
ScsiCd::Change(std::unique_ptr<CdImage> image)
{
	fChangeTimer.Cancel();
	fNextImage.reset();
	if (!fImage) {
		if (image)
			Insert(std::move(image));
		return;
	}
	Eject();
	if (image) {
		fNextImage = std::move(image);
		fChangeTimer.ArmIn(kChangeSeconds * fTicksPerSecond);
	}
}


bool
ScsiCd::Insert(std::unique_ptr<CdImage> image)
{
	fImage = std::move(image);
	fUnitAttention = true;		// medium changed
	return true;
}


void
ScsiCd::Eject()
{
	fImage.reset();
	fUnitAttention = true;
}


std::string
ScsiCd::MediumPath() const
{
	return fImage ? fImage->Path() : std::string();
}


u64
ScsiCd::BlockCount()
{
	if (!fImage)
		return 0;
	return (u64)fImage->Sectors() * (2048 / fBlockSize);
}


void
ScsiCd::Inquiry(std::vector<u8>& data)
{
	data[0] = 0x05;			// CD-ROM
	data[1] = 0x80;			// removable
	data[2] = 0x02;
	data[3] = 0x02;
	data[4] = 31;
	data[7] = 0x10;
	PutString(data, 8, 8, fVendor);
	PutString(data, 16, 16, fProduct);
	PutString(data, 32, 4, "3593");
}


bool
ScsiCd::ModeSense(u8 page, std::vector<u8>& data)
{
	switch (page) {
		case 0x01:
			data.insert(data.end(), { 0x01, 0x06, 0x00, 0x05, 0, 0, 0, 0 });
			return true;
		case 0x0e:	// CD audio control
			data.insert(data.end(), { 0x0e, 0x0e, 0x04, 0, 0, 0, 0, 0x4b,
				0x01, 0xff, 0x02, 0xff, 0, 0, 0, 0 });
			return true;
	}
	return false;
}


bool
ScsiCd::_Read(ScsiCommand& command, u64 lba, u32 count)
{
	if (!fImage) {
		CheckCondition(command, kSenseNotReady, 0x3a);
		return true;
	}
	u64 blocks = BlockCount();
	if (lba + count > blocks) {
		CheckCondition(command, kSenseIllegalRequest, 0x21);
		return true;
	}
	BB_LOG(kLogScsi, kLogDebug, "CD read lba %llu count %u (block %u)",
		(unsigned long long)lba, count, fBlockSize);
	if (fActivity)
		fActivity(true);
	command.dataIn.resize((size_t)count * fBlockSize);
	u8 sector[2048];
	u64 byteOffset = lba * fBlockSize;
	u64 byteEnd = byteOffset + (u64)count * fBlockSize;
	size_t out = 0;
	while (byteOffset < byteEnd) {
		u32 lsn = (u32)(byteOffset / 2048);
		u32 within = (u32)(byteOffset % 2048);
		if (!fImage->ReadSector(lsn, sector)) {
			CheckCondition(command, kSenseMediumError, 0x11);
			return true;
		}
		size_t chunk = std::min<u64>(2048 - within, byteEnd - byteOffset);
		memcpy(&command.dataIn[out], sector + within, chunk);
		out += chunk;
		byteOffset += chunk;
	}
	return true;
}


static void
ToMsf(u32 lba, u8* msf)
{
	lba += 150;
	msf[0] = 0;
	msf[1] = (u8)(lba / 4500);
	msf[2] = (u8)((lba / 75) % 60);
	msf[3] = (u8)(lba % 75);
}


void
ScsiCd::_ReadToc(ScsiCommand& command)
{
	const u8* cdb = command.cdb;
	bool msf = (cdb[1] & 2) != 0;
	int format = cdb[9] >> 6;
	if (format == 0)
		format = cdb[2] & 0x0f;
	int startTrack = cdb[6];
	const std::vector<CdImage::Track>& tracks = fImage->Tracks();
	std::vector<u8> data(4, 0);
	auto addEntry = [&](int number, bool audio, u32 lba) {
		u8 entry[8] = { 0, (u8)(audio ? 0x10 : 0x14), (u8)number, 0, 0, 0,
			0, 0 };
		if (msf)
			ToMsf(lba, &entry[4]);
		else
			StoreBE32(&entry[4], lba);
		data.insert(data.end(), entry, entry + 8);
	};
	if (format == 1) {
		// session info: first session, first track
		addEntry(tracks.front().number, tracks.front().audio,
			tracks.front().start);
	} else {
		for (const CdImage::Track& track : tracks) {
			if (track.number >= startTrack)
				addEntry(track.number, track.audio, track.start);
		}
		addEntry(0xaa, false, fImage->Sectors());
	}
	StoreBE16(&data[0], (u16)(data.size() - 2));
	data[2] = (u8)tracks.front().number;
	data[3] = (u8)tracks.back().number;
	size_t length = LoadBE16(&cdb[7]);
	data.resize(std::min(data.size(), length));
	command.dataIn = data;
}


bool
ScsiCd::ExecuteSpecific(ScsiCommand& command)
{
	const u8* cdb = command.cdb;
	u8 opcode = cdb[0];
	// Commands that need a disc.
	bool needsMedium = opcode == 0x00 || opcode == 0x25 || opcode == 0x08
		|| opcode == 0x28 || opcode == 0xa8 || opcode == 0x43
		|| opcode == 0x44 || opcode == 0x42;
	if (needsMedium && !fImage) {
		CheckCondition(command, kSenseNotReady, 0x3a);
		return true;
	}
	switch (opcode) {
		case 0x00:		// TEST UNIT READY
			return true;
		case 0x1b:		// START STOP UNIT
			if ((cdb[4] & 3) == 2) {	// LoEj without Start: eject
				if (fLocked) {
					CheckCondition(command, kSenseIllegalRequest, 0x53, 0x02);
					return true;
				}
				std::string path = MediumPath();
				fImage.reset();
				if (fEjectCallback && !path.empty())
					fEjectCallback(path);
			}
			return true;
		case 0x1e:		// PREVENT ALLOW MEDIUM REMOVAL
			fLocked = (cdb[4] & 1) != 0;
			return true;
		case 0x25:		// READ CAPACITY
		{
			command.dataIn.assign(8, 0);
			StoreBE32(&command.dataIn[0], (u32)(BlockCount() - 1));
			StoreBE32(&command.dataIn[4], fBlockSize);
			return true;
		}
		case 0x08:
			return _Read(command, ((cdb[1] & 0x1f) << 16) | (cdb[2] << 8)
				| cdb[3], cdb[4] == 0 ? 256 : cdb[4]);
		case 0x28:
			return _Read(command, LoadBE32(&cdb[2]), LoadBE16(&cdb[7]));
		case 0xa8:
			return _Read(command, LoadBE32(&cdb[2]), LoadBE32(&cdb[6]));
		case 0x0a: case 0x2a: case 0xaa:
			CheckCondition(command, kSenseDataProtect, 0x27);
			return true;
		case 0x43:		// READ TOC
			_ReadToc(command);
			return true;
		case 0x44:		// READ HEADER
		{
			u32 lba = LoadBE32(&cdb[2]);
			command.dataIn.assign(8, 0);
			command.dataIn[0] = 1;		// mode 1
			if ((cdb[1] & 2) != 0)
				ToMsf(lba, &command.dataIn[4]);
			else
				StoreBE32(&command.dataIn[4], lba);
			return true;
		}
		case 0x42:		// READ SUB-CHANNEL
		{
			size_t length = LoadBE16(&cdb[7]);
			std::vector<u8> data(4, 0);
			data[1] = 0x15;		// audio status: no current status
			if ((cdb[2] & 0x40) != 0 && cdb[3] == 1) {
				std::vector<u8> position(12, 0);
				position[0] = 1;
				position[1] = 0x14;
				position[2] = 1;
				position[3] = 1;
				data.insert(data.end(), position.begin(), position.end());
			}
			StoreBE16(&data[2], (u16)(data.size() - 4));
			data.resize(std::min(data.size(), length));
			command.dataIn = data;
			return true;
		}
		case 0x45: case 0x47: case 0x48: case 0x4b: case 0x4e:
			// audio playback is accepted (not played yet)
			return true;
	}
	return false;
}


void
ScsiCd::DataOutSpecific(ScsiCommand& command, const std::vector<u8>& data)
{
	u8 opcode = command.cdb[0];
	if (opcode != 0x15 && opcode != 0x55)
		return;
	if (Log::Enabled(kLogScsi, kLogDebug)) {
		char text[3 * 32 + 1] = "";
		for (size_t i = 0; i < data.size() && i < 32; i++)
			sprintf(text + i * 3, "%02x ", data[i]);
		BB_LOG(kLogScsi, kLogDebug, "MODE SELECT data: %s", text);
	}
	// The block descriptor may change the logical block size (old drivers
	// switch to 512 byte blocks).
	size_t header = opcode == 0x15 ? 4 : 8;
	if (data.size() < header)
		return;
	size_t descriptorLength = opcode == 0x15 ? data[3]
		: LoadBE16(&data[6]);
	if (descriptorLength >= 8 && data.size() >= header + 8) {
		u32 size = (data[header + 5] << 16) | (data[header + 6] << 8)
			| data[header + 7];
		if (size == 512 || size == 2048) {
			fBlockSize = size;
			BB_LOG(kLogScsi, kLogInfo, "CD-ROM block size %u", size);
		}
	}
}

}	// namespace bebox
