/*
 * BeBox emulator - QCOW2 images.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Written from the published QCOW2 format description. All on-disk
 * numbers are big endian. Metadata writes go straight to the file (write
 * through caches), in an order that keeps the image consistent: data
 * before the L2 entry pointing to it, reference counts before use.
 */
#include "Qcow2Image.h"

#include "../Log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace bebox {

namespace {

const u32 kMagic = 0x514649fb;				// "QFI\xfb"
const u64 kOffsetMask = 0x00fffffffffffe00ULL;
const u64 kCopied = 1ULL << 63;
const u64 kCompressed = 1ULL << 62;
const u64 kZeroFlag = 1ULL;					// version 3 L2 entries

// incompatible feature bits
const u64 kIncompatibleDirty = 1;
const u64 kIncompatibleCorrupt = 2;

const u32 kExtensionEnd = 0;
const u32 kExtensionBackingFormat = 0xe2792aca;


u64
RoundUp(u64 value, u64 alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}


std::string
DirectoryOf(const std::string& path)
{
	size_t slash = path.rfind('/');
	return slash == std::string::npos ? std::string(".")
		: path.substr(0, slash);
}

}	// namespace


Qcow2Image::~Qcow2Image()
{
	if (fFd >= 0) {
		if (!fReadOnly)
			fsync(fFd);
		close(fFd);
	}
}


bool
Qcow2Image::IsQcow2(int fd)
{
	u8 magic[4];
	return pread(fd, magic, 4, 0) == 4 && LoadBE32(magic) == kMagic;
}


std::unique_ptr<Qcow2Image>
Qcow2Image::Open(const std::string& path, bool readOnly, std::string& error)
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
	std::unique_ptr<Qcow2Image> image(new Qcow2Image);
	image->fFd = fd;
	image->fReadOnly = readOnly;
	image->fPath = path;
	if (!image->_Init(error))
		return nullptr;
	return image;
}


