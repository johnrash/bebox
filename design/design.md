# BeBox emulator design

## Layers

```
src/core/      portable C++17, no Haiku headers: the machine
  cpu/         PowerPC 603/603e interpreter (PpcCpu, PpcOpsInt, PpcOpsFpu)
  bus/         PhysBus (physical address space), IoSpace (ports),
               Mpc105 (bridge, PCI configuration), PciBus
  board/       Machine (the interface the front ends use, Create() picks
               the kind), BeBoxMachine (assembly, run loop), BeBoxBoard
               (Be registers, LEDs), Scheduler (timers), Flash29F0x0,
               MachineConfig, BootItems (the boot ROM's boot items,
               adapted for DR6)
  hobbit/      the Hobbit prototype: HobbitMachine (assembly, run loop,
               the boot ROM's job), HobbitCpu and HobbitDisasm (AT&T
               92010), HobbitBoard (I/O window, interrupts, resets, floppy
               DMA), Ncr53c94, HobbitVideo (Trident VGA, graphics engine),
               HobbitVolume (its Be file system, read only; also bebox-img's)
  isa/         8259, 8254, 8237 (in Sio82378), 8042, RTC, UARTs, floppy,
               IDE, CS4231A, MAX186 A/D, 82091 AIP, NE2000
  pci/         Ncr53c810 (SCRIPTS), ScsiDevice (disks, CD-ROMs)
  video/       VgaCore, S3Vision864, CirrusGd5434
  storage/     DiskImage (raw, QCOW2), CdImage (ISO, BIN/CUE), BfsReader,
               BfsWriter, SharedFolder
  host/        interfaces to the front end: VideoSink, AudioSink,
               SerialBackend (+ file, TCP, host port backends),
               NetworkBackend (libslirp), KeyCodes,
               EmulatorThread (real time pacing on its own thread)
src/haiku/     the Haiku application
src/headless/  bebox-headless, the command line runner (Linux and Haiku)
tools/         bebox-img, extract-rom-images.sh, ppc-testgen.py, make_icon.py,
               merge_hvif.py (Haiku icon + overlay icon in one HVIF)
tests/         Linux unit and boot tests, the Haiku end-to-end test
```

The core builds on Linux, where it is tested under ASan and UBSan, and on
Haiku; the app links it in.

## Time and scheduling

Time is counted in machine ticks, 8 per bus clock (264 MHz for the 33 MHz
bus), so both models' processor clocks and the time base (bus/4) are whole
numbers of ticks. Each CPU has its own clock and advances it by a fixed
number of ticks per instruction (the CPI setting); the time base and
decrementer derive from it.

`Machine::RunFor()` runs the processors in time slices: each runs until the
end of a quantum (1-50 us of emulated time, a setting) or the next device
event, then due timers fire. Devices arm `Timer`s at absolute times; MMIO
accesses see the issuing CPU's clock (`Scheduler::SetClock()` is per
thread). By default (and in `bebox-headless`) one host thread runs both
processors in turn: any store or DMA to a reservation granule clears the
other's `lwarx` reservation, and runs are deterministic, which the boot
regression relies on.

Threaded mode (`MachineConfig::threadPerCpu`, the app's default) runs CPU 1
on a worker thread: when both processors work, they run each slice at the
same time and meet at a spin barrier; when one of them is in reset or
dozing, the slice runs on one thread as before (no handover cost). What it
takes:

- one recursive spin lock (`BusLock`) around device accesses, DMA and page
  table changes; CPUs read page entries lock free (a sequence count) and
  their own translation caches without any lock. Haiku's mutex sleeps in
  the kernel at once when contended, which made a mutex here 30 times
  slower;
- `stwcx.` stores with a host compare-and-swap against the value `lwarx`
  loaded (snooping alone races), and `sync` is a full fence;
- changes to another CPU's state from a device (reset lines, a bus map
  change flushing its caches) are requests the CPU applies on its own
  thread before its next instruction; interrupt lines are atomic;
- a reset asked for from inside a slice waits for its end.

Both processors busy (DR6, or BeOS at work), threaded mode is 1.7-1.8
times as fast; `THREADS=1 tests/boot-regression.sh` boots every case so,
and `cpu-unit-tests` counts 200000 `lwarx`/`stwcx.` increments from two
threads.

`EmulatorThread` runs the machine in slices against the host clock (speed
limit in percent, or unlimited), posts commands from the UI between slices
and publishes a status snapshot (speed, MIPS, LED levels). A CPU that dozes
(MSR[POW] with HID0 DOZE/NAP) is halted until an exception is pending, and
so is one that branches to itself with external interrupts enabled
(`b .`): BeOS R4.5's idle loop, which would otherwise keep both processors
busy all the time (a guest at rest then costs next to nothing).

