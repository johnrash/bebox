/*
 * BeBox emulator - floppy disk controller (82077AA compatible, part of
 * the 82091AA) with 3.5" drives.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Ports 0x3f0-0x3f5 and 0x3f7, IRQ6 (Be bit 18), ISA DMA channel 2.
 * Images: 720 KB, 1.44 MB or 2.88 MB raw sector dumps.
 */
#ifndef BEBOX_FDC_82077_H
#define BEBOX_FDC_82077_H

#include "../bus/IoSpace.h"
#include "../board/Scheduler.h"
#include "../storage/DiskImage.h"

#include <functional>
#include <memory>
#include <vector>

namespace bebox {

class DmaChannels;

class Fdc82077 : public ByteIoDevice {
public:
	typedef std::function<void(bool)> IrqCallback;
	typedef std::function<void(int drive, bool active)> ActivityCallback;

								Fdc82077(Scheduler& scheduler,
									u64 ticksPerSecond, DmaChannels& dma);
								~Fdc82077();

			void				Reset();
			void				SetIrqCallback(IrqCallback callback)
									{ fIrq = callback; }
			void				SetActivityCallback(ActivityCallback callback)
									{ fActivity = callback; }
			// The 82077 polls the drives after a reset: it interrupts,
			// and four sense interrupt results report the drives. The
			// Hobbit's controller does neither (its kernels have no
			// handler for that interrupt early on).
			void				SetResetPolling(bool polling)
									{ fResetPolling = polling; }

			// Inserts or ejects (null) a disk. Returns false if the image
			// has an unsupported size.
			bool				InsertDisk(int drive,
									std::unique_ptr<DiskImage> image,
									bool writeProtected, std::string& error);
			void				EjectDisk(int drive);
			bool				HasDisk(int drive) const
									{ return fDrives[drive].image != nullptr; }

	virtual	u8					IoRead8(u32 port);
	virtual	void				IoWrite8(u32 port, u8 value);
	virtual	const char*			IoName() const { return "fdc"; }

private:
	struct Drive {
		std::unique_ptr<DiskImage> image;
		bool writeProtected = false;
		bool writeFailing = false;	// logged once until it works
		int cylinders = 80;
		int heads = 2;
		int sectors = 18;
		int cylinder = 0;		// head position
		bool diskChanged = true;
		bool motor = false;
	};

	enum Phase {
		kCommand,
		kExecution,
		kResult
	};

			void				_ResetController(bool fromDor);
			void				_SetIrq(bool level);
			void				_StartCommand();
			void				_ExecuteTransfer();
			void				_FinishTransfer(u8 st0, u8 st1, u8 st2);
			bool				_WriteSector(Drive& d, u64 lba,
									const u8* data);
			void				_Result(std::initializer_list<u8> bytes,
									bool irq);
			int					_CommandLength(u8 command) const;
			u64					_Ms(u64 ms) const
									{ return ms * fTicksPerSecond / 1000; }

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			DmaChannels&		fDma;
			bool				fResetPolling = true;
			IrqCallback			fIrq;
			ActivityCallback	fActivity;
			Drive				fDrives[2];

			u8					fDor;
			u8					fDataRate;
			Phase				fPhase;
			std::vector<u8>		fCommand;
			std::vector<u8>		fResult;
			size_t				fResultPosition;
			bool				fIrqLevel;
			int					fResetSenses;	// pending sense interrupts
			u8					fSeekStatus[4];	// ST0 for sense interrupt
			bool				fSeekPending[4];
			bool				fNonDma;
			u8					fSpecify[2];
			u8					fConfigure;
			u8					fPrecomp;
			bool				fLocked;
			u8					fPerpendicular;

			// current transfer
			int					fXferDrive;
			int					fXferCylinder;
			int					fXferHead;
			int					fXferSector;
			int					fXferEnd;		// EOT
			bool				fXferMultiTrack;
			bool				fXferWrite;
			bool				fXferFormat;
			std::vector<u8>		fSectorBuffer;
			u32					fPioPosition;
			std::unique_ptr<Timer> fTimer;
			std::function<void()> fTimerAction;
};

}	// namespace bebox

#endif	// BEBOX_FDC_82077_H