bool
Qcow2Image::_Init(std::string& error)
{
	u8 header[112];
	memset(header, 0, sizeof(header));
	if (pread(fFd, header, sizeof(header), 0) < 72
		|| LoadBE32(header) != kMagic) {
		error = fPath + ": not a QCOW2 image";
		return false;
	}
	fVersion = (int)LoadBE32(header + 4);
	if (fVersion != 2 && fVersion != 3) {
		error = fPath + ": unsupported QCOW version "
			+ std::to_string(fVersion);
		return false;
	}
	u64 backingOffset = LoadBE64(header + 8);
	u32 backingSize = LoadBE32(header + 16);
	fClusterBits = (int)LoadBE32(header + 20);
	fSize = LoadBE64(header + 24);
	u32 cryptMethod = LoadBE32(header + 32);
	u32 l1Size = LoadBE32(header + 36);
	fL1Offset = LoadBE64(header + 40);
	fRefcountTableOffset = LoadBE64(header + 48);
	fRefcountTableClusters = LoadBE32(header + 56);
	u32 snapshots = LoadBE32(header + 60);
	if (fClusterBits < 9 || fClusterBits > 21) {
		error = fPath + ": bad cluster size";
		return false;
	}
	if (cryptMethod != 0) {
		error = fPath + ": encrypted images are not supported";
		return false;
	}
	fClusterSize = 1u << fClusterBits;
	fL2Entries = fClusterSize / 8;
	fRefcountOrder = 4;
	fHeaderLength = 72;
	if (fVersion == 3) {
		u64 incompatible = LoadBE64(header + 72);
		u64 autoclear = LoadBE64(header + 88);
		fRefcountOrder = (int)LoadBE32(header + 96);
		fHeaderLength = LoadBE32(header + 100);
		u64 known = kIncompatibleDirty | kIncompatibleCorrupt;
		if ((incompatible & ~known) != 0) {
			error = fPath + ": uses unsupported QCOW2 features (external "
				"data file, extended L2 entries or zstd compression)";
			return false;
		}
		if ((incompatible & (kIncompatibleDirty | kIncompatibleCorrupt))
				!= 0 && !fReadOnly) {
			error = fPath + ": the image is marked dirty or corrupt; "
				"repair it with 'qemu-img check -r all'";
			return false;
		}
		if (fRefcountOrder > 6) {
			error = fPath + ": bad reference count width";
			return false;
		}
		// Features this writer does not maintain (bitmaps) must be
		// invalidated when the image is opened for writing.
		if (autoclear != 0 && !fReadOnly) {
			u8 zero[8] = { 0 };
			_WriteAt(88, zero, 8);
		}
	}
	u32 refcountBits = 1u << fRefcountOrder;
	fRefcountsPerBlock = (u32)((u64)fClusterSize * 8 / refcountBits);
	if (snapshots != 0) {
		BB_LOG(kLogStorage, kLogInfo, "%s: has %u internal snapshots (the "
			"current state is used)", fPath.c_str(), snapshots);
	}

	// L1 and reference count tables
	u64 neededL1 = (fSize + (u64)fClusterSize * fL2Entries - 1)
		/ ((u64)fClusterSize * fL2Entries);
	if (l1Size < neededL1 || l1Size > (1u << 26)) {
		error = fPath + ": bad L1 table size";
		return false;
	}
	fL1.resize(l1Size);
	std::vector<u8> raw((size_t)l1Size * 8);
	if (!_ReadAt(fL1Offset, raw.data(), raw.size())) {
		error = fPath + ": cannot read the L1 table";
		return false;
	}
	for (u32 i = 0; i < l1Size; i++)
		fL1[i] = LoadBE64(&raw[i * 8]);
	u64 tableEntries = (u64)fRefcountTableClusters * fClusterSize / 8;
	if (tableEntries == 0 || tableEntries > (1u << 24)) {
		error = fPath + ": bad reference count table";
		return false;
	}
	fRefcountTable.resize(tableEntries);
	raw.resize(tableEntries * 8);
	if (!_ReadAt(fRefcountTableOffset, raw.data(), raw.size())) {
		error = fPath + ": cannot read the reference count table";
		return false;
	}
	for (u64 i = 0; i < tableEntries; i++)
		fRefcountTable[i] = LoadBE64(&raw[i * 8]);

	struct stat st;
	if (fstat(fFd, &st) != 0) {
		error = fPath + ": " + strerror(errno);
		return false;
	}
	fFileEnd = RoundUp((u64)st.st_size, fClusterSize);

	if (backingOffset != 0 && backingSize > 0) {
		if (backingSize > 1023) {
			error = fPath + ": bad backing file name";
			return false;
		}
		std::string name(backingSize, '\0');
		if (!_ReadAt(backingOffset, &name[0], backingSize)) {
			error = fPath + ": cannot read the backing file name";
			return false;
		}
		fBackingPath = name;
		std::string resolved = name[0] == '/' ? name
			: DirectoryOf(fPath) + "/" + name;
		std::string backingError;
		fBacking = DiskImage::Open(resolved, true, backingError);
		if (!fBacking) {
			error = fPath + ": backing file: " + backingError;
			return false;
		}
	}
	BB_LOG(kLogStorage, kLogDebug, "%s: qcow2 v%d, %llu bytes, %u byte "
		"clusters%s%s", fPath.c_str(), fVersion, (unsigned long long)fSize,
		fClusterSize, fBacking ? ", backing " : "", fBackingPath.c_str());
	return true;
}


bool
Qcow2Image::_ReadAt(u64 offset, void* buffer, size_t length)
{
	u8* out = (u8*)buffer;
	while (length > 0) {
		ssize_t count = pread(fFd, out, length, (off_t)offset);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (count == 0) {
			memset(out, 0, length);		// past the end of the file
			return true;
		}
		out += count;
		offset += count;
		length -= count;
	}
	return true;
}


bool
Qcow2Image::_WriteAt(u64 offset, const void* buffer, size_t length)
{
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
	return true;
}


