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
               adapted for DR6), ThreadGovernor (threaded mode: one host
               thread or two)
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
threads. `tests/tsan.sh` runs that test and a threaded R4.5 boot under
ThreadSanitizer (`make TSAN=1`): its only reports are races on guest
memory (both CPUs touching the same words, as the guest's own code does
on a real BeBox), which it counts and accepts; a race anywhere else fails
it.

That holds while both threads have a core of their own. When other work
keeps the host's cores busy, the scheduler takes one of the threads off
its core for milliseconds at a time and every handover waits for it:
R4.5 at 133 MHz with 5 busy loops on 6 cores ran at 201% on two threads
against 283% on one, and R5 playing an MP3 in the app with 3 of 4 cores
busy at 5-37% of real time. `ThreadGovernor` therefore chooses one or
two threads for the slices in which both CPUs run:

- With two threads, each thread times its waits for the other beyond 256
  spins (a clock read only then; a wait both threads did at once counts
  once). Over 0.2 s with both running, two threads that are S times as
  fast as one while they never wait are slower once they wait more than
  1 - 1/S of the time; 5 points above that, one thread takes over. An idle
  host shows 1-5% waits, an overloaded one 50% and more.
- S depends on the guest's work. A trial measures it while the threads
  waited less than 5%: the slices alternate between one thread and two in
  5 ms blocks (20 ms each), timed in instructions per second, the waits
  taken out of the two threads' time; R4.5 booting gives 1.4-1.9. On a
  busy host such short blocks mislead (a worker woken after a block on one
  thread is placed on the main thread's core by Linux, or runs boosted on
  Haiku), so they are not used there. Trials repeat after 1, 2, 4 ... 32 s.
- With one thread, a probe runs two for 10 ms (settling) plus 40 ms and
  keeps them if they waited 5 points less than the break-even share; each
  probe that goes back to one thread doubles the time to the next (1 to
  16 s).

The worker sleeps while the slices run on one thread, so it takes no core
then. Measured (R4.5 at 133 MHz booting for 25 emulated seconds,
unpaced, `bebox-headless`):

| host, busy loops | two threads before | now | one thread |
|------------------|--------------------|-----|------------|
| Linux, 6 cores, 0 | 383% | 379-382% | 291% |
| Linux, 6 cores, 4 | 362% | 351-360% | 290% |
| Linux, 6 cores, 5 | 201% | 263-282% | 230-286% |
| Linux, 6 cores, 6 | 182% | 229-237% | 233% |
| Haiku, 4 cores, 0 | 356-361% | 354-361% | 276% |
| Haiku, 4 cores, 3 | 190% | 265-275% | 275-278% |

R5 playing an MP3 in the app (100% speed, Haiku, 4 cores): idle, both
keep up. With 3 cores busy from power on (128 MB), the old build ran at
5% on average (0.5% at worst; 2 s of sound in 40 s), the new one at 99%
(84% at worst) without underruns while playing; with the load starting
during playback (64 MB), 11-37% against 99-101% (one underrun at the switch
to one thread).

