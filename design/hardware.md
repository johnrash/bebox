# BeBox hardware, as emulated

What the emulator implements and why, with the places the knowledge came
from. Sources:

- Be Newsletter #27, "OS Writer's Cookbook" (memory map, interrupts, boot
  contract) and Be's "Dual603 hardware specification" page.
- The DR8 User's Guide and the BeBook (www.be.com archive in `docs/`).
- NetBSD/bebox (`sys/arch/bebox`) and MAME's `bebox` driver, as references.
- Chip manuals: MPC603e, MPC105, NCR 53C810, S3 Vision868 (with its
  Vision864 differences), the VGADOC notes on S3, 8514/A, Cirrus Logic,
  RAMDACs and clock chips (`work/manuals/`).
- Disassembly of the boot ROMs and of the BeOS kernels and drivers. The
  findings that no document has are marked **(found)**.

## Models

| Model | CPUs | PLL | PVR | Default RAM |
|---|---|---|---|---|
| Dual603-66 | 2x 603 at 66 MHz | 2:1 | 0x0003xxxx | 32 MB |
| Dual603e-133 | 2x 603e at 133 MHz | 4:1 | 0x0006xxxx | 32 MB |

The processor and PCI bus run at 33 MHz; the time base and decrementer tick
at a quarter of the bus clock. The 603e has HID1 with the PLL_CFG pins.
RAM is 8-256 MB in SIMM pairs. The motherboard revision is 5 (128 KB boot
flash) or 6 (512 KB); the I/O card revision is reported through the A/D
converter (below).

## Physical memory map (both processors)

| Address | Contents |
|---|---|
| 0x00000000 | RAM |
| 0x7ffff0f0 / 0x7ffff1f0 | CPU 0 / CPU 1 interrupt mask |
| 0x7ffff2f0 | interrupt sources |
| 0x7ffff3f0 | cross-processor: SMI, TLBISYNC, who am I |
| 0x7ffff4f0 | CPU 1 SRESET/HRESET, FLUSHREQ, disk LED (0x01000000) |
| 0x80000000 + port | ISA/PCI I/O |
| 0x80000cf8 / cfc | PCI configuration mechanism #1 |
| 0x80800000 | direct (IDSEL) configuration window |
| 0xbffffff0 | interrupt acknowledge (8259) |
| 0xc0000000 | PCI memory |
| 0xfff00000 | boot flash: nub; the main ROM at 0xfff04000 |