bool
Qcow2Image::_WriteU64(u64 offset, u64 value)
{
	u8 bytes[8];
	StoreBE64(bytes, value);
	return _WriteAt(offset, bytes, 8);
}


// #pragma mark - reading


std::vector<u64>*
Qcow2Image::_L2Table(u64 l2Offset)
{
	auto it = fL2Cache.find(l2Offset);
	if (it != fL2Cache.end())
		return &it->second;
	std::vector<u8> raw(fClusterSize);
	if (!_ReadAt(l2Offset, raw.data(), raw.size()))
		return nullptr;
	std::vector<u64>& table = fL2Cache[l2Offset];
	table.resize(fL2Entries);
	for (u32 i = 0; i < fL2Entries; i++)
		table[i] = LoadBE64(&raw[i * 8]);
	return &table;
}


// The L2 entry of a guest cluster (0 when unallocated).
bool
Qcow2Image::_GetCluster(u64 guestCluster, u64& entry)
{
	entry = 0;
	u64 l1Index = guestCluster / fL2Entries;
	if (l1Index >= fL1.size())
		return true;
	u64 l2Offset = fL1[l1Index] & kOffsetMask;
	if (l2Offset == 0)
		return true;
	std::vector<u64>* table = _L2Table(l2Offset);
	if (table == nullptr)
		return false;
	entry = (*table)[guestCluster % fL2Entries];
	return true;
}


bool
Qcow2Image::_ReadCompressed(u64 entry, u8* cluster)
{
	if (entry == fCompressedEntry && !fCompressedCluster.empty()) {
		memcpy(cluster, fCompressedCluster.data(), fClusterSize);
		return true;
	}
	int offsetBits = 62 - (fClusterBits - 8);
	u64 hostOffset = entry & ((1ULL << offsetBits) - 1);
	u64 sectors = ((entry & ~(kCopied | kCompressed)) >> offsetBits) + 1;
	u64 length = sectors * 512 - (hostOffset & 511);
	std::vector<u8> compressed(length);
	if (!_ReadAt(hostOffset, compressed.data(), length))
		return false;

	z_stream stream;
	memset(&stream, 0, sizeof(stream));
	if (inflateInit2(&stream, -12) != Z_OK)		// raw deflate
		return false;
	stream.next_in = compressed.data();
	stream.avail_in = (uInt)length;
	stream.next_out = cluster;
	stream.avail_out = fClusterSize;
	int result = inflate(&stream, Z_FINISH);
	bool ok = (result == Z_STREAM_END || result == Z_BUF_ERROR
		|| result == Z_OK) && stream.avail_out == 0;
	inflateEnd(&stream);
	if (!ok) {
		BB_LOG(kLogStorage, kLogWarning, "%s: bad compressed cluster at "
			"%llx", fPath.c_str(), (unsigned long long)hostOffset);
		return false;
	}
	fCompressedEntry = entry;
	fCompressedCluster.assign(cluster, cluster + fClusterSize);
	return true;
}


bool
Qcow2Image::_ReadCluster(u64 guestCluster, u32 inCluster, u8* buffer,
	u32 length)
{
	u64 entry;
	if (!_GetCluster(guestCluster, entry))
		return false;
	if ((entry & kCompressed) != 0) {
		std::vector<u8> cluster(fClusterSize);
		if (!_ReadCompressed(entry, cluster.data()))
			return false;
		memcpy(buffer, cluster.data() + inCluster, length);
		return true;
	}
	if (fVersion >= 3 && (entry & kZeroFlag) != 0) {
		memset(buffer, 0, length);
		return true;
	}
	u64 hostOffset = entry & kOffsetMask;
	if (hostOffset == 0) {
		u64 guestOffset = guestCluster * fClusterSize + inCluster;
		if (fBacking && guestOffset < fBacking->Size()) {
			u64 available = fBacking->Size() - guestOffset;
			u32 fromBacking = (u32)std::min<u64>(length, available);
			if (!fBacking->Read(guestOffset, buffer, fromBacking))
				return false;
			memset(buffer + fromBacking, 0, length - fromBacking);
		} else
			memset(buffer, 0, length);
		return true;
	}
	return _ReadAt(hostOffset + inCluster, buffer, length);
}