## Processor

An interpreter dispatching on the primary opcode through a table. State: GPRs, FPRs, CR,
LR, CTR, XER, MSR, FPSCR, the SPRs of the 603 including the TLB miss
registers (DMISS, DCMP, HASH1/2, IMISS, ICMP, RPA) and TGPR0-3.

Address translation: real mode, BATs, segment registers, and the 603's
software TLBs (instruction and data, 32 sets of 2 ways). A miss vectors to
0x1000/0x1100/0x1200 with the miss registers filled and GPR0-3 swapped for
TGPR0-3; `tlbld`/`tlbli` load entries. PP/key protection raises DSI/ISI.
A per-CPU cache maps effective pages to host pointers for fetch, read and
write; it is flushed (by bumping a generation number in the tags) on TLB,
segment, BAT, SDR1 and MSR changes. Instructions are fetched from memory
each time (no predecoding), so self-modifying code needs no tracking.

The FPU uses host doubles (with `fma` for the fused instructions), rounds
single precision results and keeps the FPSCR status bits.

Tests: `tools/ppc-testgen.py` builds small programs with the PowerPC cross
toolchain; `tests/linux/run-ppc-oracle.sh` runs them here and under
`qemu-ppc -cpu 603e` and compares the register dumps (20000 cases).
Hand-written tests cover the MMU, exceptions and both processors.

## The Hobbit prototype

`HobbitMachine` implements `Machine` like `BeBoxMachine`; the front ends
only see the interface (plus `FloppyUnit()`: the Hobbit's drive is unit
1, and `HasLeds()`). Ticks are a quarter of the 25 MHz clock.

The processor (`HobbitCpu`) is an interpreter written from AT&T's ATT92010
Programmer's Reference Manual (January 1993). It decodes 1, 3 and 5 parcel
instructions every time (no cache); operands are memory, immediates or,
after a CPU prefix, control registers. Points the kernels depend on:

- Stack offsets are relative to the shadow register SHAD, not to the SP or
  ISP: the kernel moves the ISP and keeps addressing its frame
  (`set_stacks`), and switches the stack pointer in the PSW before setting
  SHAD.
- The stack cache holds the SP..MSP words on the chip, untranslated. The
  context switch flushes it (ENTER with an indirect operand), refills the
  top 16 bytes (CATCH) and loads the new address space's STB while still
  reading those words. ENTER writes back, CATCH fills, RETURN/POPN and SP
  writes discard, as the manual's pseudo code says. ENTER and CATCH take
  their indirect operand with one indirection less than the other
  instructions.
- The MMU walks the segment and page tables (paged and nonpaged segments,
  referenced and modified bits) into a per-mode soft TLB for fetches,
  reads and writes; STB writes, FLUSHPTE and bus map changes flush it.
- Faults are C++ exceptions inside an instruction; the event sequences
  (interrupts, exceptions, KCALL) push their block on the interrupt stack
  and reset the processor if that faults.

The boot ROM is replaced: at power on the machine reads the kernel's ELF
file from the boot volume (`HobbitVolume`), loads its segments, stores the
memory size at 0x08000048, and starts CPU 1 on a temporary stack in the
last page of memory (which the kernel keeps for the boot stacks), on the
SP, with the caches enabled. When the kernel releases CPU 2 (the reset
register), CPU 2 starts the same way.

