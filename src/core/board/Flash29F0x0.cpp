/*
 * BeBox emulator - AMD Am29F010 / Am29F040 flash memory (boot ROM).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Flash29F0x0.h"

#include "../Log.h"

namespace bebox {

Flash29F0x0::Flash29F0x0(PhysBus& bus, Type type, u32 base, u32 window)
	:
	fBus(bus),
	fType(type),
	fBase(base),
	fWindow(window),
	fState(kReadArray),
	fProtected(1),		// the nub's sector
	fDirty(false),
	fArrayMapped(false)
{
	u32 size;
	if (type == kAm29F010) {
		size = 128 * 1024;
		fSectorSize = 16 * 1024;
		fManufacturer = 0x01;
		fDevice = 0x20;
	} else {
		size = 512 * 1024;
		fSectorSize = 64 * 1024;
		fManufacturer = 0x01;
		fDevice = 0xa4;
	}
	fData.assign(size, 0xff);
	_MapArray(true);
}


void
Flash29F0x0::Reset()
{
	_SetState(kReadArray);
}


void
Flash29F0x0::_MapArray(bool array)
{
	if (array == fArrayMapped)
		return;
	fArrayMapped = array;
	u32 size = (u32)fData.size();
	for (u32 offset = 0; offset < fWindow; offset += size) {
		if (array)
			fBus.MapMemory(fBase + offset, size, fData.data(), false, this);
		else
			fBus.MapDevice(fBase + offset, size, this);
	}
}


void
Flash29F0x0::_SetState(State state)
{
	fState = state;
	_MapArray(state != kAutoselect);
}


bool
Flash29F0x0::_SectorProtected(u32 offset) const
{
	u32 sector = offset / fSectorSize;
	return (fProtected & (1u << sector)) != 0;
}


u8
Flash29F0x0::_ReadByte(u32 offset)
{
	if (fState == kAutoselect) {
		switch (offset & 0xff) {
			case 0: return fManufacturer;
			case 1: return fDevice;
			case 2: return _SectorProtected(offset) ? 1 : 0;
		}
		return 0;
	}
	return fData[offset];
}


u32
Flash29F0x0::MmioRead(u32 addr, int size, int initiator)
{
	u32 offset = (addr - fBase) % (u32)fData.size();
	u32 value = 0;
	for (int i = 0; i < size; i++)
		value = (value << 8) | _ReadByte((offset + i) % (u32)fData.size());
	return value;
}


void
Flash29F0x0::MmioWrite(u32 addr, u32 value, int size, int initiator)
{
	if (fWriteEnable && !fWriteEnable()) {
		BB_LOG(kLogFlash, kLogInfo,
			"flash write while disabled (MPC105 PICR1) at %08x", addr);
		return;
	}
	u32 offset = (addr - fBase) % (u32)fData.size();
	// The flash is eight bits wide; wider stores reach it a byte at a time.
	for (int i = 0; i < size; i++)
		_WriteByte((offset + i) % (u32)fData.size(),
			(u8)(value >> (8 * (size - 1 - i))));
}


void
Flash29F0x0::_WriteByte(u32 offset, u8 value)
{
	u32 command = offset & 0x7fff;
	if (value == 0xf0 && fState != kProgram) {
		_SetState(kReadArray);
		return;
	}
	switch (fState) {
		case kReadArray:
		case kAutoselect:
			if (command == 0x5555 && value == 0xaa)
				fState = kCycle1;
			else if (fState != kAutoselect)
				BB_LOG(kLogFlash, kLogDebug, "flash: stray write %02x at %05x",
					value, offset);
			break;
		case kCycle1:
			fState = command == 0x2aaa && value == 0x55 ? kCycle2 : kReadArray;
			break;
		case kCycle2:
			if (command != 0x5555) {
				_SetState(kReadArray);
				break;
			}
			switch (value) {
				case 0xa0: fState = kProgram; break;
				case 0x80: fState = kErase1; break;
				case 0x90: _SetState(kAutoselect); break;
				default: _SetState(kReadArray); break;
			}
			break;
		case kProgram:
			if (_SectorProtected(offset)) {
				BB_LOG(kLogFlash, kLogInfo,
					"flash: program in protected sector at %05x", offset);
			} else {
				fData[offset] &= value;	// bits only go from 1 to 0
				fDirty = true;
			}
			_SetState(kReadArray);
			break;
		case kErase1:
			fState = command == 0x5555 && value == 0xaa ? kErase2 : kReadArray;
			break;
		case kErase2:
			fState = command == 0x2aaa && value == 0x55 ? kErase3 : kReadArray;
			break;
		case kErase3:
			if (value == 0x10 && command == 0x5555) {
				for (u32 sector = 0; sector < fData.size() / fSectorSize;
						sector++) {
					if ((fProtected & (1u << sector)) == 0) {
						memset(&fData[sector * fSectorSize], 0xff,
							fSectorSize);
					}
				}
				fDirty = true;
				BB_LOG(kLogFlash, kLogInfo, "flash: chip erase");
			} else if (value == 0x30) {
				u32 sector = offset / fSectorSize;
				if (_SectorProtected(offset)) {
					BB_LOG(kLogFlash, kLogInfo,
						"flash: erase of protected sector %u", sector);
				} else {
					memset(&fData[sector * fSectorSize], 0xff, fSectorSize);
					fDirty = true;
					BB_LOG(kLogFlash, kLogInfo, "flash: sector %u erased",
						sector);
				}
			}
			_SetState(kReadArray);
			break;
	}
}

}	// namespace bebox
