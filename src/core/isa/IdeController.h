/*
 * BeBox emulator - IDE interface of the 82091AA with ATA disks (and ATAPI
 * devices, see AtapiCd).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Ports 0x1f0-0x1f7 and 0x3f6 (device control / alternate status), IRQ14
 * (Be interrupt controller bit 24). PIO transfers.
 */
#ifndef BEBOX_IDE_CONTROLLER_H
#define BEBOX_IDE_CONTROLLER_H

#include "../bus/IoSpace.h"
#include "../board/Scheduler.h"
#include "../storage/DiskImage.h"

#include <functional>
#include <memory>
#include <vector>

namespace bebox {

class IdeController;

// The task file as seen by a device.
struct IdeTaskFile {
	u8 features = 0;
	u8 error = 0;
	u8 count = 0;
	u8 sector = 0;
	u8 cylinderLow = 0;
	u8 cylinderHigh = 0;
	u8 head = 0;		// drive/head register
	u8 status = 0;
	// HOB values for 48-bit commands (written before the current ones)
	u8 previousCount = 0;
	u8 previousSector = 0;
	u8 previousCylinderLow = 0;
	u8 previousCylinderHigh = 0;
};

enum {
	kIdeStatusError = 0x01,
	kIdeStatusDrq = 0x08,
	kIdeStatusSeekComplete = 0x10,
	kIdeStatusFault = 0x20,
	kIdeStatusReady = 0x40,
	kIdeStatusBusy = 0x80,

	kIdeErrorAbort = 0x04,
	kIdeErrorIdNotFound = 0x10,
	kIdeErrorUncorrectable = 0x40
};

// A device on the IDE channel.
class IdeDevice {
public:
	virtual						~IdeDevice() {}

			void				Attach(IdeController* controller, int unit)
									{ fController = controller;
									  fUnit = unit; }

	virtual	void				Reset() = 0;
	// Signature in the task file after a reset.
	virtual	void				SetSignature(IdeTaskFile& taskFile) = 0;
	virtual	void				Command(IdeTaskFile& taskFile, u8 command)
									= 0;
	// The host read all data of the current DRQ block.
	virtual	void				ReadBlockDone(IdeTaskFile& taskFile) {}
	// The host wrote a whole DRQ block (in the controller's buffer).
	virtual	void				WriteBlockDone(IdeTaskFile& taskFile) {}
	virtual	const char*			Kind() const = 0;
	virtual	bool				IsBusy() const { return false; }

protected:
			IdeController*		fController = nullptr;
			int					fUnit = 0;
};

class AtaDisk : public IdeDevice {
public:
								AtaDisk(std::unique_ptr<DiskImage> image);

			DiskImage*			Image() const { return fImage.get(); }

	virtual	void				Reset();
	virtual	void				SetSignature(IdeTaskFile& taskFile);
	virtual	void				Command(IdeTaskFile& taskFile, u8 command);
	virtual	void				ReadBlockDone(IdeTaskFile& taskFile);
	virtual	void				WriteBlockDone(IdeTaskFile& taskFile);
	virtual	const char*			Kind() const { return "ATA disk"; }

private:
			bool				_Address(IdeTaskFile& taskFile, u64& lba);
			void				_SetAddress(IdeTaskFile& taskFile, u64 lba);
			void				_ReadNext(IdeTaskFile& taskFile);
			void				_Identify(IdeTaskFile& taskFile);
			void				_Abort(IdeTaskFile& taskFile);

			std::unique_ptr<DiskImage> fImage;
			u64					fSectors;
			u32					fCylinders;
			u32					fHeads;
			u32					fSectorsPerTrack;
			u32					fLogicalHeads;
			u32					fLogicalSectors;
			u32					fMultiple;		// sectors per DRQ block
			u8					fCommand;
			u64					fLba;
			u32					fRemaining;		// sectors left
			bool				fLba48;
};

class IdeController : public IoDevice {
public:
	typedef std::function<void(bool)> IrqCallback;
	typedef std::function<void(bool)> ActivityCallback;

								IdeController(Scheduler& scheduler,
									u64 ticksPerSecond);
								~IdeController();

			void				Reset();
			void				SetIrqCallback(IrqCallback callback)
									{ fIrq = callback; }
			void				SetActivityCallback(ActivityCallback callback)
									{ fActivity = callback; }
			void				AttachDevice(int unit,
									std::unique_ptr<IdeDevice> device);
			IdeDevice*			Device(int unit) const
									{ return fDevices[unit].get(); }

	// Device services
			std::vector<u8>&	Buffer() { return fBuffer; }
			// Make 'length' bytes of the buffer available to the host
			// (read) or request them from the host (write); the interrupt
			// is raised after 'delay' ticks (or not at all).
			void				StartTransfer(u32 length, bool raiseIrq,
									u64 delay = 0);
			void				CompleteCommand(u8 status, bool raiseIrq,
									u64 delay = 0);
			// Busy for 'delay' ticks, then call 'then'.
			void				Defer(u64 delay, std::function<void()> then);
			void				SetActivity(bool active);
			u64					Ticks(u64 microseconds) const
									{ return microseconds * fTicksPerSecond
										/ 1000000; }
			IdeTaskFile&		TaskFile() { return fTaskFile; }

	virtual	u32					IoRead(u32 port, int size);
	virtual	void				IoWrite(u32 port, u32 value, int size);
	virtual	const char*			IoName() const { return "ide"; }

private:
			int					_Selected() const
									{ return (fTaskFile.head >> 4) & 1; }
			IdeDevice*			_SelectedDevice() const
									{ return fDevices[_Selected()].get(); }
			void				_SetIrq(bool pending);
			void				_UpdateIrqLine();
			u16					_ReadData();
			void				_WriteData(u16 value);
			void				_SoftReset();
			u32					_IoRead(u32 port, int size);

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			std::unique_ptr<IdeDevice> fDevices[2];
			IdeTaskFile			fTaskFile;
			u8					fControl;
			bool				fIrqPending;
			bool				fIrqLine;
			IrqCallback			fIrq;
			ActivityCallback	fActivity;
			std::vector<u8>		fBuffer;
			u32					fBufferPosition;
			u32					fBufferLength;
			bool				fWriting;
			std::unique_ptr<Timer> fTimer;
			std::function<void()> fPending;
};

}	// namespace bebox

#endif	// BEBOX_IDE_CONTROLLER_H