Idle: the kernels are not stripped (except the installer's), so the
machine finds the idle loop (`_null_thread`, or `_null_process` with its
`_sc_hint`; else the November 1994 loop by its code) and the processors
skip to their next timer event there.

## Buses and devices

`PhysBus` is a 4 KB page table: a page is host memory (RAM, ROM, video
memory: the CPU reads and writes it directly) or an `MmioDevice` (values in
the processor's big endian view). PCI BARs remap pages when written.
Unclaimed accesses are logged with the CPU and PC (the first 100, then at
powers of two): the main tool for finding undocumented registers.

`IoSpace` dispatches ISA ports (64 K) and PCI I/O ranges; devices see little
endian values. The 8514/A style S3 registers (x2E8h) are ISA ports too.

Video cards derive from `VgaCore` (standard VGA registers, planar and
chained memory access, text and graphics rendering) and add their
extensions through hooks (`ComputeMode`, register reads and writes, banking,
overlays). Each vertical retrace (60 Hz of emulated time) renders the
visible frame into a host buffer handed to the `VideoSink`.

The S3 Vision864's graphics engine executes commands at once (rectangle
fill, BitBLT, pattern fill, Bresenham and radial lines, short stroke
vectors); commands that wait for CPU data (image and monochrome transfers)
consume PIX_TRANS writes as they come. GP_STAT always reports an idle
engine with an empty FIFO.

## Storage

`DiskImage` is the interface for raw and QCOW2 images (versions 2 and 3:
L1/L2 tables, refcounts with table growth, compressed clusters read only,
backing files for overlays). `CdImage` reads ISO, Toast and BIN/CUE
(2048 and 2352 byte sectors, audio tracks). `BfsReader` reads files and
directories from BFS volumes (big and little endian; Apple partition maps
and CD tracks) for extracting boot ROM images.

## Network

`Ne2000` is the card (an ISA device on the 8259); a `NetworkBackend` carries
its frames. The backend is libslirp (`SlirpNetwork`): the user mode network
QEMU uses, with DHCP, a DNS relay, NAT for TCP, UDP and ICMP, and host port
forwarding. Slirp runs on the emulation thread: the card polls it every
millisecond of emulated time (sockets polled without waiting, slirp's
timers fired), and received frames stay queued in the backend while the
card's ring buffer is full, so nothing is dropped.

## Shared folders

`BfsWriter` writes big endian BFS volumes the way BeOS's mkbfs lays them
out (2 KB blocks, one bitmap block per allocation group, a 2 MB log,
B+tree directories of 1 KB nodes, small data attributes in the inode and an
attribute directory for the others, indirect runs for large files, an empty
index directory). `SharedFolder` builds such a volume (a thin QCOW2 image)
from a host folder at power on and records a snapshot of what it wrote;
after a clean shutdown it reads the volume back with `BfsReader` and
applies BeOS's changes to the folder (files whose BFS time or size differ,
attribute hashes that differ, entries that disappeared), keeping a "NAME
(BeBox)" copy when the host changed the same file. Attributes of numeric
types are byte swapped between the little endian host and the big endian
volume. The front ends supply the host's attributes (`HostAttributes`:
`HaikuAttributes` on Haiku). The Haiku app copies the folders on a thread of
its own (`JobQueue`, one job after the other, so a folder is back before it
is copied again) and starts the machine when they are ready; quitting waits
for the queue.

## The Haiku application

- `App`: settings, command line, About.
- `MachineWindow`: menus, the `ScreenView` between two `LedColumnView`s, a
  status line; owns the `EmulatorThread`, the audio output (`HaikuAudio`,
  a `BSoundPlayer` fed through a ring buffer with resampling) and the serial
  backends. The sound card's clock and the clock the emulation is paced by
  drift apart (by 1.3% in a QEMU virtual machine), so the resampling step
  is corrected (up to 2%) to keep the ring's fill level near its target
  instead of letting it run dry or overflow. Keyboard input is taken in `DispatchMessage()` (so window
  shortcuts don't eat keys) and mapped to scan code set 2; the mouse is
  captured on click and released with right Control. Scripting (`hey`) is
  implemented here.
- `PrefsWindow`: a sidebar of sections with Haiku's own preference icons
  (Machine: the virtual memory icon with the "Be" overlay, merged into one
  HVIF by `tools/merge_hvif.py`; ROM & NVRAM, Display, Storage, Sound,
  Keyboard & mouse, Game ports, Network, Serial ports, Advanced) editing
  the current profile. The file panels for disk slots get a "New disk
  image" button (added next to the panel's Cancel button) that opens
  `NewDiskWindow` on the panel's folder.
- `ProfileNameWindow`: the name of a new or renamed profile.
- `Settings`: flattened `BMessage`s. `~/config/settings/BeBox/settings`
  holds what all profiles share (keyboard and mouse, the LED look, the
  window) and the current profile's name; each machine profile is a folder
  in `Profiles/` with its `settings`, `flash.bin` and `nvram.bin` (and the
  shared folder volumes while they are in use). A folder counts as a
  profile once it has its settings file. A new profile is a copy of the
  current one; when the machine runs, `Machine::SetPersistentPaths` moves
  its flash and NVRAM to the copy. Switching to another profile restarts
  the machine.
- `SerialConsole`: a console window per serial port.

## Debugging aids (bebox-headless)

`--trace`, `--trace-pc ADDR` (time-stamped hits of chosen PCs),
`--dump`/`--dump-va` (physical or virtual memory), `--watch` and
`--watch-write ADDR` (who writes a word), `--log CATEGORY=LEVEL`,
`--key`, `--type`, `--click`/`--press`/`--release`/`--dclick` (closed loop
on the hardware cursor position), `--shots` (a PNG every second),
`--serialN` destinations, `--interrupt`/`--power-cycle` and `--debug-boot`
(F1 for the kernel's debug output on serial 4, as the app's Machine menu
holds it).