The Be registers use set/clear writes: the top bit says set or clear, the
other set bits select. PCI masters see RAM at `phys | 0x80000000`. The
MPC105 runs big endian and keeps byte addresses, so little endian PCI
registers need `lwbrx`/`stwbrx`; the I/O space hands devices little endian
values. The MPC105 answers CONFIG_ADDRESS (CF8h) only to 32-bit accesses:
R4.5's boot ROM detects mechanism #1 with `outl 0x80000000, outb 0x0e,
inl`, which fails otherwise **(found)**.

## Interrupts

Be interrupt controller bits (bit 0 = MSB): 4-7 serial 1-4, 8-9 MIDI,
10 SCSI, 11-13 PCI slots, 14 sound, 15 keyboard, 16 RTC, 18 floppy,
23 mouse, 24 IDE, 26 8259 cascade, 27 IR, 28 A/D, 29 GeekPort. Each CPU
sees `sources & mask[cpu]`. ISA lines go to both the Be controller and the
8259 pair; the PIT only to the 8259.

- 8259 edge mode: IRR clears when the line drops before the acknowledge,
  else a spurious keyboard interrupt reads garbage (a phantom held key that
  opens the boot menu) **(found)**.

## Chipset and ISA devices

- **MPC105** (PCI device 0): PICR1 MP_ID reads differently on each CPU;
  flash write enable.
- **Intel 82378ZB SIO** (device 11): two 8259s, 8254, two 8237s with page
  registers, ELCR. The 8237's extended mode (82378 register 40Bh/4D6h bits
  2-3) matters: BeOS plays sound with "16-bit, count by bytes", where
  addresses are not shifted **(found)**.
- **82091AA AIP**: COM1/COM2 (serial 1 and 2), floppy (82077, DMA 2), IDE
  at 1F0h (IRQ 14), parallel (stub).
- **TL16C554** (8 MHz): serial 3 and 4 at 380h/388h; MIDI 1/2 at 3A0h/3A8h.
  The modem status of MIDI 1 (3A6h) bits 4-7 give the motherboard revision:
  6 -> rev 6, 7 -> rev 5, 0xe -> rev 3 **(found)**.
- **8042** at 60h/64h, AT keyboard and PS/2 mouse **(found)**:
  - the boot ROM decodes raw scan set 2 and turns translation off itself
    (command 60h, byte 05h);
  - its send helper waits to *see* IBF set after a write, so IBF stays set
    about 50 us;
  - interrupts only for device data, never for replies to controller
    commands (the R3 ROM polls replies with interrupts enabled);
  - BeOS takes mouse data on IRQ 12 (Be bit 23) although the command byte
    keeps the aux interrupt disabled;
  - held keys need typematic repeat.
- **RTC** (146818 compatible) at 70h with the NVRAM (boot preferences).
  It runs in binary 24 hour mode (register B 06h): the boot ROM never
  sets the mode, BeOS DR6 to PR2 read the clock as binary numbers (in BCD
  mode they read 2026 as 2038 and overflow), R3 and later honor the mode
  and write register B back as they read it **(found)**. DR8's `date`
  prints years after 1999 as 100, 101, ... (years since 1900).
- **CS4231A** codec at 830h (Be bit 14): BeOS plays on DMA 6 (mode 2,
  16-bit big endian stereo, 44.1 kHz) and records on DMA 7 **(found;
  NetBSD has them the other way round)**.
- **MAX186 A/D** (game ports) at 360h: control/status (done bit 800h),
  result at 362h. Channel 6 reports the I/O card revision (> 256: revision
  5) **(found)**. From BeOS's `a2d` driver **(found)**: the upper game port
  is channel 4, the lower channel 5; bits 8-9 of the register at 36Eh select
  the multiplexer input (stick 1 X, stick 1 Y, stick 2 X, stick 2 Y; older
  I/O cards share it with the GeekPort output enable at 366h). The buttons
  are at 397h, sticks 1-4 using bits 01h/08h, 02h/04h, 10h/80h, 20h/40h.
- **Front panel LEDs**: an 8-bit latch at C00h, high nibble the left
  column's level (0-15), low nibble the right's. Each column has sixteen
  LEDs behind an acrylic light pipe: the fifteen load LEDs above the power
  LED (left) or the disk LED (right). The disk LED is the register bit at
  0x7ffff4f0; DR releases set it rarely, so the emulator also lights it on
  SCSI and IDE transfers.
- **Ethernet** (an expansion card): an NE2000 compatible ISA card, the
  first of the adapters the DR8 User's Guide lists ("NE2000, 3Com 3C503,
  DEC 21040 or 21041"); Be's networking software defaults to port 300h and
  IRQ 5, and every release from DR8 to R4.5 has its `ne2000` add-on. A
  DP8390 core with 16 KB of buffer memory (pages 40h-7Fh), the station
  address PROM (bytes doubled, 57h 57h at 14-15) and remote DMA through the
  data port.
- **Flash**: AMD 29F010/29F040 command set (program, sector erase,
  autoselect), written by BeOS's ROM updater.

## PCI devices

- **NCR 53C810** (device 12, Be bit 10) with a full SCRIPTS processor.
  SCRIPTS memory moves address the chip's own registers by the I/O BAR
  value (BeOS and the ROMs modify their scripts that way) **(found)**.
  SCSI targets report no unit attention after a reset: the R3 ROM gives up
  on TEST UNIT READY with unit attention **(found)**.
- **IDE**: an empty channel reads 00h (neither busy nor ready): 7Fh makes
  DR7 send IDENTIFY and wait forever, FFh makes the ROM wait 31.5 s
  **(found)**.
- **Video** in a PCI slot (device 13, Be bit 11). The boot ROM supports S3
  Trio, Vision864/964 and Cirrus GD5430/34/36.
  - **Number Nine #9 GXE64**: S3 Vision864 (5333:88C0), AT&T 21C498
    RAMDAC, ICD2061A clock. **(found)**: the CR50 engine width is bits 7-6
    (640, 800, 1280) extended by bit 0 (1152, 1600); the CRTC counts bytes
    through the 21C498's 16-bit port (CR67 mode 3/5: 2 clocks per pixel,
    7: 4); at 32 bpp the hardware cursor works in half pixels: DR8 and
    R4.5 write twice the pointer's x (except once right at the mode
    switch, when they write it in pixels) and a pattern with every pixel
    doubled, so 64 pattern columns cover 32 pixels; BeOS writes engine
    registers such as FRGD_COLOR with single byte writes; R4.5's driver
    identifies the RAMDAC by reading 3C6h four times, then 84h and 98h,
    and uses at most 2 MB on a Vision864.
  - **Cirrus Logic CL-GD5434** (1013:00A8): in 32 bpp the CRTC offset
    (CR13) counts 16-byte units; BeOS draws with the BitBLT engine's color
    expansion; the cursor patterns are in the last 16 KB of the memory in
    use, and with 4 MB installed BeOS's driver turns DRAM bank switching
    off (SR0F bit 7, the bit that doubles 2 MB to 4 MB), so its cursor
    lives below 2 MB **(found)**.

## Processor details that BeOS depends on (found)

- `mfspr` 284/285 (the time base *write* numbers) read the time base in
  supervisor mode; the DR7 kernel uses them.
- The 603's TLB entries are tagged with EA bits 4-14 besides VSID and API;
  a store miss on a page with C=0 reports the way that hit in SRR1[WAY].
- Misaligned accesses that cross a page are split in hardware (DR7's
  `memset` relies on it).

## Boot contract

CPU 1 is held in reset; CPU 0 runs the nub, which tests memory (animating
the LEDs) and starts the main ROM, a PEF container loaded at 0x300000. The
main ROM releases CPU 1, scans PCI, shows the logo and the boot menu and
loads the kernel at 0x3100; both processors enter it. Low memory: 0x3010
RAM size, 0x3014 0xfeedbabe, 0x3018 kernel entry, 0x301c the boot item list
({ next, name, size, data } records; the kernel copies them within a few
milliseconds). The kernel takes its PCI device list from it: DR6's kernel
wants `pci_table_v2` (112-byte records), the 1.1d7 ROM provides
`pci_table_v5` (120-byte records, the same fields rearranged plus the
subsystem IDs; DR7's kernel converts either) **(found)**. When the ROM
jumps into a kernel that knows only `pci_table_v2`, the emulator rewrites
the table in place as `pci_table_v2` (`src/core/board/BootItems.cpp`).
DR6's mouse driver moves the pointer two pixels per count left and down
(negative deltas) and one right and up.

Boot ROM key checks happen 5.5-6 s after a cold power on: F1 sends kernel
debug output to serial 4 (19200 baud), other keys open the boot menu. A
warm reset keeps RAM and shortens the memory test, so power on clears RAM.

The BFS releases need newer boot ROMs, shipped as
`/beos/etc/bebox_bootmain.image` on the PR2, R3 and R4.5 CDs (R4.5's has a
4-byte "BOOT" tag before the PEF). PR2's boots Advanced Access and PR1.
These ROMs stop at their boot menu (Return) when no boot volume is set.
The R4.5 CD (BIN/CUE) holds an x86 BFS volume in track 2 and the PowerPC
one in track 3.

## The Hobbit prototype

Sources: Be Labs' draft hardware notes (September 1993: BeBus, I/O,
joysticks, keyboard, RTC, serial, floppy, printer, SCSI, memory map),
AT&T's ATT92010 Programmer's Reference Manual (January 1993), and the
unstripped kernels on the three surviving disks (March, November and
December 1994). **(found)** marks what comes from the kernels' code.

Processors: two AT&T 92010 at 25 MHz. ID register 0x1000003b (mask 2);
the November 1994 kernel has extra code for the 92020 (0x0e33d03b).

Physical memory map **(found: the kernel's table of physical objects)**:

| Address | Contents |
|---|---|
| 0x00000000 | boot flash (512 KB) |
| 0x00800000 | I/O window (4 KB) |
| 0x08000000 | RAM (4, 8, 16, 20 or 32 MB; the boot ROM stores the size at 0x08000048) |
| 0x60000000 | the DSPs' 2 MB |
| 0x80000000 | "slot_8" |
| 0x90000000, 0x98000000 | Ethernet card (slot 1) |
| 0xa0000000 | telephone card (slot 2) |
| 0xb0000000, 0xb1000000, 0xbffc0000 | development cards (slot 3) |
| 0xc0000000 | audio card (slot 4) |
| 0xd0000000, 0xd1000000 | video card (slot 5): frame buffer (1 MB), registers |

I/O window: each 8-bit register has a word: byte lane 3 of word `port`
(0x800000 + port * 4 + 3) for the PC compatible chips at their usual port
numbers (keyboard 0x60/0x64, RTC 0x70/0x71, SER2 0x2f8, SER1 0x3f8,
floppy at the secondary address 0x370-0x377, parallel 0x378, joystick
controller 0x222/0x226 **(found)**), lane 2 for the 53C94 (0x800802 +
reg * 4, 16-bit pseudo-DMA at 0x800842).

Board registers (lane 3):

| Address | Register |
|---|---|
| 0x800003 | I/O interrupts: 0 floppy, 1 SER1, 2 SER2, 4 RTC, 5 keyboard, 6 mouse, 7 SCSI **(found)** |
| 0x800007, 0x80000b | I/O interrupt masks of CPU 1, CPU 2 (the kernel routes all to CPU 1) |
| 0x80000f | slot interrupts (and the joystick port D) |
| 0x800013, 0x800017 | slot interrupt masks |
| 0x80001b | processor reset: bit 0 CPU 1, bit 1 CPU 2 (0 holds it) **(found: pulsed to start CPU 2)** |
| 0x80001f | internal interrupts: bit 0 Hobbit A, 1 Hobbit B, 2-4 DSP 1-3 |
| 0x800dc3 | who am I: bits 3-0 the bus master (4 CPU 1, 5 CPU 2) |
| 0x800020-0x80002f | floppy DMA: count - 1, direction (1 from memory), address **(found)** |

Interrupt levels **(found: the kernel's handlers)**: 1 internal interrupts
(and the timers), 3 I/O, 5 slots, 6 joystick controller.

Floppy: an "MCS3201" at 0x370, the kernel's drive 1. It neither polls the
drives nor interrupts after a reset (the March 1994 kernel has no handler
for that early) **(found)**.

SCSI: NCR 53C94, the kernel's driver selects without ATN (CDB in the
FIFO), moves data through the pseudo-DMA port (or byte by byte for short
or odd buffers), then initiator command complete and message accepted; a
bus reset is answered by two interrupts **(found)**.

Video: a Trident VGA on a BeBus card. The window servers program the VGA
and Trident extended registers for 640x480 or 800x600 at 256 colors (8
pixels per character clock, 16 bytes per offset unit), load the palette
from `/system/Color_map` through a gamma table into an 8 bits per
component RAMDAC (so the screen is a little brighter than screenshots
taken on the machine, which record the color map), and an XGA style
graphics engine through index 0x217a and 32-bit data at 0x217c (little
endian): pixel maps A (0x94-0x9c) and B (0xb4-0xbc), pixel bit mask 0xc4,
mixes 0xc8, colors 0xd8/0xdc, dimensions 0xe0, destination and source
linear addresses 0xe4/0xe8, source and destination x,y 0xf0/0xf8, pixel
operation 0xfc (0xa8: bit blit, 0x0a: area fill; direction in bits 2-0),
status 0x90 (bit 31 busy) **(found)**.

Disks: Be's 1993-94 file system (version 1.0, see
`src/core/hobbit/HobbitVolume.h`), unlike the later BFS. DR6 and DR8 have
versions 2 and 3 of it (same volume header, but 128 byte directory entries
with 64 character names), and do not mount version 1 **(tested: DR8)**.
A deleted entry keeps its first sector and data; its name's first byte is
0 **(found)**.

Boot contract **(found)**: the ROM loads `/system/kernel` (ELF, machine
0x39d) at its addresses, stores the memory size at 0x08000048 and enters
it with the PSW on the SP; the kernel moves both CPUs' stacks into the
last page of memory. CPU 2 starts when the kernel releases it from reset;
the ROM sends it to the kernel's entry too.
