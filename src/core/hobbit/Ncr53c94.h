/*
 * BeBox emulator - NCR 53C94 SCSI controller (initiator role) with the
 * Hobbit board's 16-bit pseudo-DMA port.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The 53C9x family: a 16 byte FIFO, a transfer counter and a command
 * register driving the bus phases (select, transfer information, initiator
 * command complete, message accepted). Register 0x10 here is the pseudo-
 * DMA port: each 16-bit access moves the next two bytes of a DMA transfer
 * (the board holds the processor until the chip has them).
 *
 * Targets are ScsiDevices: a command executes in one step once its CDB
 * has arrived.
 */
#ifndef BEBOX_NCR53C94_H
#define BEBOX_NCR53C94_H

#include "../board/Scheduler.h"
#include "../bus/IoSpace.h"
#include "../pci/ScsiDevice.h"
#include "../Types.h"

#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace bebox {

class Ncr53c94 : public IoDevice {
public:
	typedef std::function<void(bool)> IrqCallback;

	static const u32 kPseudoDmaPort = 0x10;

								Ncr53c94(Scheduler& scheduler,
									u64 ticksPerSecond, u32 clockHz);
								~Ncr53c94();

			void				Reset();
			void				SetIrqCallback(IrqCallback callback)
									{ fIrq = callback; }
			void				AttachDevice(int id,
									std::unique_ptr<ScsiDevice> device);

			// Registers 0-15 (bytes) and the pseudo-DMA port (16 bits).
	virtual	u32					IoRead(u32 port, int size);
	virtual	void				IoWrite(u32 port, u32 value, int size);
	virtual	const char*			IoName() const { return "53c94"; }

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

			void				_Command(u8 command);
			void				_Select(bool attention);
			void				_TransferInformation(bool dma);
			void				_InitiatorCommandComplete();
			void				_MessageAccepted();
			void				_ResetBus();
			// The phase after the data phase (or the command phase).
			void				_DataDone();
			void				_RaiseInterrupt(u8 isr, u64 delay);
			void				_DeliverInterrupt();
			void				_UpdateIrq();
			u32					_TransferCount() const;
			u8					_PseudoDmaRead();
			void				_PseudoDmaWrite(u8 value);
			void				_CountDown();
			u8					_FifoFlags() const;

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			u32					fClockHz;
			IrqCallback			fIrq;
			std::unique_ptr<ScsiDevice> fDevices[8];

			std::deque<u8>		fFifo;
			u16					fStartCount;		// STC
			u32					fCount;				// current TC
			bool				fDmaActive;
			u8					fStatus;
			u8					fInterruptStatus;
			u8					fSequenceStep;
			u8					fDestination;
			u8					fTimeout;
			u8					fConfig1;
			u8					fConfig2;
			u8					fConfig3;
			u8					fClockFactor;
			u8					fCommandRegister;

			Phase				fPhase;
			int					fTarget;
			ScsiCommand			fCommand;
			size_t				fDataInPosition;
			std::vector<u8>		fDataOut;

			// interrupt raised later (selection, bus reset)
			u8					fPendingIsr;
			int					fExtraResetInterrupts;
			std::unique_ptr<Timer> fInterruptTimer;
};

}	// namespace bebox

#endif	// BEBOX_NCR53C94_H
