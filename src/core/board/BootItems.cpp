/*
 * BeBox emulator - the boot ROM's boot items.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "BootItems.h"

#include "../bus/PhysBus.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace bebox {

namespace {

// RAM only, through the page table (no device or unmapped accesses).
u8*
RamPointer(PhysBus& bus, u32 address, u32 length)
{
	const BusPage& page = bus.Page(address);
	if (page.read == nullptr || page.write == nullptr
		|| (address & PhysBus::kPageMask) + length > PhysBus::kPageSize)
		return nullptr;
	return page.write + (address & PhysBus::kPageMask);
}


bool
ReadWord(PhysBus& bus, u32 address, u32& value)
{
	const u8* p = RamPointer(bus, address, 4);
	if (p == nullptr)
		return false;
	value = LoadBE32(p);
	return true;
}


bool
ReadBytes(PhysBus& bus, u32 address, u8* buffer, u32 length)
{
	for (u32 done = 0; done < length;) {
		u32 chunk = std::min(length - done,
			PhysBus::kPageSize - ((address + done) & PhysBus::kPageMask));
		const u8* p = RamPointer(bus, address + done, chunk);
		if (p == nullptr)
			return false;
		memcpy(buffer + done, p, chunk);
		done += chunk;
	}
	return true;
}


bool
WriteBytes(PhysBus& bus, u32 address, const u8* buffer, u32 length)
{
	for (u32 done = 0; done < length;) {
		u32 chunk = std::min(length - done,
			PhysBus::kPageSize - ((address + done) & PhysBus::kPageMask));
		u8* p = RamPointer(bus, address + done, chunk);
		if (p == nullptr)
			return false;
		memcpy(p, buffer + done, chunk);
		done += chunk;
	}
	return true;
}


bool
ReadName(PhysBus& bus, u32 address, std::string& name)
{
	name.clear();
	for (int i = 0; i < 64; i++) {
		u8 c;
		if (!ReadBytes(bus, address + i, &c, 1))
			return false;
		if (c == 0)
			return true;
		name += (char)c;
	}
	return false;
}


// Whether the kernel image mentions 'text'.
bool
KernelMentions(PhysBus& bus, const char* text)
{
	size_t length = strlen(text);
	std::vector<u8> chunk(PhysBus::kPageSize + length);
	for (u32 page = BootItems::kKernelStart & ~PhysBus::kPageMask;
			page < BootItems::kKernelEnd; page += PhysBus::kPageSize) {
		// a page and the start of the next (strings across pages)
		u32 count = PhysBus::kPageSize;
		if (!ReadBytes(bus, page, chunk.data(), count))
			continue;
		if (page + PhysBus::kPageSize < BootItems::kKernelEnd
			&& ReadBytes(bus, page + PhysBus::kPageSize,
				chunk.data() + count, length))
			count += length;
		for (u32 i = 0; i + length <= count; i++) {
			if (memcmp(chunk.data() + i, text, length) == 0)
				return true;
		}
	}
	return false;
}

}	// namespace


// The boot ROM's memory test leaves address patterns in low memory, so
// the entry point only counts with the boot ROM's magic number next to it
// and within the kernel.
u32
BootItems::EntryPoint(PhysBus& bus)
{
	u32 magic = 0;
	u32 entry = 0;
	if (!ReadWord(bus, kMagic, magic) || magic != kMagicValue
		|| !ReadWord(bus, kEntryPointer, entry)
		|| entry < kKernelStart || entry >= kKernelEnd)
		return 0;
	return entry;
}


// Field by field, the inverse of DR7's conversion of v2 records.
void
BootItems::ConvertPciRecordV5ToV2(const u8* v5, u8* v2)
{
	memset(v2, 0, kV2RecordSize);
	v2[0] = v5[4];					// bus
	v2[1] = v5[5];					// device
	v2[2] = v5[6];					// function
	v2[3] = v5[15];
	memcpy(v2 + 4, v5 + 0, 4);		// vendor and device IDs
	v2[8] = v5[7];					// revision
	memcpy(v2 + 9, v5 + 8, 7);		// class, cache line, latency, ...
	memcpy(v2 + 16, v5 + 24, 12);	// expansion ROM: address, ..., size
	memcpy(v2 + 28, v5 + 36, 24);	// base addresses (processor view)
	memcpy(v2 + 52, v5 + 60, 24);	// base addresses (PCI view)
	memcpy(v2 + 76, v5 + 84, 24);	// sizes
	memcpy(v2 + 100, v5 + 108, 6);	// flags (bit 0: I/O space)
	memcpy(v2 + 106, v5 + 114, 4);	// interrupt line, pin, ...
}


bool
BootItems::AdaptForKernel(PhysBus& bus, std::string& log)
{
	if (!KernelMentions(bus, "pci_table_v2")
		|| KernelMentions(bus, "pci_table_v5"))
		return false;

	u32 item;
	if (!ReadWord(bus, kListPointer, item))
		return false;
	for (int count = 0; item != 0 && count < 256; count++) {
		u8 header[16];
		if (!ReadBytes(bus, item, header, sizeof(header)))
			return false;
		u32 name = LoadBE32(header + 4);
		u32 size = LoadBE32(header + 8);
		u32 data = LoadBE32(header + 12);
		std::string itemName;
		if (ReadName(bus, name, itemName) && itemName == "pci_table_v2")
			return false;
		if (itemName == "pci_table_v5" && size % kV5RecordSize == 0
			&& size <= 64 * kV5RecordSize) {
			u32 records = size / kV5RecordSize;
			std::vector<u8> v5(size);
			std::vector<u8> v2((size_t)records * kV2RecordSize);
			if (!ReadBytes(bus, data, v5.data(), size))
				return false;
			for (u32 i = 0; i < records; i++) {
				ConvertPciRecordV5ToV2(&v5[i * kV5RecordSize],
					&v2[i * kV2RecordSize]);
			}
			u8 newSize[4];
			StoreBE32(newSize, (u32)v2.size());
			const u8 version = '2';
			if (!WriteBytes(bus, data, v2.data(), (u32)v2.size())
				|| !WriteBytes(bus, item + 8, newSize, 4)
				|| !WriteBytes(bus, name + 11, &version, 1))
				return false;
			log = "boot items: pci_table_v5 given as pci_table_v2 ("
				+ std::to_string(records) + " devices) for a kernel older "
				"than the boot ROM";
			return true;
		}
		item = LoadBE32(header);
	}
	return false;
}

}	// namespace bebox
