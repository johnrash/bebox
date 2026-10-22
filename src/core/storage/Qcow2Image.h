/*
 * BeBox emulator - QCOW2 (QEMU copy on write, version 2 and 3) images.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Thin provisioned disk images: a two level table (L1 -> L2 -> data
 * cluster) maps the virtual disk; clusters are allocated on first write at
 * the end of the file and reference counted. Supported: reading and
 * writing normal clusters, reading deflate compressed clusters, zero
 * clusters, backing files (copy on write overlays). Not supported:
 * encryption, external data files, extended L2 entries, zstd compression
 * and writing images with internal snapshots' shared clusters is done by
 * copying (the old cluster's reference count drops).
 *
 * A write that fails (a full disk) leaves the image consistent: new
 * clusters are written at the end of the file and counted before anything
 * refers to them, on disk before in memory (see _WriteCluster()).
 */
#ifndef BEBOX_QCOW2_IMAGE_H
#define BEBOX_QCOW2_IMAGE_H

#include "DiskImage.h"

#include <map>
#include <vector>

namespace bebox {

class Qcow2Image : public DiskImage {
public:
								~Qcow2Image();

	static	bool				IsQcow2(int fd);
	static	std::unique_ptr<Qcow2Image> Open(const std::string& path,
									bool readOnly, std::string& error);
	// Creates an empty image (cluster size 64 KB, version 3); with a
	// backing file the new image is an overlay of it.
	static	bool				Create(const std::string& path, u64 size,
									std::string& error,
									const std::string& backingFile = "",
									int clusterBits = 16);

	virtual	u64					Size() const { return fSize; }
	virtual	bool				IsReadOnly() const { return fReadOnly; }
	virtual	bool				Read(u64 offset, void* buffer, size_t length);
	virtual	bool				Write(u64 offset, const void* buffer,
									size_t length);
	virtual	bool				Flush();
	virtual	std::string			Path() const { return fPath; }
	virtual	const char*			Format() const { return "qcow2"; }

			// Information for tools.
			int					Version() const { return fVersion; }
			u32					ClusterSize() const { return fClusterSize; }
			std::string			BackingFile() const { return fBackingPath; }
			u64					AllocatedClusters();
			// Recomputes all reference counts from the tables and compares
			// them with the stored ones; returns the number of problems
			// (leaks count as problems too) and describes them in 'report'.
			int					Check(std::string& report);
			// Whether a guest cluster is allocated in this image (for
			// convert: unallocated clusters read as zeros or from the
			// backing file).
			bool				IsAllocated(u64 offset);

private:
								Qcow2Image() {}

			bool				_Init(std::string& error);
			bool				_ReadAt(u64 offset, void* buffer,
									size_t length);
			bool				_WriteAt(u64 offset, const void* buffer,
									size_t length);
			bool				_WriteU64(u64 offset, u64 value);

			// L2 tables (cached)
			std::vector<u64>*	_L2Table(u64 l2Offset);
			bool				_GetCluster(u64 guestCluster, u64& entry);
			bool				_ReadCluster(u64 guestCluster, u32 inCluster,
									u8* buffer, u32 length);
			bool				_ReadCompressed(u64 entry, u8* cluster);
			bool				_WriteCluster(u64 guestCluster, u32 inCluster,
									const u8* buffer, u32 length);

			// allocation and reference counts
			bool				_Append(const u8* cluster, u64& offset);
			void				_Unappend(u64 offset);
			void				_Release(u64 cluster);
			bool				_GetRefcount(u64 cluster, u64& refcount);
			bool				_SetRefcount(u64 cluster, u64 refcount);
			bool				_GrowRefcountTable(u64 neededIndex);
			u64					_RefcountOf(const u8* block, u32 index) const;
			void				_StoreRefcount(u8* block, u32 index,
									u64 value) const;
			std::vector<u8>*	_RefcountBlock(u64 offset);

			int					fFd = -1;
			bool				fReadOnly = false;
			std::string			fPath;
			int					fVersion = 0;
			u64					fSize = 0;
			int					fClusterBits = 16;
			u32					fClusterSize = 65536;
			u32					fL2Entries = 8192;
			int					fRefcountOrder = 4;
			u32					fRefcountsPerBlock = 32768;
			u32					fHeaderLength = 72;
			u64					fL1Offset = 0;
			std::vector<u64>	fL1;
			u64					fRefcountTableOffset = 0;
			u32					fRefcountTableClusters = 0;
			std::vector<u64>	fRefcountTable;
			u64					fFileEnd = 0;		// cluster aligned
			u64					fNextFree = 0;		// allocation hint

			std::map<u64, std::vector<u64> > fL2Cache;
			std::map<u64, std::vector<u8> > fRefcountCache;
			u64					fCompressedEntry = 0;
			std::vector<u8>		fCompressedCluster;

			std::string			fBackingPath;
			std::unique_ptr<DiskImage> fBacking;
};

}	// namespace bebox

#endif	// BEBOX_QCOW2_IMAGE_H