bool
Qcow2Image::Read(u64 offset, void* buffer, size_t length)
{
	u8* out = (u8*)buffer;
	while (length > 0) {
		if (offset >= fSize) {
			memset(out, 0, length);
			return true;
		}
		u64 cluster = offset >> fClusterBits;
		u32 inCluster = (u32)(offset & (fClusterSize - 1));
		u32 chunk = (u32)std::min<u64>(length, fClusterSize - inCluster);
		chunk = (u32)std::min<u64>(chunk, fSize - offset);
		if (!_ReadCluster(cluster, inCluster, out, chunk))
			return false;
		out += chunk;
		offset += chunk;
		length -= chunk;
	}
	return true;
}


bool
Qcow2Image::IsAllocated(u64 offset)
{
	u64 entry;
	if (!_GetCluster(offset >> fClusterBits, entry))
		return true;
	return (entry & (kCompressed | kOffsetMask | kZeroFlag)) != 0;
}


u64
Qcow2Image::AllocatedClusters()
{
	u64 count = 0;
	for (u64 l1Entry : fL1) {
		u64 l2Offset = l1Entry & kOffsetMask;
		if (l2Offset == 0)
			continue;
		std::vector<u64>* table = _L2Table(l2Offset);
		if (table == nullptr)
			continue;
		for (u64 entry : *table) {
			if ((entry & kCompressed) != 0 || (entry & kOffsetMask) != 0)
				count++;
		}
	}
	return count;
}


// #pragma mark - reference counts


u64
Qcow2Image::_RefcountOf(const u8* block, u32 index) const
{
	switch (fRefcountOrder) {
		case 0: return (block[index / 8] >> (index % 8)) & 0x1;
		case 1: return (block[index / 4] >> (2 * (index % 4))) & 0x3;
		case 2: return (block[index / 2] >> (4 * (index % 2))) & 0xf;
		case 3: return block[index];
		case 4: return LoadBE16(block + index * 2);
		case 5: return LoadBE32(block + index * 4);
		default: return LoadBE64(block + index * 8);
	}
}


void
Qcow2Image::_StoreRefcount(u8* block, u32 index, u64 value) const
{
	switch (fRefcountOrder) {
		case 0:
			block[index / 8] = (block[index / 8] & ~(1 << (index % 8)))
				| ((value & 1) << (index % 8));
			break;
		case 1:
			block[index / 4] = (block[index / 4] & ~(3 << (2 * (index % 4))))
				| ((value & 3) << (2 * (index % 4)));
			break;
		case 2:
			block[index / 2] = (block[index / 2]
					& ~(0xf << (4 * (index % 2))))
				| ((value & 0xf) << (4 * (index % 2)));
			break;
		case 3: block[index] = (u8)value; break;
		case 4: StoreBE16(block + index * 2, (u16)value); break;
		case 5: StoreBE32(block + index * 4, (u32)value); break;
		default: StoreBE64(block + index * 8, value); break;
	}
}


std::vector<u8>*
Qcow2Image::_RefcountBlock(u64 offset)
{
	auto it = fRefcountCache.find(offset);
	if (it != fRefcountCache.end())
		return &it->second;
	std::vector<u8>& block = fRefcountCache[offset];
	block.resize(fClusterSize);
	if (!_ReadAt(offset, block.data(), fClusterSize)) {
		fRefcountCache.erase(offset);
		return nullptr;
	}
	return &block;
}


bool
Qcow2Image::_GetRefcount(u64 cluster, u64& refcount)
{
	refcount = 0;
	u64 tableIndex = cluster / fRefcountsPerBlock;
	if (tableIndex >= fRefcountTable.size())
		return true;
	u64 blockOffset = fRefcountTable[tableIndex] & kOffsetMask;
	if (blockOffset == 0)
		return true;
	std::vector<u8>* block = _RefcountBlock(blockOffset);
	if (block == nullptr)
		return false;
	refcount = _RefcountOf(block->data(),
		(u32)(cluster % fRefcountsPerBlock));
	return true;
}


