/*
 * BeBox emulator - the boot ROM's boot items.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The boot ROM hands the kernel named blobs ("boot items": the boot
 * device, the PCI devices, ...) in a list starting at the word at 0x301c
 * of physical memory: { next, name, size, data } records, all physical
 * addresses. It stores the kernel's entry point at 0x3018 when it jumps
 * into the kernel; the kernel copies the items within milliseconds.
 *
 * The Be Boot ROM 1.1d7 describes the PCI devices as "pci_table_v5"
 * (120-byte records). BeOS DR6 (1.1d6) predates it: its kernel looks for
 * "pci_table_v2" (112-byte records, the layout DR7's kernel still
 * converts from) and finds no PCI devices without it. For a kernel that
 * knows "pci_table_v2" but not "pci_table_v5", the table is converted in
 * place.
 */
#ifndef BEBOX_BOOT_ITEMS_H
#define BEBOX_BOOT_ITEMS_H

#include "../Types.h"

#include <string>

namespace bebox {

class PhysBus;

class BootItems {
public:
	static const u32 kMagic = 0x3014;
	static const u32 kMagicValue = 0xfeedbabe;
	static const u32 kEntryPointer = 0x3018;
	static const u32 kListPointer = 0x301c;
	// The kernel is loaded below the boot ROM's own image.
	static const u32 kKernelStart = 0x3100;
	static const u32 kKernelEnd = 0x300000;

	// The kernel entry point, 0 while the boot ROM runs (or RAM is not
	// mapped yet).
	static	u32					EntryPoint(PhysBus& bus);
	// Adapts the boot items to the kernel being entered; 'log' says what
	// was done. False if nothing was changed.
	static	bool				AdaptForKernel(PhysBus& bus, std::string& log);

	// pci_table_v5 record -> pci_table_v2 record.
	static const int kV5RecordSize = 120;
	static const int kV2RecordSize = 112;
	static	void				ConvertPciRecordV5ToV2(const u8* v5, u8* v2);
};

}	// namespace bebox

#endif	// BEBOX_BOOT_ITEMS_H
