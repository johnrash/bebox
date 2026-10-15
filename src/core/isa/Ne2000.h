/*
 * BeBox emulator - NE2000 compatible ISA Ethernet card.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The card the BeBox documentation recommends (DR8 User's Guide: "NE2000,
 * 3Com 3C503, DEC 21040 or 21041"; Be's networking defaults to port 300h,
 * IRQ 5). A National DP8390 core with the NE2000's 16 KB of buffer memory
 * (pages 40h-7Fh) in 16-bit mode, the station address PROM at 0, remote
 * DMA through the data port (base + 10h) and the reset port (base + 1Fh).
 * Frames go to and come from a NetworkBackend.
 */
#ifndef BEBOX_NE2000_H
#define BEBOX_NE2000_H

#include "../board/Scheduler.h"
#include "../bus/IoSpace.h"

#include <functional>
#include <memory>

namespace bebox {

class NetworkBackend;

class Ne2000 : public IoDevice {
public:
	typedef std::function<void(bool level)> IrqCallback;

	static const u32 kPorts = 0x20;

								Ne2000(Scheduler& scheduler,
									u64 ticksPerSecond, const u8 mac[6]);
								~Ne2000();

			void				Reset();
			void				SetIrqCallback(IrqCallback callback)
									{ fIrq = callback; }
			void				SetBackend(NetworkBackend* backend);

			// A frame from the network; false when the receive ring is
			// full (the backend keeps it and retries).
			bool				Receive(const u8* frame, size_t length);

	virtual	u32					IoRead(u32 port, int size);
	virtual	void				IoWrite(u32 port, u32 value, int size);
	virtual	const char*			IoName() const { return "ne2000"; }

private:
			u8					_ReadRegister(int offset);
			void				_WriteRegister(int offset, u8 value);
			void				_Command(u8 value);
			void				_Transmit();
			u16					_DataRead(int size);
			void				_DataWrite(u16 value, int size);
			void				_AdvanceDma(int count);
			bool				_Accepts(const u8* frame) const;
			int					_FreePages() const;
			u8&					_Memory(u32 address);
			void				_UpdateIrq();
			void				_Poll();

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			IrqCallback			fIrq;
			NetworkBackend*		fBackend;
			std::unique_ptr<Timer> fPollTimer;

			u8					fMac[6];
			u8					fProm[32];
			u8					fRam[16384];	// 0x4000-0x7fff

			u8					fCommand;		// CR
			u8					fPageStart;		// PSTART
			u8					fPageStop;		// PSTOP
			u8					fBoundary;		// BNRY
			u8					fCurrent;		// CURR
			u8					fTransmitPage;	// TPSR
			u16					fTransmitCount;	// TBCR
			u8					fIsr;
			u8					fImr;
			u16					fRemoteAddress;	// RSAR (CRDA)
			u16					fRemoteCount;	// RBCR
			u8					fRcr;
			u8					fTcr;
			u8					fDcr;
			u8					fTsr;
			u8					fRsr;
			u8					fPar[6];
			u8					fMar[8];
			u8					fCounters[3];
			bool				fIrqLevel;
};

}	// namespace bebox

#endif	// BEBOX_NE2000_H