bool
Qcow2Image::_SetRefcount(u64 cluster, u64 refcount)
{
	u64 tableIndex = cluster / fRefcountsPerBlock;
	if (tableIndex >= fRefcountTable.size()
		&& !_GrowRefcountTable(tableIndex))
		return false;
	u64 blockOffset = fRefcountTable[tableIndex] & kOffsetMask;
	if (blockOffset == 0) {
		// A new reference count block at the end of the file; it counts
		// itself once it is in the table.
		blockOffset = fFileEnd;
		fFileEnd += fClusterSize;
		std::vector<u8>& block = fRefcountCache[blockOffset];
		block.assign(fClusterSize, 0);
		if (!_WriteAt(blockOffset, block.data(), fClusterSize))
			return false;
		fRefcountTable[tableIndex] = blockOffset;
		if (!_WriteU64(fRefcountTableOffset + tableIndex * 8, blockOffset))
			return false;
		if (!_SetRefcount(blockOffset >> fClusterBits, 1))
			return false;
	}
	std::vector<u8>* block = _RefcountBlock(blockOffset);
	if (block == nullptr)
		return false;
	u32 index = (u32)(cluster % fRefcountsPerBlock);
	_StoreRefcount(block->data(), index, refcount);
	// write back the bytes holding the entry
	u32 bits = 1u << fRefcountOrder;
	u32 byte = (u32)((u64)index * bits / 8);
	u32 bytes = std::max<u32>(1, bits / 8);
	return _WriteAt(blockOffset + byte, block->data() + byte, bytes);
}


// Moves the reference count table to a bigger place at the end of the
// file that covers 'neededIndex'.
bool
Qcow2Image::_GrowRefcountTable(u64 neededIndex)
{
	u64 entries = std::max<u64>(neededIndex + 1, fRefcountTable.size() * 2);
	u64 clusters = RoundUp(entries * 8, fClusterSize) / fClusterSize;
	entries = clusters * fClusterSize / 8;
	u64 oldOffset = fRefcountTableOffset;
	u64 oldClusters = fRefcountTableClusters;
	u64 newOffset = fFileEnd;
	fFileEnd += clusters * fClusterSize;
	fRefcountTable.resize(entries, 0);

	// Reference counts of the new table: blocks created meanwhile are
	// entered into the old table location, which is only valid for the
	// old size, so the table is written as a whole afterwards.
	u64 savedTableOffset = fRefcountTableOffset;
	fRefcountTableOffset = newOffset;
	std::vector<u8> raw(clusters * fClusterSize, 0);
	if (!_WriteAt(newOffset, raw.data(), raw.size()))
		return false;
	for (u64 i = 0; i < clusters; i++) {
		if (!_SetRefcount((newOffset >> fClusterBits) + i, 1))
			return false;
	}
	for (u64 i = 0; i < fRefcountTable.size(); i++)
		StoreBE64(&raw[i * 8], fRefcountTable[i]);
	if (!_WriteAt(newOffset, raw.data(), raw.size()))
		return false;
	fsync(fFd);
	// switch the header to the new table
	u8 header[12];
	StoreBE64(header, newOffset);
	StoreBE32(header + 8, (u32)clusters);
	if (!_WriteAt(48, header, 12))
		return false;
	fRefcountTableClusters = (u32)clusters;
	(void)savedTableOffset;
	for (u64 i = 0; i < oldClusters; i++)
		_SetRefcount((oldOffset >> fClusterBits) + i, 0);
	return true;
}


u64
Qcow2Image::_AllocateCluster()
{
	u64 offset = fFileEnd;
	fFileEnd += fClusterSize;
	if (!_SetRefcount(offset >> fClusterBits, 1))
		return 0;
	return offset;
}


// #pragma mark - writing


