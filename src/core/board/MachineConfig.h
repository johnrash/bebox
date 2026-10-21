/*
 * BeBox emulator - machine configuration.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#ifndef BEBOX_MACHINE_CONFIG_H
#define BEBOX_MACHINE_CONFIG_H

#include "../Types.h"

#include <string>

namespace bebox {

struct MachineConfig {
	enum Model {
		kBeBox66,		// two PowerPC 603 at 66 MHz
		kBeBox133,		// two PowerPC 603e at 133 MHz
		kHobbit			// the 1993/94 prototype: two AT&T 92010 (Hobbit)
						// at 25 MHz (HobbitMachine)
	};

	Model			model = kBeBox66;
	u32				ramMB = 32;			// 8-512, banks of 8-64 MB (SIMM pairs)
	int				cpuCount = 2;		// 1 for debugging
	u32				pvr = 0;			// 0: the model's default
	u32				busHz = 33000000;	// processor and PCI bus
	// Average CPU cycles per instruction, times 100.
	u32				cpiPercent = 100;
	// Emulated time slice per CPU, in microseconds.
	u32				quantumUs = 10;
	// Each processor on a host thread of its own (they run at the same
	// time between the time slice boundaries, where the devices run).
	// Faster when both are busy; runs are no longer reproducible.
	bool			threadPerCpu = false;
	// Motherboard revision (3, 5 or 6): 5 has a 128 KB flash ROM, 6 a
	// 512 KB one.
	int				boardRevision = 6;
	// I/O card revision (4 or 5).
	int				ioCardRevision = 5;
	// ISA DMA channels of the sound codec.
	int				soundPlaybackDma = 6;
	int				soundCaptureDma = 7;

	enum VideoCard {
		kVideoNone,
		kVideoCirrus5434,	// Cirrus Logic CL-GD5434
		kVideoS3Vision864	// #9 GXE64: S3 Vision864, AT&T 21C498
	};
	VideoCard		videoCard = kVideoS3Vision864;
	u32				vramMB = 2;

	// Storage: image paths ("" = no device); read only flags.
	std::string		ideImage[2];		// master, slave (ATA disks)
	bool			ideReadOnly[2] = { false, false };
	struct ScsiTarget {
		enum Kind {
			kNone,
			kDisk,
			kCdRom,
			// a host folder (image: the folder); the front end builds its
			// BFS volume (SharedFolder) and runs the machine with the
			// volume's image as a disk
			kSharedFolder
		};
		Kind		kind = kNone;
		std::string	image;
		bool		readOnly = false;
	};
	ScsiTarget		scsi[7];			// IDs 0-6 (the controller is 7)

	// Ethernet: an NE2000 compatible ISA card (Be's defaults: port 300h,
	// IRQ 5; the DR8 User's Guide allows IRQ 5, 9, 10, 11, 13 or 15).
	bool			ethernet = false;
	u32				ethernetPort = 0x300;
	int				ethernetIrq = 5;
	u8				ethernetMac[6] = { 0x00, 0x00, 0xe8, 0xbe, 0xb0, 0x01 };

	std::string		floppyImage;		// internal 3.5" drive
	bool			floppyWriteProtected = false;

	// Hobbit: the kernel to start instead of /system/kernel of the boot
	// volume (the floppy when a disk is inserted, else the SCSI disk with
	// the lowest ID), an ELF file of the host.
	std::string		hobbitKernelPath;

	std::string		nubRomPath;			// bootnub.rom (16 KB)
	std::string		mainRomPath;		// bootmain.rom (128 KB)
	std::string		flashPath;			// persistent flash image
	std::string		nvramPath;			// persistent RTC RAM
};

}	// namespace bebox

#endif	// BEBOX_MACHINE_CONFIG_H
