/*
 * BeBox emulator - Motorola MPC105 "Eagle" PCI bridge / memory controller.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Address map A (PReP), contiguous I/O mode:
 *   0x00000000-0x3fffffff  system memory (eight banks)
 *   0x80000000-0x807fffff  ISA / PCI I/O 0x000000-0x7fffff
 *   0x80800000-0x80ffffff  direct PCI configuration (IDSEL = address bit)
 *   0x81000000-0xbf7fffff  PCI I/O
 *   0xbffffff0-0xbfffffff  interrupt acknowledge
 *   0xc0000000-0xfeffffff  PCI memory 0x00000000-0x3effffff
 *   0xff000000-0xffffffff  ROM
 * PCI bus masters see system memory at 0x80000000 + physical address.
 */
#ifndef BEBOX_MPC105_H
#define BEBOX_MPC105_H

#include "IoSpace.h"
#include "PciBus.h"
#include "PhysBus.h"

#include <functional>

namespace bebox {

class Mpc105 : public PciDevice, public IoDevice {
public:
	static const u32 kPciMemoryBase = 0xc0000000;
	static const u32 kPciMasterMemoryBase = 0x80000000;

	struct Bank {
		u8* host = nullptr;		// null: no SIMM in this bank
		u32 size = 0;
	};

	typedef std::function<u8()> InterruptAcknowledge;

								Mpc105(PhysBus& bus, IoSpace& io,
									PciBus& pci);
								~Mpc105();

			void				SetBank(int index, u8* host, u32 size);
			void				SetInterruptAcknowledge(
									InterruptAcknowledge callback)
									{ fIntAck = callback; }
			void				Reset();

			bool				FlashWritesEnabled() const
									{ return (LoadLE32(&fConfig[0xa8])
										& 0x1000) != 0; }

			// PCI bus master (DMA) access; 'address' is a PCI memory address.
			void				MasterRead(u32 address, void* buffer,
									u32 length);
			void				MasterWrite(u32 address, const void* buffer,
									u32 length);

	// PciDevice
	virtual	u32					ConfigRead(u32 reg, int size, int initiator);
	virtual	const char*			PciName() const { return "mpc105"; }

	// IoDevice (0xcf8-0xcff configuration mechanism #1)
	virtual	u32					IoRead(u32 port, int size);
	virtual	void				IoWrite(u32 port, u32 value, int size);
	virtual	const char*			IoName() const { return "mpc105-config"; }

protected:
	virtual	void				ConfigRegisterWritten(u32 reg);

private:
	class IoWindow;
	class ConfigWindow;
	class IntAckWindow;
	class EmptyBank;

			void				_UpdateMemoryMap();
			u32					_BankStart(int bank) const;
			u32					_BankEnd(int bank) const;

			PhysBus&			fBus;
			IoSpace&			fIo;
			PciBus&				fPci;
			Bank				fBanks[8];
			u32					fConfigAddress;
			u8					fMappedConfig[0x21];	// banks + MBEN (0x80-0xa0)
			InterruptAcknowledge fIntAck;
			IoWindow*			fIoWindow;
			ConfigWindow*		fConfigWindow;
			IntAckWindow*		fIntAckWindow;
			EmptyBank*			fEmptyBank;
};

}	// namespace bebox

#endif	// BEBOX_MPC105_H
