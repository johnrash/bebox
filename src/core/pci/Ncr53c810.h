/*
 * BeBox emulator - NCR 53C810 PCI SCSI I/O processor.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The BeBox's built-in SCSI controller (PCI device 12, Be interrupt bit
 * 10). Registers through I/O BAR0 or memory BAR1 (little endian), a
 * SCRIPTS processor fetching instructions and moving data as a PCI bus
 * master, and an emulated narrow SCSI bus with target devices on IDs 0-6
 * (the controller is ID 7).
 */
#ifndef BEBOX_NCR_53C810_H
#define BEBOX_NCR_53C810_H

#include "ScsiDevice.h"
#include "../board/Scheduler.h"
#include "../bus/IoSpace.h"
#include "../bus/PciBus.h"
#include "../bus/PhysBus.h"

#include <functional>
#include <memory>
#include <vector>

namespace bebox {

class Mpc105;

class Ncr53c810 : public PciDevice {
public:
	typedef std::function<void(bool)> IrqCallback;

								Ncr53c810(PhysBus& bus, IoSpace& io,
									Mpc105& bridge, Scheduler& scheduler,
									u64 ticksPerSecond);
								~Ncr53c810();

			void				Reset();
			void				SetIrqCallback(IrqCallback callback)
									{ fIrq = callback; }
			void				AttachDevice(int id,
									std::unique_ptr<ScsiDevice> device);
			ScsiDevice*			Device(int id) const
									{ return fDevices[id].get(); }

			// Register access (little endian, offset 0-0x7f).
			u8					ReadRegister(u32 offset);
			void				WriteRegister(u32 offset, u8 value);

	// PciDevice
	virtual	const char*			PciName() const { return "ncr53c810"; }

protected:
	virtual	void				BarsChanged();

private:
	enum Phase {
		kPhaseDataOut = 0,
		kPhaseDataIn = 1,
		kPhaseCommand = 2,
		kPhaseStatus = 3,
		kPhaseMessageOut = 6,
		kPhaseMessageIn = 7,
		kPhaseBusFree = 8
	};

	enum WaitState {
		kNotWaiting,
		kWaitSelection,		// selection timeout pending
		kWaitReselect		// WAIT RESELECT (no target reconnects)
	};

	class IoPorts;
	class Registers;

			// SCSI bus / target side
			void				_BusReset();
			bool				_Select(int id, bool attention);
			void				_TargetNextPhase();
			void				_TargetReceive(u8 byte);
			bool				_TargetSend(u8& byte);
			void				_TargetAckReleased();
			void				_Disconnect();

			// SCRIPTS
			void				_Start();
			void				_Run();
			bool				_Step();
			bool				_BlockMove(u32 dcmd, u32 count, u32 address);
			bool				_IoInstruction(u32 first, u32 second);
			bool				_ReadWrite(u32 first);
			bool				_TransferControl(u32 first, u32 second);
			bool				_MemoryMove(u32 first, u32 source,
									u32 destination);
			u32					_Fetch32(u32 address);
			void				_Read(u32 address, void* buffer, u32 length);
			void				_Write(u32 address, const void* buffer,
									u32 length);
			bool				_InOurRegisters(u32 address) const;
			u32					_RegisterOffset(u32 address) const;

			// interrupts
			void				_DmaInterrupt(u8 dstat);
			void				_ScsiInterrupt(u8 sist0, u8 sist1);
			void				_UpdateIrq();
			bool				_Halted() const { return !fRunning; }

			u32					_Reg32(u32 offset) const;
			void				_SetReg32(u32 offset, u32 value);

			PhysBus&			fBus;
			IoSpace&			fIo;
			Mpc105&				fBridge;
			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			IrqCallback			fIrq;
			IoPorts*			fIoPorts;
			Registers*			fMmio;
			u32					fIoBase;		// 0: not mapped
			u32					fMemoryBase;

			u8					fRegs[0x80];
			u8					fDstat;
			u8					fSist0;
			u8					fSist1;
			bool				fCarry;
			bool				fRunning;
			WaitState			fWait;
			bool				fIrqLevel;
			std::unique_ptr<Timer> fTimer;
			std::unique_ptr<Timer> fSelectionTimer;

			// SCSI bus
			std::unique_ptr<ScsiDevice> fDevices[8];
			int					fTarget;		// connected target or -1
			Phase				fPhase;
			bool				fAttention;
			bool				fAckHeld;
			ScsiCommand			fCommand;
			std::vector<u8>		fMessageOut;
			std::vector<u8>		fMessageIn;
			size_t				fMessageInPosition;
			bool				fDisconnectAfterMessage;
			bool				fCommandReceived;
			u32					fCdbExpected;
			size_t				fDataPosition;
			std::vector<u8>		fDataOut;
};

}	// namespace bebox

#endif	// BEBOX_NCR_53C810_H