bool
Qcow2Image::_WriteCluster(u64 guestCluster, u32 inCluster, const u8* buffer,
	u32 length)
{
	u64 l1Index = guestCluster / fL2Entries;
	u32 l2Index = (u32)(guestCluster % fL2Entries);
	if (l1Index >= fL1.size())
		return false;
	u64 l1Entry = fL1[l1Index];
	u64 l2Offset = l1Entry & kOffsetMask;
	if (l2Offset == 0 || (l1Entry & kCopied) == 0) {
		// new L2 table, or a private copy of one shared with a snapshot
		u64 newL2 = _AllocateCluster();
		if (newL2 == 0)
			return false;
		std::vector<u64> entries(fL2Entries, 0);
		if (l2Offset != 0) {
			std::vector<u64>* shared = _L2Table(l2Offset);
			if (shared == nullptr)
				return false;
			entries = *shared;
			// the data clusters gain a reference from the copy
			for (u64 entry : entries) {
				u64 data = entry & kOffsetMask;
				if (data != 0 && (entry & kCompressed) == 0) {
					u64 count;
					_GetRefcount(data >> fClusterBits, count);
					_SetRefcount(data >> fClusterBits, count + 1);
				}
			}
		}
		std::vector<u8> raw(fClusterSize);
		for (u32 i = 0; i < fL2Entries; i++)
			StoreBE64(&raw[i * 8], entries[i]);
		if (!_WriteAt(newL2, raw.data(), raw.size()))
			return false;
		fL2Cache[newL2] = entries;
		if (l2Offset != 0) {
			u64 count;
			_GetRefcount(l2Offset >> fClusterBits, count);
			if (count > 0)
				_SetRefcount(l2Offset >> fClusterBits, count - 1);
		}
		l2Offset = newL2;
		fL1[l1Index] = newL2 | kCopied;
		if (!_WriteU64(fL1Offset + l1Index * 8, fL1[l1Index]))
			return false;
	}
	std::vector<u64>* table = _L2Table(l2Offset);
	if (table == nullptr)
		return false;
	u64 entry = (*table)[l2Index];
	u64 dataOffset = entry & kOffsetMask;
	bool compressed = (entry & kCompressed) != 0;
	bool zero = fVersion >= 3 && !compressed && (entry & kZeroFlag) != 0;
	bool inPlace = !compressed && dataOffset != 0
		&& (entry & kCopied) != 0;
	if (inPlace && !zero)
		return _WriteAt(dataOffset + inCluster, buffer, length);

	// The whole cluster gets written: the old contents (zeros, backing
	// file, compressed or shared data) fill the rest.
	std::vector<u8> cluster(fClusterSize);
	if (length < fClusterSize) {
		if (zero)
			memset(cluster.data(), 0, fClusterSize);
		else if (!_ReadCluster(guestCluster, 0, cluster.data(),
				fClusterSize))
			return false;
	}
	memcpy(cluster.data() + inCluster, buffer, length);
	u64 target = inPlace ? dataOffset : _AllocateCluster();
	if (target == 0)
		return false;
	if (!_WriteAt(target, cluster.data(), fClusterSize))
		return false;
	if (!inPlace) {
		// drop the reference to the old cluster(s)
		if (compressed) {
			int offsetBits = 62 - (fClusterBits - 8);
			u64 hostOffset = entry & ((1ULL << offsetBits) - 1);
			u64 sectors = ((entry & ~(kCopied | kCompressed))
				>> offsetBits) + 1;
			// the data ends at the end of its last 512 byte sector
			u64 end = (hostOffset & ~511ULL) + sectors * 512;
			u64 first = hostOffset >> fClusterBits;
			u64 last = (end - 1) >> fClusterBits;
			for (u64 c = first; c <= last; c++) {
				u64 count;
				_GetRefcount(c, count);
				if (count > 0)
					_SetRefcount(c, count - 1);
			}
		} else if (dataOffset != 0) {
			u64 count;
			_GetRefcount(dataOffset >> fClusterBits, count);
			if (count > 0)
				_SetRefcount(dataOffset >> fClusterBits, count - 1);
		}
	}
	(*table)[l2Index] = target | kCopied;
	return _WriteU64(l2Offset + (u64)l2Index * 8, (*table)[l2Index]);
}


