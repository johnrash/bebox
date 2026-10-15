/*
 * BeBox emulator - SCSI target devices (disk and CD-ROM).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * A command is executed in one step: the bus model collects the CDB, the
 * device returns the data to send (data in) or the length it wants
 * (data out, then DataOut() with the bytes), plus the status byte.
 */
#ifndef BEBOX_SCSI_DEVICE_H
#define BEBOX_SCSI_DEVICE_H

#include "../Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bebox {

class CdImage;
class DiskImage;

enum {
	kScsiStatusGood = 0x00,
	kScsiStatusCheckCondition = 0x02,
	kScsiStatusBusy = 0x08
};

struct ScsiCommand {
	int lun = 0;
	u8 cdb[16];
	int cdbLength = 0;
	std::vector<u8> dataIn;		// device -> initiator
	u32 dataOutLength = 0;		// initiator -> device
	u8 status = kScsiStatusGood;
};

class ScsiDevice {
public:
	typedef std::function<void(bool)> ActivityCallback;

								ScsiDevice();
	virtual						~ScsiDevice() {}

	virtual	void				Reset();
			void				SetActivityCallback(ActivityCallback callback)
									{ fActivity = callback; }

			// Executes the command; fills dataIn or dataOutLength and status.
			void				Execute(ScsiCommand& command);
			// The data out bytes have arrived.
			void				DataOut(ScsiCommand& command,
									const std::vector<u8>& data);

	virtual	const char*			Kind() const = 0;

protected:
	virtual	bool				ExecuteSpecific(ScsiCommand& command) = 0;
	virtual	void				DataOutSpecific(ScsiCommand& command,
									const std::vector<u8>& data) {}
	virtual	void				Inquiry(std::vector<u8>& data) = 0;
	virtual	u64					BlockCount() = 0;
	virtual	u32					BlockSize() = 0;
	virtual	bool				ModeSense(u8 page, std::vector<u8>& data);

			void				SetSense(u8 key, u8 asc, u8 ascq = 0);
			void				CheckCondition(ScsiCommand& command, u8 key,
									u8 asc, u8 ascq = 0);
			static u32			Cdb32(const u8* p)
									{ return LoadBE32(p); }

			u8					fSenseKey;
			u8					fSenseAsc;
			u8					fSenseAscq;
			bool				fUnitAttention;
			ActivityCallback	fActivity;
};

class ScsiDisk : public ScsiDevice {
public:
								ScsiDisk(std::unique_ptr<DiskImage> image,
									const std::string& vendor,
									const std::string& product);

	virtual	const char*			Kind() const { return "disk"; }

protected:
	virtual	bool				ExecuteSpecific(ScsiCommand& command);
	virtual	void				DataOutSpecific(ScsiCommand& command,
									const std::vector<u8>& data);
	virtual	void				Inquiry(std::vector<u8>& data);
	virtual	u64					BlockCount();
	virtual	u32					BlockSize() { return 512; }

private:
			std::unique_ptr<DiskImage> fImage;
			std::string			fVendor;
			std::string			fProduct;
			u64					fWriteLba;
};

class ScsiCd : public ScsiDevice {
public:
								ScsiCd(const std::string& vendor,
									const std::string& product);

			bool				Insert(std::unique_ptr<CdImage> image);
			void				Eject();
			bool				HasMedium() const { return fImage != nullptr; }

	virtual	void				Reset();
	virtual	const char*			Kind() const { return "CD-ROM"; }

protected:
	virtual	bool				ExecuteSpecific(ScsiCommand& command);
	virtual	void				DataOutSpecific(ScsiCommand& command,
									const std::vector<u8>& data);
	virtual	void				Inquiry(std::vector<u8>& data);
	virtual	u64					BlockCount();
	virtual	u32					BlockSize() { return fBlockSize; }
	virtual	bool				ModeSense(u8 page, std::vector<u8>& data);

private:
			bool				_Read(ScsiCommand& command, u64 lba,
									u32 count);
			void				_ReadToc(ScsiCommand& command);

			std::unique_ptr<CdImage> fImage;
			std::string			fVendor;
			std::string			fProduct;
			u32					fBlockSize;		// 2048 or 512
			bool				fLocked;
};

}	// namespace bebox

#endif	// BEBOX_SCSI_DEVICE_H