`EmulatorThread` runs the machine in 1 ms slices against the host clock,
posts commands from the UI between slices and publishes a status snapshot
(speed, MIPS, LED levels). Emulated time follows real time; the speed
setting is the processors' speed: an instruction takes the model's ticks
divided by a speed factor (`Machine::SetInstructionTicks()`, the fraction
carried from slice to slice), capped by the setting (as fast as possible:
a tick an instruction). While the emulation is more than 5 ms behind with
busy processors the factor drops 3% a slice, and it comes back 1% a slice
while the thread waits for real time; it never goes below the setting or
a real BeBox: BeOS's sound (an MP3 decoder) needs every instruction of an
emulated second, so a host that cannot keep up then slows time (catching
up within 250 ms), and the sound output keeps more in reserve after it
ran dry once (`HaikuAudio`: 139 ms, growing to 279 ms). A CPU that dozes
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
single precision results and keeps the FPSCR status bits. The general
path reads the host's exception flags (MXCSR) around each operation;
`ArithFast()` takes the common case without them (round to nearest, no
exception enabled, finite operands, results away from overflow and
underflow): the rounding error of add, sub, mul and div comes exactly from
2Sum and Dekker's product, a multiply-add whose product is exact (single
precision operands: at most 48 significant bits) is a multiply and an add,
and rounding to single is checked by converting back. That spares `fma`,
which hosts without the instruction (QEMU's virtual CPUs) do in software
(9 to 15 ns a call). The builds use `-ffp-contract=off` (Dekker's product
needs separate roundings), and values are pinned in registers around the
flag reads (`Pin()`: the compiler does not know that arithmetic sets them).
`cpu-unit-tests` runs a million random cases through both paths and
expects the same results and FPSCR bit for bit.

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
visible frame into a host buffer handed to the `VideoSink` with the lines
that changed, or not at all when nothing did. In the packed pixel modes
(the desktop's) each line of video memory on the screen is compared with a
copy of what was converted last time and only the changed lines are
converted; the overlays (the hardware cursor, `OverlayLines()`) are drawn
on a copy of that picture, and their lines count as changed only when
they look different. The Haiku app then copies those lines into its
bitmap and has app_server redraw just them. Text, planar and pixel doubled
modes are converted whole every time. With R5 playing an MP3 (the screen
mostly still), the display's timer went from 0.84 s to 0.09 s per 30 s,
291 frames instead of 6300 reached the sink in a run of 105 s, and in the
app on the Haiku VM the app took 396 ms of processor time a second
instead of 438, app_server 2 ms instead of 23.

The S3 Vision864's graphics engine executes commands at once (rectangle
fill, BitBLT, pattern fill, Bresenham and radial lines, short stroke
vectors); commands that wait for CPU data (image and monochrome transfers)
consume PIX_TRANS writes as they come. GP_STAT always reports an idle
engine with an empty FIFO.

## Profiling

`bebox-headless --stats` reports, for the part of a run from
`--stats-from MS` on: the run loop's host time (the processors' slices on
one thread or two, the devices' timers between them, by timer name from
`Scheduler::SetProfiling()`), the threaded mode's decisions
(`ThreadGovernor::GetCounts()`), and per processor its slow paths
(`PpcCpu::Stats`: translations its host side cache missed, instruction
fetches from another page, cache flushes, device accesses, accesses across
pages) and exceptions by vector. The counters sit in the slow paths only,
so they are always on; the timing is only while profiling. `--profile`
(Linux, x86-64) samples the program counter every millisecond of processor
time (`SIGPROF`) and names the functions with `addr2line` (inlined
functions under their own names).

R4.5 booting (133 MHz, one thread) spends 96% in the processors: the
dispatch loop, the opcode 31 table, branches and loads. R5 playing an MP3
spent about 30% in floating point (`Arith` 18%, reading and clearing the
host's exception flags 9%, the FPSCR summary 2%); with `ArithFast()` it is
about 15%, and the same 30 s of playback take 9.8 s instead of 11.2 s on
the Linux host (306% of a real BeBox instead of 268%; 286% instead of 254%
on the Haiku VM). It spent 6% converting frames for the display
(`VgaCore::RenderFrame()`, 60 a second; now only changed lines, under 1%)
and spends 14% in the dispatch loop; the
603's TLB misses come to about 60000 a second per processor, each a short
handler in the guest.

## Storage

`DiskImage` is the interface for raw and QCOW2 images (versions 2 and 3:
L1/L2 tables, refcounts with table growth, compressed clusters read only,
backing files for overlays). A QCOW2 write that allocates writes the new
cluster (data, an L2 table, a reference count block) at the end of the
file, counts it, enters it into the table that refers to it on disk and
then in memory, and releases what it replaces last, as QEMU does; growing
the reference count table switches the header last. So a write that fails
(a full disk) leaves the image consistent, as opened and reopened: the
cluster that could not be written is cut off the file again, and a table
that could not grow is undone. `qcow2-tests` fills the disk (a file size
limit) at random points with 512 byte clusters, where every kind of
allocation meets it. `CdImage` reads ISO, Toast and BIN/CUE
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
(BeBox)" copy when the host changed the same file. Anything it cannot
read from the volume (a directory's tree, an inode, a file) or write to
the folder (a directory, a file) leaves the deletions out (what was not
seen may still exist, a moved file may not be at its new place yet) and
keeps the volume; the snapshot then records what did go to the folder, so
that the retry at the next power on does not take those files for
conflicts. A host file that cannot be read while the volume is built fails
the power on ("cannot read NAME"); one that shrank meanwhile reads as
zeros. Attributes of numeric
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
  implemented here. Its log sink (`Log::SetSink()`, any thread) passes
  everything to the standard error and errors of the `storage` and `host`
  categories to the window: a disk image that cannot be written (the
  SCSI, IDE and floppy devices log that once until a write works again)
  or flash and NVRAM that cannot be saved show in an alert (once per power
  on) and in the status line for 30 s (`hey get Problem`). `--log
  CATEGORY=LEVEL` sets levels (`Log::Configure()`, also bebox-headless').
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
