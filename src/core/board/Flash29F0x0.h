/*
 * BeBox emulator - AMD Am29F010 / Am29F040 flash memory (boot ROM).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The BeBox boot ROM lives in flash at 0xfff00000: the "nub" in the first
 * (write protected) sector and the main ROM at 0xfff04000. Revision 5
 * boards have 128 KB (Am29F010, 16 KB sectors); later boards need a larger
 * part for newer ROM images (Am29F040, 512 KB, 64 KB sectors). Programming
 * and erasing complete immediately. Writes need MPC105 PICR1[FLASH_WR_EN].
 */
#ifndef BEBOX_FLASH_29F0X0_H
#define BEBOX_FLASH_29F0X0_H

#include "../bus/PhysBus.h"

#include <functional>
#include <vector>

namespace bebox {

class Flash29F0x0 : public MmioDevice {
public:
	enum Type {
		kAm29F010,
		kAm29F040
	};
	typedef std::function<bool()> WriteEnable;

								Flash29F0x0(PhysBus& bus, Type type,
									u32 base, u32 window);

			void				SetWriteEnable(WriteEnable callback)
									{ fWriteEnable = callback; }
			void				SetProtectedSectors(u32 mask)
									{ fProtected = mask; }
			void				Reset();

			u8*					Data() { return fData.data(); }
			u32					Size() const { return (u32)fData.size(); }
			u32					SectorSize() const { return fSectorSize; }
			bool				IsDirty() const { return fDirty; }
			void				ClearDirty() { fDirty = false; }
			Type				GetType() const { return fType; }

	// MmioDevice
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "flash"; }

private:
	enum State {
		kReadArray,
		kCycle1,		// AA at 5555
		kCycle2,		// 55 at 2AAA
		kProgram,		// A0: next write programs
		kErase1,		// 80
		kErase2,		// AA
		kErase3,		// 55, then 30 (sector) or 10 (chip)
		kAutoselect
	};

			void				_WriteByte(u32 offset, u8 value);
			void				_SetState(State state);
			void				_MapArray(bool array);
			u8					_ReadByte(u32 offset);
			bool				_SectorProtected(u32 offset) const;

			PhysBus&			fBus;
			Type				fType;
			u32					fBase;
			u32					fWindow;
			u32					fSectorSize;
			u8					fManufacturer;
			u8					fDevice;
			std::vector<u8>		fData;
			State				fState;
			u32					fProtected;		// bit per sector
			bool				fDirty;
			bool				fArrayMapped;
			WriteEnable			fWriteEnable;
};

}	// namespace bebox

#endif	// BEBOX_FLASH_29F0X0_H