bool
Qcow2Image::Write(u64 offset, const void* buffer, size_t length)
{
	if (fReadOnly)
		return false;
	const u8* in = (const u8*)buffer;
	while (length > 0) {
		if (offset >= fSize)
			return false;
		u64 cluster = offset >> fClusterBits;
		u32 inCluster = (u32)(offset & (fClusterSize - 1));
		u32 chunk = (u32)std::min<u64>(length, fClusterSize - inCluster);
		chunk = (u32)std::min<u64>(chunk, fSize - offset);
		if (!_WriteCluster(cluster, inCluster, in, chunk)) {
			BB_LOG(kLogStorage, kLogWarning, "%s: write at %llx failed",
				fPath.c_str(), (unsigned long long)offset);
			return false;
		}
		in += chunk;
		offset += chunk;
		length -= chunk;
	}
	return true;
}


bool
Qcow2Image::Flush()
{
	return fReadOnly || fsync(fFd) == 0;
}


// #pragma mark - check


int
Qcow2Image::Check(std::string& report)
{
	int problems = 0;
	auto complain = [&](const std::string& text) {
		if (problems < 20)
			report += text + "\n";
		problems++;
	};
	u64 clusters = fFileEnd >> fClusterBits;
	std::vector<u64> counts(clusters + 1, 0);
	auto reference = [&](u64 offset, u64 length, const char* what) {
		if (length == 0)
			return;
		u64 first = offset >> fClusterBits;
		u64 last = (offset + length - 1) >> fClusterBits;
		for (u64 c = first; c <= last; c++) {
			if (c >= counts.size()) {
				complain(std::string(what) + " beyond the end of the file");
				return;
			}
			counts[c]++;
		}
	};
	reference(0, fClusterSize, "header");
	reference(fL1Offset, fL1.size() * 8, "L1 table");
	reference(fRefcountTableOffset, (u64)fRefcountTableClusters
		* fClusterSize, "reference count table");
	for (u64 block : fRefcountTable) {
		if ((block & kOffsetMask) != 0)
			reference(block & kOffsetMask, fClusterSize, "refcount block");
	}
	for (size_t i = 0; i < fL1.size(); i++) {
		u64 l2Offset = fL1[i] & kOffsetMask;
		if (l2Offset == 0)
			continue;
		reference(l2Offset, fClusterSize, "L2 table");
		std::vector<u64>* table = _L2Table(l2Offset);
		if (table == nullptr) {
			complain("cannot read an L2 table");
			continue;
		}
		for (u64 entry : *table) {
			if ((entry & kCompressed) != 0) {
				int offsetBits = 62 - (fClusterBits - 8);
				u64 hostOffset = entry & ((1ULL << offsetBits) - 1);
				u64 sectors = ((entry & ~(kCopied | kCompressed))
					>> offsetBits) + 1;
				reference(hostOffset, sectors * 512 - (hostOffset & 511),
					"compressed cluster");
			} else if ((entry & kOffsetMask) != 0)
				reference(entry & kOffsetMask, fClusterSize, "data cluster");
		}
	}
	for (u64 c = 0; c < clusters; c++) {
		u64 stored;
		if (!_GetRefcount(c, stored)) {
			complain("cannot read reference counts");
			break;
		}
		if (stored == counts[c])
			continue;
		char text[128];
		snprintf(text, sizeof(text), "cluster %llu: reference count %llu, "
			"%llu references%s", (unsigned long long)c,
			(unsigned long long)stored, (unsigned long long)counts[c],
			stored > counts[c] && counts[c] == 0 ? " (leaked)" : "");
		complain(text);
	}
	if (problems > 20)
		report += "...\n";
	return problems;
}


// #pragma mark - creating


