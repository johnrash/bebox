/*
 * BeBox emulator - raw disk images.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "DiskImage.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bebox {

RawImage::~RawImage()
{
	if (fFd >= 0)
		close(fFd);
}


std::unique_ptr<RawImage>
RawImage::Open(const std::string& path, bool readOnly, std::string& error)
{
	int fd = open(path.c_str(), readOnly ? O_RDONLY : O_RDWR);
	if (fd < 0 && !readOnly && (errno == EACCES || errno == EROFS)) {
		readOnly = true;
		fd = open(path.c_str(), O_RDONLY);
	}
	if (fd < 0) {
		error = path + ": " + strerror(errno);
		return nullptr;
	}
	struct stat st;
	if (fstat(fd, &st) != 0) {
		error = path + ": " + strerror(errno);
		close(fd);
		return nullptr;
	}
	std::unique_ptr<RawImage> image(new RawImage);
	image->fFd = fd;
	image->fSize = (u64)st.st_size;
	image->fReadOnly = readOnly;
	image->fPath = path;
	if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
		off_t end = lseek(fd, 0, SEEK_END);
		if (end > 0)
			image->fSize = (u64)end;
	}
	return image;
}


bool
RawImage::Create(const std::string& path, u64 size, std::string& error)
{
	int fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		error = path + ": " + strerror(errno);
		return false;
	}
	bool ok = ftruncate(fd, (off_t)size) == 0;
	if (!ok)
		error = path + ": " + strerror(errno);
	close(fd);
	return ok;
}


bool
RawImage::Read(u64 offset, void* buffer, size_t length)
{
	u8* out = (u8*)buffer;
	while (length > 0) {
		if (offset >= fSize) {
			memset(out, 0, length);		// past the end reads as zeros
			return true;
		}
		ssize_t count = pread(fFd, out, length, (off_t)offset);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (count == 0) {
			memset(out, 0, length);
			return true;
		}
		out += count;
		offset += count;
		length -= count;
	}
	return true;
}


bool
RawImage::Write(u64 offset, const void* buffer, size_t length)
{
	if (fReadOnly)
		return false;
	const u8* in = (const u8*)buffer;
	while (length > 0) {
		ssize_t count = pwrite(fFd, in, length, (off_t)offset);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		in += count;
		offset += count;
		length -= count;
	}
	if (offset > fSize)
		fSize = offset;
	return true;
}


bool
RawImage::Flush()
{
	return fsync(fFd) == 0;
}


}	// namespace bebox
