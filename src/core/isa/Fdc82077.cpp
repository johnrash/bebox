/*
 * BeBox emulator - floppy disk controller (82077AA compatible).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Fdc82077.h"

#include "DmaChannels.h"
#include "../Log.h"

namespace bebox {

enum {
	kMsrRqm = 0x80, kMsrDio = 0x40, kMsrNonDma = 0x20, kMsrBusy = 0x10,
	kDorReset = 0x04, kDorDmaIrq = 0x08,
	kSt0AbnormalTermination = 0x40, kSt0Invalid = 0x80,
	kSt0SeekEnd = 0x20, kSt0Equipment = 0x10, kSt0NotReady = 0x08,
	kSt1MissingAddressMark = 0x01, kSt1NotWritable = 0x02,
	kSt1NoData = 0x04, kSt1EndOfCylinder = 0x80
};

static const u32 kSectorSize = 512;


Fdc82077::Fdc82077(Scheduler& scheduler, u64 ticksPerSecond,
	DmaChannels& dma)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fDma(dma),
	fIrqLevel(false)
{
	fTimer.reset(new Timer(scheduler, [this]() {
		std::function<void()> action;
		action.swap(fTimerAction);
		if (action)
			action();
	}, "fdc"));
	fSectorBuffer.resize(kSectorSize * 36);
	Reset();
}


Fdc82077::~Fdc82077()
{
}


void
Fdc82077::Reset()
{
	fDor = 0;
	fDataRate = 0;
	fSpecify[0] = fSpecify[1] = 0;
	fConfigure = 0x20;
	fPrecomp = 0;
	fLocked = false;
	fPerpendicular = 0;
	fNonDma = false;
	_ResetController(false);
}


void
Fdc82077::_ResetController(bool fromDor)
{
	fTimer->Cancel();
	fTimerAction = nullptr;
	fPhase = kCommand;
	fCommand.clear();
	fResult.clear();
	fResultPosition = 0;
	for (int i = 0; i < 4; i++) {
		fSeekPending[i] = false;
		fSeekStatus[i] = 0;
	}
	fResetSenses = 0;
	_SetIrq(false);
	if (fromDor && fResetPolling) {
		// After a reset the controller interrupts once; four sense
		// interrupt status commands report the polling of the drives.
		fResetSenses = 4;
		fTimerAction = [this]() { _SetIrq(true); };
		fTimer->ArmIn(_Ms(1));
	}
}


void
Fdc82077::_SetIrq(bool level)
{
	bool output = level && (fDor & kDorDmaIrq) != 0;
	fIrqLevel = level;
	if (fIrq)
		fIrq(output);
}


bool
Fdc82077::InsertDisk(int drive, std::unique_ptr<DiskImage> image,
	bool writeProtected, std::string& error)
{
	Drive& d = fDrives[drive];
	u64 size = image->Size();
	switch (size) {
		case 737280: d.cylinders = 80; d.heads = 2; d.sectors = 9; break;
		case 1228800: d.cylinders = 80; d.heads = 2; d.sectors = 15; break;
		case 1474560: d.cylinders = 80; d.heads = 2; d.sectors = 18; break;
		case 2949120: d.cylinders = 80; d.heads = 2; d.sectors = 36; break;
		case 368640: d.cylinders = 40; d.heads = 2; d.sectors = 9; break;
		default:
			error = "unsupported floppy image size";
			return false;
	}
	d.writeProtected = writeProtected || image->IsReadOnly();
	d.image = std::move(image);
	d.diskChanged = true;
	return true;
}


void
Fdc82077::EjectDisk(int drive)
{
	fDrives[drive].image.reset();
	fDrives[drive].diskChanged = true;
}


int
Fdc82077::_CommandLength(u8 command) const
{
	switch (command & 0x1f) {
		case 0x02: case 0x05: case 0x06: case 0x09: case 0x0c: case 0x16:
		case 0x11: case 0x19: case 0x1d:
			return 9;
		case 0x03: return 3;
		case 0x04: return 2;
		case 0x07: return 2;
		case 0x08: return 1;
		case 0x0a: return 2;
		case 0x0d: return 6;
		case 0x0e: return 1;
		case 0x0f: return 3;
		case 0x10: return 1;
		case 0x12: return 2;
		case 0x13: return 4;
		case 0x14: return 1;
		case 0x18: return 1;
	}
	return 1;
}


void
Fdc82077::_Result(std::initializer_list<u8> bytes, bool irq)
{
	fResult.assign(bytes);
	fResultPosition = 0;
	fPhase = kResult;
	if (irq)
		_SetIrq(true);
}


u8
Fdc82077::IoRead8(u32 port)
{
	switch (port) {
		case 0x3f0:
		case 0x3f1:
			return 0xff;	// status registers A/B (PS/2 mode only)
		case 0x3f2:
			return fDor;
		case 0x3f3:
			return 0;
		case 0x3f4:
		{
			u8 msr = 0;
			if ((fDor & kDorReset) == 0)
				return 0;
			switch (fPhase) {
				case kCommand:
					msr = kMsrRqm | (fCommand.empty() ? 0 : kMsrBusy);
					break;
				case kExecution:
					msr = kMsrBusy;
					if (fNonDma && fPioPosition < fSectorBuffer.size())
						msr |= kMsrRqm | kMsrNonDma
							| (fXferWrite ? 0 : kMsrDio);
					break;
				case kResult:
					msr = kMsrRqm | kMsrDio | kMsrBusy;
					break;
			}
			return msr;
		}
		case 0x3f5:
			if (fPhase == kResult) {
				// Reading the result clears the interrupt of data commands.
				_SetIrq(false);
				u8 value = fResultPosition < fResult.size()
					? fResult[fResultPosition++] : 0;
				if (fResultPosition >= fResult.size()) {
					fPhase = kCommand;
					fCommand.clear();
				}
				return value;
			}
			return 0xff;
		case 0x3f7:
		{
			int drive = fDor & 1;
			return fDrives[drive].diskChanged ? 0x80 : 0x00;
		}
	}
	return 0xff;
}


void
Fdc82077::IoWrite8(u32 port, u8 value)
{
	switch (port) {
		case 0x3f2:
		{
			u8 old = fDor;
			fDor = value;
			for (int drive = 0; drive < 2; drive++) {
				bool motor = (value & (0x10 << drive)) != 0;
				if (motor != fDrives[drive].motor) {
					fDrives[drive].motor = motor;
					if (fActivity)
						fActivity(drive, motor);
				}
			}
			if ((old & kDorReset) != 0 && (value & kDorReset) == 0)
				_ResetController(false);
			else if ((old & kDorReset) == 0 && (value & kDorReset) != 0)
				_ResetController(true);
			else
				_SetIrq(fIrqLevel);	// DMA/IRQ gate may have changed
			return;
		}
		case 0x3f4:		// data rate select
			fDataRate = value & 3;
			if ((value & 0x80) != 0)
				_ResetController(true);
			return;
		case 0x3f7:		// configuration control
			fDataRate = value & 3;
			return;
		case 0x3f5:
			break;
		default:
			return;
	}

	if (fPhase == kExecution && fNonDma && fXferWrite) {
		if (fPioPosition < fSectorBuffer.size())
			fSectorBuffer[fPioPosition++] = value;
		return;
	}
	if (fPhase != kCommand)
		return;
	fCommand.push_back(value);
	if ((int)fCommand.size() >= _CommandLength(fCommand[0]))
		_StartCommand();
}


void
Fdc82077::_StartCommand()
{
	u8 command = fCommand[0];
	int drive = fCommand.size() > 1 ? fCommand[1] & 1 : 0;
	Drive& d = fDrives[drive];
	BB_LOG(kLogFloppy, kLogDebug, "command %02x (%zu bytes)", command,
		fCommand.size());

	switch (command & 0x1f) {
		case 0x03:		// specify
			fSpecify[0] = fCommand[1];
			fSpecify[1] = fCommand[2];
			fNonDma = (fCommand[2] & 1) != 0;
			fPhase = kCommand;
			fCommand.clear();
			return;

		case 0x04:		// sense drive status
		{
			int head = (fCommand[1] >> 2) & 1;
			u8 st3 = 0x20 | 0x08 | (head << 2) | drive;	// ready, 2 sides
			if (d.cylinder == 0)
				st3 |= 0x10;
			if (d.image == nullptr || d.writeProtected)
				st3 |= 0x40;
			_Result({ st3 }, false);
			return;
		}

		case 0x07:		// recalibrate
		case 0x0f:		// seek
		{
			int target = (command & 0x1f) == 0x07 ? 0 : fCommand[2];
			int distance = target > d.cylinder ? target - d.cylinder
				: d.cylinder - target;
			bool stepped = distance > 0;
			d.cylinder = target;
			if (d.image != nullptr && stepped)
				d.diskChanged = false;
			fPhase = kExecution;
			fCommand.clear();
			fTimerAction = [this, drive, target]() {
				fSeekStatus[drive] = kSt0SeekEnd | drive;
				fSeekPending[drive] = true;
				fPhase = kCommand;
				(void)target;
				_SetIrq(true);
			};
			fTimer->ArmIn(_Ms(1) + _Ms(distance) / 2);
			return;
		}

		case 0x08:		// sense interrupt status
			// The interrupt line drops with the first sense interrupt.
			_SetIrq(false);
			if (fResetSenses > 0) {
				int polled = 4 - fResetSenses;
				fResetSenses--;
				_Result({ (u8)(0xc0 | polled),
					(u8)fDrives[polled & 1].cylinder }, false);
				return;
			}
			for (int i = 0; i < 4; i++) {
				if (!fSeekPending[i])
					continue;
				fSeekPending[i] = false;
				_Result({ fSeekStatus[i], (u8)fDrives[i & 1].cylinder },
					false);
				_SetIrq(false);
				return;
			}
			_Result({ kSt0Invalid }, false);
			return;

		case 0x0a:		// read ID
		{
			int head = (fCommand[1] >> 2) & 1;
			if (d.image == nullptr) {
				fPhase = kExecution;
				fTimerAction = [this, drive, head]() {
					_Result({ (u8)(kSt0AbnormalTermination | (head << 2)
						| drive), kSt1MissingAddressMark, 0, 0, 0, 0, 0 },
						true);
				};
				fTimer->ArmIn(_Ms(200));
				return;
			}
			fPhase = kExecution;
			fTimerAction = [this, drive, head]() {
				_Result({ (u8)((head << 2) | drive), 0, 0,
					(u8)fDrives[drive].cylinder, (u8)head, 1, 2 }, true);
			};
			fTimer->ArmIn(_Ms(2));
			return;
		}

		case 0x06: case 0x05: case 0x0c: case 0x09: case 0x02: case 0x16:
		case 0x0d:
		{
			fXferDrive = drive;
			fXferHead = (fCommand[1] >> 2) & 1;
			fXferMultiTrack = (command & 0x80) != 0;
			fXferWrite = (command & 0x1f) == 0x05 || (command & 0x1f) == 0x09;
			fXferFormat = (command & 0x1f) == 0x0d;
			if (fXferFormat) {
				fXferCylinder = d.cylinder;
				fXferSector = 1;
				fXferEnd = fCommand[3];		// sectors per track
			} else {
				fXferCylinder = fCommand[2];
				fXferHead = fCommand[3] & 1;
				fXferSector = fCommand[4];
				fXferEnd = fCommand[6];
			}
			fPhase = kExecution;
			fPioPosition = 0;
			// Without a disk (or at the wrong data rate) the controller
			// never sees an ID field and fails after two revolutions.
			int requiredRate = d.sectors >= 36 ? 3 : d.sectors >= 15 ? 0 : 2;
			if (d.image == nullptr || fDataRate != requiredRate) {
				int head = fXferHead;
				fTimerAction = [this, drive, head]() {
					_FinishTransfer((u8)(kSt0AbnormalTermination | (head << 2)
						| drive), kSt1MissingAddressMark, 0);
				};
				fTimer->ArmIn(_Ms(200));
				return;
			}
			if ((fXferWrite || fXferFormat) && d.writeProtected) {
				int head = fXferHead;
				fTimerAction = [this, drive, head]() {
					_FinishTransfer((u8)(kSt0AbnormalTermination | (head << 2)
						| drive), kSt1NotWritable, 0);
				};
				fTimer->ArmIn(_Ms(2));
				return;
			}
			if (fActivity)
				fActivity(drive, true);
			fTimerAction = [this]() { _ExecuteTransfer(); };
			fTimer->ArmIn(_Ms(2));
			return;
		}

		case 0x0e:		// dumpreg
			_Result({ (u8)fDrives[0].cylinder, (u8)fDrives[1].cylinder, 0, 0,
				fSpecify[0], fSpecify[1], 0, fPerpendicular,
				(u8)((fLocked ? 0x80 : 0) | fConfigure), fPrecomp }, false);
			return;
		case 0x10:		// version
			_Result({ 0x90 }, false);
			return;
		case 0x12:		// perpendicular mode
			fPerpendicular = fCommand[1];
			fPhase = kCommand;
			fCommand.clear();
			return;
		case 0x13:		// configure
			fConfigure = fCommand[2];
			fPrecomp = fCommand[3];
			fPhase = kCommand;
			fCommand.clear();
			return;
		case 0x14:		// lock / unlock
			fLocked = (command & 0x80) != 0;
			_Result({ (u8)(fLocked ? 0x10 : 0) }, false);
			return;
		case 0x18:		// part ID
			_Result({ 0x41 }, false);
			return;
	}
	BB_LOG(kLogFloppy, kLogInfo, "invalid command %02x", command);
	_Result({ kSt0Invalid }, false);
}


void
Fdc82077::_ExecuteTransfer()
{
	Drive& d = fDrives[fXferDrive];
	u8 st0 = (u8)((fXferHead << 2) | fXferDrive);

	if (fXferFormat) {
		// Four ID bytes per sector come through DMA; the track is filled.
		u8 fill = fCommand[5];
		int count = fCommand[3];
		for (int i = 0; i < count; i++) {
			u8 id[4];
			bool terminal;
			if (fDma.Transfer(2, id, 4, terminal) < 4)
				break;
			int sector = id[2];
			if (sector < 1 || sector > d.sectors)
				continue;
			u64 lba = ((u64)fXferCylinder * d.heads + fXferHead) * d.sectors
				+ sector - 1;
			std::vector<u8> data(kSectorSize, fill);
			d.image->Write(lba * kSectorSize, data.data(), kSectorSize);
		}
		_FinishTransfer(st0, 0, 0);
		return;
	}

	if (fNonDma) {
		BB_LOG(kLogFloppy, kLogInfo, "non-DMA transfers are not supported");
		_FinishTransfer(st0 | kSt0AbnormalTermination, kSt1NoData, 0);
		return;
	}

	// The DMA controller must be ready; poll a little if it is not yet.
	if (fDma.IsMasked(2)) {
		fTimerAction = [this]() { _ExecuteTransfer(); };
		fTimer->ArmIn(_Ms(1));
		return;
	}

	bool verify = (fCommand[0] & 0x1f) == 0x16;
	int sectorsMoved = 0;
	while (true) {
		if (fXferCylinder >= d.cylinders || fXferSector < 1
			|| fXferSector > d.sectors || fXferHead >= d.heads) {
			_FinishTransfer(st0 | kSt0AbnormalTermination, kSt1NoData, 0);
			return;
		}
		u64 lba = ((u64)fXferCylinder * d.heads + fXferHead) * d.sectors
			+ fXferSector - 1;
		bool terminal = false;
		u8* buffer = fSectorBuffer.data();
		if (fXferWrite) {
			u32 moved = fDma.Transfer(2, buffer, kSectorSize, terminal);
			if (moved < kSectorSize) {
				_FinishTransfer(st0 | kSt0AbnormalTermination, 0x10, 0);
				return;
			}
			d.image->Write(lba * kSectorSize, buffer, kSectorSize);
		} else {
			d.image->Read(lba * kSectorSize, buffer, kSectorSize);
			if (!verify) {
				u32 moved = fDma.Transfer(2, buffer, kSectorSize, terminal);
				if (moved < kSectorSize)
					terminal = true;
			}
		}
		sectorsMoved++;
		// next sector
		bool endOfTrack = fXferSector >= fXferEnd;
		if (endOfTrack) {
			fXferSector = 1;
			if (fXferMultiTrack && fXferHead == 0) {
				fXferHead = 1;
			} else {
				fXferHead = fXferMultiTrack ? 0 : fXferHead;
				fXferCylinder++;
				if (!terminal) {
					// end of cylinder without terminal count
					fTimerAction = [this, st0]() {
						_FinishTransfer(st0 | kSt0AbnormalTermination,
							kSt1EndOfCylinder, 0);
					};
					fTimer->ArmIn(_Ms(2) * sectorsMoved);
					return;
				}
			}
		} else
			fXferSector++;
		if (terminal || verify)
			break;
	}
	fTimerAction = [this, st0]() { _FinishTransfer(st0, 0, 0); };
	fTimer->ArmIn(_Ms(2) * sectorsMoved);
}


void
Fdc82077::_FinishTransfer(u8 st0, u8 st1, u8 st2)
{
	if (fActivity)
		fActivity(fXferDrive, fDrives[fXferDrive].motor);
	u8 n = fCommand.size() > 5 ? fCommand[5] : 2;
	_Result({ st0, st1, st2, (u8)fXferCylinder, (u8)fXferHead,
		(u8)fXferSector, n }, true);
}

}	// namespace bebox