bool
Qcow2Image::Create(const std::string& path, u64 size, std::string& error,
	const std::string& backingFile, int clusterBits)
{
	if (clusterBits < 9 || clusterBits > 21) {
		error = "bad cluster size";
		return false;
	}
	u64 clusterSize = 1ULL << clusterBits;
	u64 l2Entries = clusterSize / 8;
	u64 l1Size = std::max<u64>(1, (size + clusterSize * l2Entries - 1)
		/ (clusterSize * l2Entries));
	u64 l1Clusters = RoundUp(l1Size * 8, clusterSize) / clusterSize;
	// layout: header, reference count table, one reference count block,
	// the L1 table
	u64 refcountTable = clusterSize;
	u64 refcountBlock = 2 * clusterSize;
	u64 l1Table = 3 * clusterSize;
	u64 usedClusters = 3 + l1Clusters;
	if (usedClusters > clusterSize / 2) {
		error = "the image is too big for its cluster size";
		return false;
	}

	std::vector<u8> header(clusterSize, 0);
	StoreBE32(&header[0], kMagic);
	StoreBE32(&header[4], 3);
	StoreBE32(&header[20], (u32)clusterBits);
	StoreBE64(&header[24], size);
	StoreBE32(&header[36], (u32)l1Size);
	StoreBE64(&header[40], l1Table);
	StoreBE64(&header[48], refcountTable);
	StoreBE32(&header[56], 1);
	StoreBE32(&header[96], 4);			// 16-bit reference counts
	StoreBE32(&header[100], 104);		// header length
	size_t position = 104;
	std::unique_ptr<DiskImage> backing;
	if (!backingFile.empty()) {
		std::string resolved = backingFile[0] == '/' ? backingFile
			: DirectoryOf(path) + "/" + backingFile;
		backing = DiskImage::Open(resolved, true, error);
		if (!backing)
			return false;
		std::string format = backing->Format();
		StoreBE32(&header[position], kExtensionBackingFormat);
		StoreBE32(&header[position + 4], (u32)format.size());
		memcpy(&header[position + 8], format.data(), format.size());
		position += 8 + RoundUp(format.size(), 8);
	}
	StoreBE32(&header[position], kExtensionEnd);
	StoreBE32(&header[position + 4], 0);
	position += 8;
	if (!backingFile.empty()) {
		if (position + backingFile.size() > clusterSize) {
			error = "backing file name too long";
			return false;
		}
		StoreBE64(&header[8], position);
		StoreBE32(&header[16], (u32)backingFile.size());
		memcpy(&header[position], backingFile.data(), backingFile.size());
	}

	int fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		error = path + ": " + strerror(errno);
		return false;
	}
	std::vector<u8> table(clusterSize, 0);
	StoreBE64(&table[0], refcountBlock);
	std::vector<u8> block(clusterSize, 0);
	for (u64 i = 0; i < usedClusters; i++)
		StoreBE16(&block[i * 2], 1);
	bool ok = pwrite(fd, header.data(), clusterSize, 0) == (ssize_t)clusterSize
		&& pwrite(fd, table.data(), clusterSize, (off_t)refcountTable)
			== (ssize_t)clusterSize
		&& pwrite(fd, block.data(), clusterSize, (off_t)refcountBlock)
			== (ssize_t)clusterSize
		&& ftruncate(fd, (off_t)(usedClusters * clusterSize)) == 0;
	if (!ok)
		error = path + ": " + strerror(errno);
	close(fd);
	if (!ok)
		unlink(path.c_str());
	return ok;
}


// #pragma mark - DiskImage::Open


std::unique_ptr<DiskImage>
DiskImage::Open(const std::string& path, bool readOnly, std::string& error)
{
	int fd = open(path.c_str(), O_RDONLY);
	if (fd >= 0) {
		bool qcow2 = Qcow2Image::IsQcow2(fd);
		close(fd);
		if (qcow2)
			return Qcow2Image::Open(path, readOnly, error);
	}
	return RawImage::Open(path, readOnly, error);
}

}	// namespace bebox
