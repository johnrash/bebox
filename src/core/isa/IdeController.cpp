/*
 * BeBox emulator - IDE interface with ATA disks.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "IdeController.h"

#include "../Log.h"

#include <algorithm>

namespace bebox {

static const u32 kSectorSize = 512;

// #pragma mark - controller


IdeController::IdeController(Scheduler& scheduler, u64 ticksPerSecond)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fControl(0),
	fIrqPending(false),
	fIrqLine(false),
	fBufferPosition(0),
	fBufferLength(0),
	fWriting(false)
{
	fBuffer.resize(kSectorSize * 256);
	fTimer.reset(new Timer(scheduler, [this]() {
		std::function<void()> action;
		action.swap(fPending);
		if (action)
			action();
	}, "ide"));
	Reset();
}


IdeController::~IdeController()
{
}


void
IdeController::AttachDevice(int unit, std::unique_ptr<IdeDevice> device)
{
	fDevices[unit] = std::move(device);
	if (fDevices[unit]) {
		fDevices[unit]->Attach(this, unit);
		fDevices[unit]->Reset();
	}
}


void
IdeController::Reset()
{
	fControl = 0;
	fTimer->Cancel();
	fPending = nullptr;
	_SoftReset();
}


void
IdeController::_SoftReset()
{
	fTaskFile = IdeTaskFile();
	fBufferPosition = fBufferLength = 0;
	fWriting = false;
	for (int unit = 0; unit < 2; unit++) {
		if (fDevices[unit])
			fDevices[unit]->Reset();
	}
	if (fDevices[0])
		fDevices[0]->SetSignature(fTaskFile);
	fTaskFile.error = 0x01;		// diagnostic: no error
	fTaskFile.status = kIdeStatusReady | kIdeStatusSeekComplete;
	_SetIrq(false);
}


void
IdeController::_SetIrq(bool pending)
{
	fIrqPending = pending;
	_UpdateIrqLine();
}


void
IdeController::_UpdateIrqLine()
{
	bool level = fIrqPending && (fControl & 2) == 0;
	if (level != fIrqLine) {
		fIrqLine = level;
		if (fIrq)
			fIrq(level);
	}
}


void
IdeController::SetActivity(bool active)
{
	if (fActivity)
		fActivity(active);
}


void
IdeController::Defer(u64 delay, std::function<void()> then)
{
	fTaskFile.status = (fTaskFile.status | kIdeStatusBusy) & ~kIdeStatusDrq;
	fPending = then;
	fTimer->ArmIn(std::max<u64>(delay, 1));
}


void
IdeController::StartTransfer(u32 length, bool raiseIrq, u64 delay)
{
	auto start = [this, length, raiseIrq]() {
		fBufferPosition = 0;
		fBufferLength = length;
		fTaskFile.status = (fTaskFile.status & ~(kIdeStatusBusy
			| kIdeStatusError)) | kIdeStatusDrq | kIdeStatusReady
			| kIdeStatusSeekComplete;
		if (raiseIrq)
			_SetIrq(true);
	};
	fWriting = false;
	if (delay == 0) {
		start();
	} else
		Defer(delay, start);
}


void
IdeController::CompleteCommand(u8 status, bool raiseIrq, u64 delay)
{
	auto complete = [this, status, raiseIrq]() {
		fBufferPosition = fBufferLength = 0;
		fTaskFile.status = status & ~(kIdeStatusBusy | kIdeStatusDrq);
		if (raiseIrq)
			_SetIrq(true);
		SetActivity(false);
	};
	if (delay == 0)
		complete();
	else
		Defer(delay, complete);
}


u16
IdeController::_ReadData()
{
	if ((fTaskFile.status & kIdeStatusDrq) == 0
		|| fBufferPosition + 2 > fBufferLength)
		return 0xffff;
	u16 value = LoadLE16(&fBuffer[fBufferPosition]);
	fBufferPosition += 2;
	if (fBufferPosition >= fBufferLength) {
		fTaskFile.status &= ~kIdeStatusDrq;
		IdeDevice* device = _SelectedDevice();
		if (device != nullptr)
			device->ReadBlockDone(fTaskFile);
	}
	return value;
}


void
IdeController::_WriteData(u16 value)
{
	if ((fTaskFile.status & kIdeStatusDrq) == 0
		|| fBufferPosition + 2 > fBufferLength)
		return;
	StoreLE16(&fBuffer[fBufferPosition], value);
	fBufferPosition += 2;
	if (fBufferPosition >= fBufferLength) {
		fTaskFile.status &= ~kIdeStatusDrq;
		IdeDevice* device = _SelectedDevice();
		if (device != nullptr)
			device->WriteBlockDone(fTaskFile);
	}
}


u32
IdeController::IoRead(u32 port, int size)
{
	u32 value = _IoRead(port, size);
	if (port != 0x1f0)
		BB_LOG(kLogIde, kLogTrace, "read%d %03x = %02x", size * 8, port, value);
	return value;
}


u32
IdeController::_IoRead(u32 port, int size)
{
	bool present = fDevices[0] || fDevices[1];
	IdeDevice* device = _SelectedDevice();
	if (port == 0x1f0) {
		if (device == nullptr)
			return size == 4 ? 0xffffffff : 0xffff;
		u32 value = _ReadData();
		if (size == 4)
			value |= (u32)_ReadData() << 16;
		return size == 1 ? value & 0xff : value;
	}
	if (size > 1) {
		// wider accesses to byte registers: byte by byte
		u32 value = 0;
		for (int i = 0; i < size; i++)
			value |= _IoRead(port + i, 1) << (8 * i);
		return value;
	}
	if (!present) {
		// Nobody drives the bus: the status reads as neither busy nor
		// ready, so drivers give up waiting for DRDY. (With "ready"
		// DR7's kernel sends IDENTIFY and waits for the interrupt
		// forever; with "busy" the ROM waits 31.5 s for the drive.)
		return 0x00;
	}
	if (device == nullptr) {
		// the other device answers for an absent one with zeros
		if (port == 0x1f6)
			return fTaskFile.head | 0xa0;
		return 0x00;
	}
	switch (port) {
		case 0x1f1: return fTaskFile.error;
		case 0x1f2: return fTaskFile.count;
		case 0x1f3: return fTaskFile.sector;
		case 0x1f4: return fTaskFile.cylinderLow;
		case 0x1f5: return fTaskFile.cylinderHigh;
		case 0x1f6: return fTaskFile.head | 0xa0;
		case 0x1f7:
			_SetIrq(false);
			return fTaskFile.status;
		case 0x3f6:
			return fTaskFile.status;
	}
	return 0xff;
}


void
IdeController::IoWrite(u32 port, u32 value, int size)
{
	if (port != 0x1f0)
		BB_LOG(kLogIde, kLogTrace, "write%d %03x = %02x", size * 8, port,
			value);
	if (port == 0x1f0) {
		_WriteData((u16)value);
		if (size == 4)
			_WriteData((u16)(value >> 16));
		return;
	}
	if (size > 1) {
		for (int i = 0; i < size; i++)
			IoWrite(port + i, (value >> (8 * i)) & 0xff, 1);
		return;
	}
	u8 byte = (u8)value;
	IdeTaskFile& task = fTaskFile;
	switch (port) {
		case 0x1f1:
			task.features = byte;
			break;
		case 0x1f2:
			task.previousCount = task.count;
			task.count = byte;
			break;
		case 0x1f3:
			task.previousSector = task.sector;
			task.sector = byte;
			break;
		case 0x1f4:
			task.previousCylinderLow = task.cylinderLow;
			task.cylinderLow = byte;
			break;
		case 0x1f5:
			task.previousCylinderHigh = task.cylinderHigh;
			task.cylinderHigh = byte;
			break;
		case 0x1f6:
			task.head = byte;
			break;
		case 0x1f7:
		{
			IdeDevice* device = _SelectedDevice();
			if (device == nullptr)
				break;
			if ((task.status & kIdeStatusBusy) != 0 && byte != 0x08) {
				BB_LOG(kLogIde, kLogInfo, "command %02x while busy", byte);
				break;
			}
			_SetIrq(false);
			task.error = 0;
			task.status &= ~(kIdeStatusError | kIdeStatusDrq);
			BB_LOG(kLogIde, kLogDebug, "unit %d command %02x at %.3f ms",
				_Selected(), byte,
				fScheduler.Now() * 1000.0 / fTicksPerSecond);
			device->Command(task, byte);
			break;
		}
		case 0x3f6:
		{
			bool wasReset = (fControl & 4) != 0;
			fControl = byte;
			if ((byte & 4) != 0 && !wasReset) {
				fTimer->Cancel();
				fPending = nullptr;
				fTaskFile.status = kIdeStatusBusy;
			} else if ((byte & 4) == 0 && wasReset) {
				_SoftReset();
			}
			_UpdateIrqLine();
			break;
		}
	}
}


// #pragma mark - ATA disk


AtaDisk::AtaDisk(std::unique_ptr<DiskImage> image)
	:
	fImage(std::move(image)),
	fMultiple(16),
	fCommand(0),
	fLba(0),
	fRemaining(0),
	fLba48(false),
	fWriteFailing(false)
{
	fSectors = fImage->Size() / kSectorSize;
	fHeads = 16;
	fSectorsPerTrack = 63;
	u64 cylinders = fSectors / (fHeads * fSectorsPerTrack);
	fCylinders = (u32)std::min<u64>(cylinders, 16383);
	if (fCylinders == 0)
		fCylinders = 1;
	fLogicalHeads = fHeads;
	fLogicalSectors = fSectorsPerTrack;
}


void
AtaDisk::Reset()
{
	fCommand = 0;
	fRemaining = 0;
}


void
AtaDisk::SetSignature(IdeTaskFile& taskFile)
{
	taskFile.count = 1;
	taskFile.sector = 1;
	taskFile.cylinderLow = 0;
	taskFile.cylinderHigh = 0;
	taskFile.head &= 0x10;
}


bool
AtaDisk::_Address(IdeTaskFile& task, u64& lba)
{
	if (fLba48) {
		lba = (u64)task.sector | ((u64)task.cylinderLow << 8)
			| ((u64)task.cylinderHigh << 16) | ((u64)task.previousSector << 24)
			| ((u64)task.previousCylinderLow << 32)
			| ((u64)task.previousCylinderHigh << 40);
	} else if ((task.head & 0x40) != 0) {
		lba = ((u64)(task.head & 0x0f) << 24) | ((u64)task.cylinderHigh << 16)
			| ((u64)task.cylinderLow << 8) | task.sector;
	} else {
		u32 cylinder = task.cylinderLow | (task.cylinderHigh << 8);
		u32 head = task.head & 0x0f;
		u32 sector = task.sector;
		if (sector == 0 || sector > fLogicalSectors || head >= fLogicalHeads)
			return false;
		lba = ((u64)cylinder * fLogicalHeads + head) * fLogicalSectors
			+ sector - 1;
	}
	return lba < fSectors;
}


void
AtaDisk::_SetAddress(IdeTaskFile& task, u64 lba)
{
	if (fLba48) {
		task.sector = (u8)lba;
		task.cylinderLow = (u8)(lba >> 8);
		task.cylinderHigh = (u8)(lba >> 16);
		task.previousSector = (u8)(lba >> 24);
		task.previousCylinderLow = (u8)(lba >> 32);
		task.previousCylinderHigh = (u8)(lba >> 40);
	} else if ((task.head & 0x40) != 0) {
		task.sector = (u8)lba;
		task.cylinderLow = (u8)(lba >> 8);
		task.cylinderHigh = (u8)(lba >> 16);
		task.head = (task.head & 0xf0) | ((lba >> 24) & 0x0f);
	} else {
		u32 sector = (u32)(lba % fLogicalSectors) + 1;
		u64 track = lba / fLogicalSectors;
		u32 head = (u32)(track % fLogicalHeads);
		u32 cylinder = (u32)(track / fLogicalHeads);
		task.sector = (u8)sector;
		task.cylinderLow = (u8)cylinder;
		task.cylinderHigh = (u8)(cylinder >> 8);
		task.head = (task.head & 0xf0) | head;
	}
}


void
AtaDisk::_Abort(IdeTaskFile& task)
{
	task.error = kIdeErrorAbort;
	fController->CompleteCommand(kIdeStatusReady | kIdeStatusSeekComplete
		| kIdeStatusError, true);
}


static void
PutString(std::vector<u8>& buffer, int word, int words, const char* text)
{
	// ATA strings: two characters per word, the first in the high byte.
	size_t length = strlen(text);
	for (int i = 0; i < words * 2; i++) {
		char c = (size_t)i < length ? text[i] : ' ';
		buffer[word * 2 + (i ^ 1)] = (u8)c;
	}
}


void
AtaDisk::_Identify(IdeTaskFile& task)
{
	std::vector<u8>& buffer = fController->Buffer();
	memset(buffer.data(), 0, kSectorSize);
	auto put = [&buffer](int word, u16 value) {
		StoreLE16(&buffer[word * 2], value);
	};
	put(0, 0x0040);
	put(1, (u16)fCylinders);
	put(3, (u16)fHeads);
	put(4, (u16)(kSectorSize * fSectorsPerTrack));
	put(5, kSectorSize);
	put(6, (u16)fSectorsPerTrack);
	PutString(buffer, 10, 10, "BEBOX0000000001");
	put(20, 3);			// buffer type
	put(21, 512);		// buffer size in sectors
	PutString(buffer, 23, 4, "1.0");
	PutString(buffer, 27, 20, "BeBox Emulator Hard Disk");
	put(47, 0x8010);	// up to 16 sectors per DRQ block
	put(49, 0x0200);	// LBA
	put(51, 0x0200);	// PIO mode 2 timing
	put(53, 0x0003);	// words 54-58 and 64-70 valid
	put(54, (u16)fCylinders);
	put(55, (u16)fLogicalHeads);
	put(56, (u16)fLogicalSectors);
	u32 chs = std::min<u64>(fSectors, (u64)fCylinders * fLogicalHeads
		* fLogicalSectors);
	put(57, (u16)chs);
	put(58, (u16)(chs >> 16));
	put(59, (u16)(0x100 | fMultiple));
	u32 lba28 = (u32)std::min<u64>(fSectors, 0x0fffffff);
	put(60, (u16)lba28);
	put(61, (u16)(lba28 >> 16));
	put(64, 0x0003);	// PIO modes 3 and 4
	put(65, 120);
	put(66, 120);
	put(67, 120);
	put(68, 120);
	put(80, 0x001e);	// ATA-1 to ATA-4
	bool large = fSectors > 0x0fffffff;
	put(82, 0x4000);
	put(83, (u16)(0x4000 | (large ? 0x0400 : 0) | 0x1000));	// FLUSH CACHE
	put(84, 0x4000);
	put(85, 0x0000);
	put(86, (u16)((large ? 0x0400 : 0) | 0x1000));
	put(87, 0x4000);
	put(100, (u16)fSectors);
	put(101, (u16)(fSectors >> 16));
	put(102, (u16)(fSectors >> 32));
	put(103, (u16)(fSectors >> 48));
	fController->StartTransfer(kSectorSize, true, fController->Ticks(20));
}


void
AtaDisk::_ReadNext(IdeTaskFile& task)
{
	u32 block = fCommand == 0xc4 || fCommand == 0x29
		? std::min(fRemaining, fMultiple) : 1;
	std::vector<u8>& buffer = fController->Buffer();
	if (!fImage->Read(fLba * kSectorSize, buffer.data(),
			block * kSectorSize)) {
		task.error = kIdeErrorUncorrectable;
		_SetAddress(task, fLba);
		fController->CompleteCommand(kIdeStatusReady | kIdeStatusError,
			true);
		return;
	}
	fController->SetActivity(true);
	fController->StartTransfer(block * kSectorSize, true,
		fController->Ticks(20));
	fRemaining -= block;
	fLba += block;
	_SetAddress(task, fLba - 1);
}


void
AtaDisk::ReadBlockDone(IdeTaskFile& task)
{
	if (fCommand == 0xec)
		return;
	if (fRemaining > 0) {
		_ReadNext(task);
		return;
	}
	fController->CompleteCommand(kIdeStatusReady | kIdeStatusSeekComplete,
		false);
}


void
AtaDisk::WriteBlockDone(IdeTaskFile& task)
{
	u32 block = fCommand == 0xc5 || fCommand == 0x39
		? std::min(fRemaining, fMultiple) : 1;
	std::vector<u8>& buffer = fController->Buffer();
	fController->SetActivity(true);
	if (!fImage->Write(fLba * kSectorSize, buffer.data(),
			block * kSectorSize)) {
		if (!fWriteFailing) {
			BB_LOG(kLogStorage, kLogError, "cannot write to the disk image "
				"%s: BeOS gets a write error", fImage->Path().c_str());
		}
		fWriteFailing = true;
		task.error = kIdeErrorAbort;
		_SetAddress(task, fLba);
		fController->CompleteCommand(kIdeStatusReady | kIdeStatusError,
			true, fController->Ticks(20));
		return;
	}
	fWriteFailing = false;
	fRemaining -= block;
	fLba += block;
	_SetAddress(task, fLba - 1);
	if (fRemaining == 0) {
		fController->CompleteCommand(kIdeStatusReady | kIdeStatusSeekComplete,
			true, fController->Ticks(20));
		return;
	}
	u32 next = fCommand == 0xc5 || fCommand == 0x39
		? std::min(fRemaining, fMultiple) : 1;
	fController->StartTransfer(next * kSectorSize, true,
		fController->Ticks(20));
}


void
AtaDisk::Command(IdeTaskFile& task, u8 command)
{
	fCommand = command;
	fLba48 = command == 0x24 || command == 0x29 || command == 0x34
		|| command == 0x39 || command == 0x42 || command == 0x27;
	switch (command) {
		case 0xec:
			_Identify(task);
			return;
		case 0x20: case 0x21: case 0xc4: case 0x24: case 0x29:
		case 0x30: case 0x31: case 0xc5: case 0x34: case 0x39:
		case 0x40: case 0x41: case 0x42:
		{
			bool read = command == 0x20 || command == 0x21 || command == 0xc4
				|| command == 0x24 || command == 0x29;
			bool verify = command == 0x40 || command == 0x41
				|| command == 0x42;
			if ((command == 0xc4 || command == 0xc5) && fMultiple == 0) {
				_Abort(task);
				return;
			}
			u32 count = task.count;
			if (fLba48)
				count |= (u32)task.previousCount << 8;
			if (count == 0)
				count = fLba48 ? 65536 : 256;
			u64 lba;
			if (!_Address(task, lba) || lba + count > fSectors) {
				task.error = kIdeErrorIdNotFound;
				fController->CompleteCommand(kIdeStatusReady
					| kIdeStatusError, true, fController->Ticks(20));
				return;
			}
			fLba = lba;
			fRemaining = count;
			BB_LOG(kLogIde, kLogDebug, "%s lba %llu count %u",
				read ? "read" : verify ? "verify" : "write",
				(unsigned long long)lba, count);
			if (verify) {
				_SetAddress(task, lba + count - 1);
				fController->CompleteCommand(kIdeStatusReady
					| kIdeStatusSeekComplete, true, fController->Ticks(20));
			} else if (read) {
				_ReadNext(task);
			} else {
				u32 block = command == 0xc5 || command == 0x39
					? std::min(fRemaining, fMultiple) : 1;
				fController->StartTransfer(block * kSectorSize, false);
			}
			return;
		}
		case 0x70: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:
		case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1a:
		case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
		case 0xef: case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0x94:
		case 0x95: case 0x96: case 0x97: case 0xe7: case 0xea:
			if (command == 0xe7 || command == 0xea)
				fImage->Flush();
			fController->CompleteCommand(kIdeStatusReady
				| kIdeStatusSeekComplete, true, fController->Ticks(20));
			return;
		case 0xe5: case 0x98:
			task.count = 0xff;	// active / idle
			fController->CompleteCommand(kIdeStatusReady
				| kIdeStatusSeekComplete, true, fController->Ticks(20));
			return;
		case 0x91:
			fLogicalHeads = (task.head & 0x0f) + 1;
			fLogicalSectors = task.count;
			if (fLogicalSectors == 0) {
				_Abort(task);
				return;
			}
			fController->CompleteCommand(kIdeStatusReady
				| kIdeStatusSeekComplete, true, fController->Ticks(20));
			return;
		case 0xc6:
			if (task.count > 16 || (task.count & (task.count - 1)) != 0) {
				_Abort(task);
				return;
			}
			fMultiple = task.count;
			fController->CompleteCommand(kIdeStatusReady
				| kIdeStatusSeekComplete, true, fController->Ticks(20));
			return;
		case 0x90:
			SetSignature(task);
			task.error = 0x01;
			fController->CompleteCommand(kIdeStatusReady
				| kIdeStatusSeekComplete, true, fController->Ticks(100));
			return;
		case 0xf8:
			_SetAddress(task, fSectors - 1);
			fController->CompleteCommand(kIdeStatusReady
				| kIdeStatusSeekComplete, true, fController->Ticks(20));
			return;
	}
	BB_LOG(kLogIde, kLogInfo, "unsupported ATA command %02x", command);
	_Abort(task);
}

}	// namespace bebox
