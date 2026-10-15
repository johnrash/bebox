/*
 * BeBox emulator - disk images.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * A DiskImage is a byte addressable block device. Implementations: raw
 * files (RawImage), QCOW2 (Qcow2Image), CD images (CdImage).
 */
#ifndef BEBOX_DISK_IMAGE_H
#define BEBOX_DISK_IMAGE_H

#include "../Types.h"

#include <memory>
#include <string>

namespace bebox {

class DiskImage {
public:
	virtual						~DiskImage() {}

	virtual	u64					Size() const = 0;
	virtual	bool				IsReadOnly() const = 0;
	virtual	bool				Read(u64 offset, void* buffer, size_t length)
									= 0;
	virtual	bool				Write(u64 offset, const void* buffer,
									size_t length) = 0;
	virtual	bool				Flush() { return true; }
	virtual	std::string			Path() const = 0;
	virtual	const char*			Format() const = 0;

			// Opens raw or QCOW2 images (detected from the header).
	static	std::unique_ptr<DiskImage> Open(const std::string& path,
									bool readOnly, std::string& error);
};

class RawImage : public DiskImage {
public:
								~RawImage();

	static	std::unique_ptr<RawImage> Open(const std::string& path,
									bool readOnly, std::string& error);
	static	bool				Create(const std::string& path, u64 size,
									std::string& error);

	virtual	u64					Size() const { return fSize; }
	virtual	bool				IsReadOnly() const { return fReadOnly; }
	virtual	bool				Read(u64 offset, void* buffer, size_t length);
	virtual	bool				Write(u64 offset, const void* buffer,
									size_t length);
	virtual	bool				Flush();
	virtual	std::string			Path() const { return fPath; }
	virtual	const char*			Format() const { return "raw"; }

private:
								RawImage() {}

			int					fFd = -1;
			u64					fSize = 0;
			bool				fReadOnly = false;
			std::string			fPath;
};

}	// namespace bebox

#endif	// BEBOX_DISK_IMAGE_H
