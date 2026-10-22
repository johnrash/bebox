/*
 * BeBox emulator - headless runner (Linux and Haiku).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Runs the machine without a user interface for development and automated
 * tests: serial ports to files or stdout, LED changes and CPU states
 * logged, optional instruction traces.
 */
#include "core/Log.h"
#include "core/board/BeBoxBoard.h"
#include "core/board/BeBoxMachine.h"
#include "core/bus/IoSpace.h"
#include "core/bus/PhysBus.h"
#include "core/cpu/PpcCpu.h"
#include "core/debug/PngWriter.h"
#include "core/hobbit/HobbitMachine.h"
#include "core/host/KeyCodes.h"
#include "core/host/NetworkBackend.h"
#include "core/pci/ScsiDevice.h"
#include "core/storage/CdImage.h"
#include "core/storage/DiskImage.h"
#include "core/storage/SharedFolder.h"
#include "core/host/VideoSink.h"
#include "core/isa/Fdc82077.h"
#include "core/isa/Kbc8042.h"
#include "core/isa/Rtc146818.h"
#include "core/video/VgaCore.h"
#include "core/host/AudioSink.h"
#include "core/host/SerialBackend.h"
#include "core/host/SerialBackends.h"
#include "core/isa/Uart16550.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>
#if defined(__linux__) && defined(__x86_64__)
#include <csignal>
#include <map>
#include <sys/time.h>
#include <ucontext.h>
#define HAVE_PROFILER 1
#endif

using namespace bebox;

namespace {


// Serial ports written to stdout and the runner's status lines share
// stdout: a status line waits for the end of the serial line in progress
// (else it would split, say, "Starting bin/sh" in a log).
static bool sSerialMidLine = false;
static std::string sPendingStatus;


static void
Status(const char* format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (sSerialMidLine)
		sPendingStatus += line;
	else
		fputs(line, stdout);
}

class FileSerial : public SerialBackend {
public:
	FileSerial(FILE* file, const char* label, bool prefix,
			Scheduler& scheduler)
		: fFile(file), fLabel(label), fPrefix(prefix), fAtLineStart(true),
		fScheduler(scheduler) {}

	// Input typed at emulated time 'ticks'.
	void Queue(u64 ticks, const std::string& text)
	{
		for (char c : text)
			fInput.push_back(std::make_pair(ticks, (u8)c));
	}

	virtual int Receive()
	{
		if (fInput.empty() || fScheduler.Now() < fInput.front().first)
			return -1;
		u8 byte = fInput.front().second;
		fInput.pop_front();
		return byte;
	}

	virtual void Transmit(u8 byte)
	{
		if (fPrefix && fAtLineStart)
			fprintf(fFile, "[%s] ", fLabel);
		fputc(byte, fFile);
		fAtLineStart = byte == '\n';
		if (fFile == stdout) {
			sSerialMidLine = !fAtLineStart;
			if (!sSerialMidLine && !sPendingStatus.empty()) {
				fputs(sPendingStatus.c_str(), stdout);
				sPendingStatus.clear();
			}
		}
		fflush(fFile);
	}

private:
	FILE* fFile;
	const char* fLabel;
	bool fPrefix;
	bool fAtLineStart;
	Scheduler& fScheduler;
	std::deque<std::pair<u64, u8> > fInput;
};


// Writes the sound output to a WAV file (at the first sample rate seen).
class WavSink : public AudioSink {
public:
	WavSink(const std::string& path) : fFile(fopen(path.c_str(), "wb")),
		fRate(0), fFrames(0), fPeak(0)
	{
		if (fFile != nullptr) {
			u8 header[44] = { 0 };
			fwrite(header, 1, sizeof(header), fFile);
		}
	}

	~WavSink()
	{
		if (fFile == nullptr)
			return;
		// RIFF header
		u32 dataBytes = (u32)(fFrames * 4);
		u8 h[44];
		memcpy(h, "RIFF", 4);
		StoreLE32(h + 4, 36 + dataBytes);
		memcpy(h + 8, "WAVEfmt ", 8);
		StoreLE32(h + 16, 16);
		StoreLE16(h + 20, 1);
		StoreLE16(h + 22, 2);
		StoreLE32(h + 24, fRate);
		StoreLE32(h + 28, fRate * 4);
		StoreLE16(h + 32, 4);
		StoreLE16(h + 34, 16);
		memcpy(h + 36, "data", 4);
		StoreLE32(h + 40, dataBytes);
		fseek(fFile, 0, SEEK_SET);
		fwrite(h, 1, sizeof(h), fFile);
		fclose(fFile);
		printf("audio: %llu frames at %u Hz, peak %d\n",
			(unsigned long long)fFrames, fRate, fPeak);
	}

	virtual void Play(const s16* frames, size_t count, u32 sampleRate)
	{
		if (fFile == nullptr)
			return;
		if (fRate == 0)
			fRate = sampleRate;
		for (size_t i = 0; i < count * 2; i++) {
			u8 bytes[2];
			StoreLE16(bytes, (u16)frames[i]);
			fwrite(bytes, 1, 2, fFile);
			int magnitude = frames[i] < 0 ? -frames[i] : frames[i];
			if (magnitude > fPeak)
				fPeak = magnitude;
		}
		fFrames += count;
	}

private:
	FILE* fFile;
	u32 fRate;
	u64 fFrames;
	int fPeak;
};


// Write watchpoint: the page is remapped read-direct with writes going
// through here, which logs the writer of the watched word.
class WriteWatch : public MmioDevice {
public:
	WriteWatch(PhysBus& bus, u32 address, u64 ticksPerSecond,
			Scheduler& scheduler)
		: fBus(bus), fAddress(address), fTicksPerSecond(ticksPerSecond),
		fScheduler(scheduler), fPage(nullptr) {}

	void Install()
	{
		u32 base = fAddress & ~PhysBus::kPageMask;
		fPage = fBus.Page(base).write;
		if (fPage == nullptr) {
			fprintf(stderr, "watch %08x: not in RAM\n", fAddress);
			return;
		}
		fBus.MapMemory(base, PhysBus::kPageSize, fPage, false, this);
	}

	virtual u32 MmioRead(u32 addr, int size, int initiator)
	{
		u8* p = fPage + (addr & PhysBus::kPageMask);
		return size == 1 ? *p : size == 2 ? LoadBE16(p) : LoadBE32(p);
	}

	virtual void MmioWrite(u32 addr, u32 value, int size, int initiator)
	{
		u8* p = fPage + (addr & PhysBus::kPageMask);
		if (addr < (fAddress & ~3u) + 4 && addr + size > (fAddress & ~3u)) {
			fprintf(stderr, "[%11.6f] watch write%d %08x = %08x by %s%d pc "
				"%08x\n", (double)fScheduler.Now() / fTicksPerSecond, size * 8, addr,
				value, initiator < PhysBus::kMaxCpus ? "cpu" : "dma",
				initiator, fBus.CurrentPc(initiator));
		}
		if (size == 1)
			*p = (u8)value;
		else if (size == 2)
			StoreBE16(p, (u16)value);
		else
			StoreBE32(p, value);
	}

	virtual const char* MmioName() const { return "watch"; }

private:
	PhysBus& fBus;
	u32 fAddress;
	u64 fTicksPerSecond;
	Scheduler& fScheduler;
	u8* fPage;
};


class TraceHook : public PpcTraceHook {
public:
	TraceHook(FILE* file, u64 limit, u64 ticksPerSecond) : fFile(file),
		fRemaining(limit), fStart(0), fCpuMask(3),
		fTicksPerSecond(ticksPerSecond) {}

	void SetStart(u64 ticks) { fStart = ticks; }
	void SetCpuMask(int mask) { fCpuMask = mask; }
	void AddPc(u32 pc) { fPcs.insert(pc); }

	virtual void Instruction(PpcCpu& cpu, u32 pc, u32 instruction)
	{
		if (fRemaining == 0 || cpu.Now() < fStart
			|| (fCpuMask & (1 << cpu.Index())) == 0
			|| (!fPcs.empty() && fPcs.count(pc) == 0))
			return;
		fRemaining--;
		if (!fPcs.empty()) {
			fprintf(fFile, "[%11.6f] ",
				(double)cpu.Now() / fTicksPerSecond);
		}
		fprintf(fFile, "cpu%d %08x: %08x r0 %08x r1 %08x r2 %08x r3 %08x "
			"r4 %08x lr %08x msr %08x\n", cpu.Index(), pc, instruction,
			cpu.gpr[0], cpu.gpr[1], cpu.gpr[2], cpu.gpr[3], cpu.gpr[4],
			cpu.fLr, cpu.GetMsr());
	}

private:
	FILE* fFile;
	u64 fRemaining;
	u64 fStart;
	int fCpuMask;
	u64 fTicksPerSecond;
	std::set<u32> fPcs;
};


class FrameCapture : public VideoSink {
public:
	std::vector<u32> pixels;
	int width = 0;
	int height = 0;
	u64 frames = 0;

	virtual void FrameReady(const u32* data, int w, int h, int stride)
	{
		width = w;
		height = h;
		pixels.resize((size_t)w * h);
		for (int y = 0; y < h; y++)
			memcpy(&pixels[(size_t)y * w], data + (size_t)y * stride, w * 4);
		frames++;
	}
};


void
Usage()
{
	printf("usage: bebox-headless [options]\n"
		"  --rom-dir DIR       directory with bootnub.rom and bootmain.rom\n"
		"  --model 66|133|hobbit  BeBox model (default 66; hobbit: the\n"
		"                      AT&T 92010 prototype)\n"
		"  --kernel FILE       Hobbit: start this kernel (an ELF file)\n"
		"  --ram MB            memory size (default 32)\n"
		"  --cpus N            processors (1 or 2)\n"
		"  --threads           each processor on a host thread of its own\n"
		"                      (faster; runs are not reproducible)\n"
		"  --flash FILE        persistent flash image\n"
		"  --main-rom FILE     main boot ROM (bootmain.rom or a ROM update\n"
		"                      image such as bebox_bootmain.image)\n"
		"  --nvram FILE        persistent RTC RAM\n"
		"  --rtc-date YYYY-MM-DD[THH:MM]  start the clock at this time\n"
		"                      (UTC)\n"
		"  --ide0/--ide1 FILE  IDE master/slave disk image\n"
		"  --floppy FILE       floppy disk image\n"
		"  --scsiN-disk FILE   SCSI disk on ID N (0-6)\n"
		"  --scsiN-cd FILE     SCSI CD-ROM on ID N (ISO or CUE)\n"
		"  --scsiN-share DIR   a host folder as a BFS disk on ID N (changes\n"
		"                      made by BeOS go back to it after a clean\n"
		"                      shutdown)\n"
		"  --seconds S         emulated seconds to run (default 5)\n"
		"  --serialN DEST      serial port N (1-4): stdout, null, a file, or\n"
		"                      tcp-listen:[ADDR:]PORT, tcp-connect:HOST:PORT,\n"
		"                      host:DEVICE, file:PATH\n"
		"  --input N:MS:TEXT   type TEXT into serial port N at MS (\\r, \\n)\n"
		"  --net               NE2000 Ethernet card on user mode networking\n"
		"                      (NAT: BeBox 10.0.2.15, host/gateway 10.0.2.2,\n"
		"                      DNS 10.0.2.3, DHCP)\n"
		"  --net-forward SPEC  forward host ports: [tcp:|udp:]HOST:GUEST,...\n"
		"  --ne2000 PORT:IRQ   card resources (hex port, default 300:5)\n"
		"  --interrupt MS      press the front panel Interrupt switch at MS\n"
		"  --power-cycle MS    power on again (hard reset) at MS\n"
		"  --debug-boot        hold F1 at power on (kernel debug output on\n"
		"                      serial 4) until the boot ROM writes there\n"
		"  --click MS:X,Y[,B]  from MS on, move the mouse until the hardware\n"
		"                      cursor is at X,Y, then click button B (1-3, 0:\n"
		"                      only move); mouse actions run in order\n"
		"  --press MS:X,Y[,B]  same, but keep the button pressed\n"
		"  --dclick MS:X,Y     double click (button 1)\n"
		"  --release MS:X,Y    move (with the button held) and release\n"
		"  --soft-cursor X,Y[,NEG]  the guest draws its pointer in\n"
		"                      software (DR6): mouse actions move it open\n"
		"                      loop from X,Y, a count at a time; NEG is the\n"
		"                      pixels per count left or down (DR6: 2)\n"
		"  --log CAT=LEVEL     log level (error, warning, info, debug,\n"
		"                      trace) for a category or 'all'\n"
		"  --trace N           trace the first N instructions to stderr\n"
		"  --trace-from MS     start tracing at MS emulated milliseconds\n"
		"  --trace-cpus MASK   CPUs to trace (1 = cpu0, 2 = cpu1, 3 = both)\n"
		"  --trace-pc ADDR     trace only these PCs (repeatable), with time\n"
		"  --status MS         print the CPU states every MS emulated ms\n"
		"  --stats             at the end: where the host's time went (the\n"
		"                      processors, the devices' timers, two\n"
		"                      threads or one) and the processors' slow\n"
		"                      paths (translations, device accesses,\n"
		"                      exceptions)\n"
#ifdef HAVE_PROFILER
		"  --profile           at the end: the emulator's functions that\n"
		"                      took the host's processor time (sampled)\n"
#endif
		"  --stats-from MS     profile from MS emulated ms on (default 0)\n"
		"  --video none|cirrus|s3  video card (default s3: #9 GXE64)\n"
		"  --vram MB           video memory (1, 2 or 4; default 2)\n"
		"  --screenshot FILE   save the last frame as PNG at the end\n"
		"  --audio FILE.wav    record the sound output\n"
		"  --dump ADDR:LEN:FILE  save physical memory at the end\n"
		"  --dump-va ADDR:LEN:FILE  same for virtual memory as CPU0 sees it\n"
		"  --watch ADDR        report changes of a physical word (1 ms steps)\n"
		"  --watch-write ADDR[:MS]  log every write to a physical word, from\n"
		"                      MS emulated ms on (with the writer's PC)\n"
		"  --type MS:TEXT      type ASCII text on the keyboard from MS on\n"
		"                      (\\n is Enter, \\t Tab)\n"
		"  --floppy-insert MS:FILE  change the floppy at MS (empty FILE:\n"
		"                      eject)\n"
		"  --cd-insert MS:N:FILE  change the disc in the CD-ROM drive on\n"
		"                      SCSI ID N at MS (empty FILE: eject; a disc\n"
		"                      in the drive comes out 6 s before)\n"
		"  --key MS:NAME[:HOLD] press a key (Enter, Up, Escape, A, ...) for\n"
		"                      HOLD ms (default 100)\n"
		"  --shots PREFIX      save a PNG every second (PREFIX-N.png)\n");
}

#ifdef HAVE_PROFILER
// --profile: the program counter of the running thread, sampled every
// millisecond of processor time; at the end the functions (by addr2line)
// with the most samples.
std::vector<uintptr_t> sSamples;
std::atomic<size_t> sSampleCount(0);


void
ProfileSample(int, siginfo_t*, void* context)
{
	uintptr_t pc = ((ucontext_t*)context)->uc_mcontext.gregs[REG_RIP];
	size_t index = sSampleCount.fetch_add(1, std::memory_order_relaxed);
	if (index < sSamples.size())
		sSamples[index] = pc;
}


void
StartProfile()
{
	sSamples.assign(4 << 20, 0);
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = ProfileSample;
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGPROF, &action, nullptr);
	struct itimerval timer = { { 0, 1000 }, { 0, 1000 } };
	setitimer(ITIMER_PROF, &timer, nullptr);
}


void
ReportProfile(int top)
{
	struct itimerval off;
	memset(&off, 0, sizeof(off));
	setitimer(ITIMER_PROF, &off, nullptr);
	size_t count = std::min(sSampleCount.load(), sSamples.size());
	if (count == 0)
		return;

	// executable mappings: start, end, file offset, file
	struct Mapping {
		uintptr_t start, end, offset;
		std::string path;
	};
	std::vector<Mapping> mappings;
	if (FILE* maps = fopen("/proc/self/maps", "r")) {
		char line[4096];
		while (fgets(line, sizeof(line), maps) != nullptr) {
			unsigned long start, end, offset;
			char perms[8];
			char path[4096] = "";
			if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %4095s", &start, &end,
					perms, &offset, path) >= 4 && strchr(perms, 'x'))
				mappings.push_back({ start, end, offset, path });
		}
		fclose(maps);
	}
	char self[4096];
	ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
	self[length > 0 ? length : 0] = '\0';

	// the executable's addresses go to addr2line; others count by library
	std::map<std::string, u64> byFunction;
	std::map<uintptr_t, u64> byAddress;
	for (size_t i = 0; i < count; i++) {
		uintptr_t pc = sSamples[i];
		const Mapping* found = nullptr;
		for (const Mapping& mapping : mappings) {
			if (pc >= mapping.start && pc < mapping.end)
				found = &mapping;
		}
		if (found == nullptr)
			byFunction["[unknown]"]++;
		else if (found->path == self)
			byAddress[pc - found->start + found->offset]++;
		else {
			std::string name = found->path;
			byFunction["[" + name.substr(name.rfind('/') + 1) + "]"]++;
		}
	}
	char list[] = "/tmp/bebox-profile-XXXXXX";
	int fd = mkstemp(list);
	if (fd >= 0) {
		std::string addresses;
		for (const auto& item : byAddress) {
			char text[32];
			snprintf(text, sizeof(text), "%lx\n", (unsigned long)item.first);
			addresses += text;
		}
		bool written = write(fd, addresses.data(), addresses.size())
			== (ssize_t)addresses.size();
		close(fd);
		std::string command = std::string("addr2line -f -C -e '") + self
			+ "' < " + list;
		FILE* pipe = written ? popen(command.c_str(), "r") : nullptr;
		char name[4096];
		char where[4096];
		for (const auto& item : byAddress) {
			if (pipe == nullptr || fgets(name, sizeof(name), pipe) == nullptr
				|| fgets(where, sizeof(where), pipe) == nullptr)
				break;
			name[strcspn(name, "\n")] = '\0';
			byFunction[name] += item.second;
		}
		if (pipe != nullptr)
			pclose(pipe);
		unlink(list);
	}
	std::vector<std::pair<u64, std::string> > sorted;
	for (const auto& item : byFunction)
		sorted.push_back(std::make_pair(item.second, item.first));
	std::sort(sorted.rbegin(), sorted.rend());
	printf("profile: %zu samples (1 ms of processor time each)\n", count);
	for (int i = 0; i < top && i < (int)sorted.size(); i++) {
		printf("  %5.1f%%  %s\n", 100.0 * sorted[i].first / count,
			sorted[i].second.substr(0, 100).c_str());
	}
}
#endif

}	// namespace


int
main(int argc, char** argv)
{
	MachineConfig config;
	std::string romDir = "ROMs";
	double seconds = 5;
	u64 traceCount = 0;
	double traceFromMs = 0;
	int traceCpus = 3;
	std::vector<u32> tracePcs;
	double statusMs = 0;
	bool stats = false;
	bool profile = false;
	double statsFromMs = 0;
	std::string screenshot;
	std::string mainRom;
	struct Dump {
		u32 address;
		u32 length;
		std::string file;
		bool virt;
	};
	std::vector<Dump> dumps;
	std::vector<u32> watches;
	std::string audioPath;
	std::string netForwards;
	u32 writeWatchAddress = 0;
	double writeWatchMs = -1;
	struct FloppyChange {
		double ms;
		std::string image;
	};
	std::vector<FloppyChange> floppyChanges;
	struct CdChange {
		double ms;
		int id;
		std::string image;
	};
	std::vector<CdChange> cdChanges;
	struct KeyEvent {
		u64 time;		// microseconds
		u32 code;
		bool down;
	};
	std::vector<KeyEvent> keys;
	std::string shots;
	std::string serial[4] = { "null", "null", "null", "stdout" };
	struct SerialInput {
		int port;
		double ms;
		std::string text;
	};
	std::vector<SerialInput> serialInputs;
	std::vector<double> interrupts;
	std::vector<double> powerCycles;
	bool debugBoot = false;
	struct MouseAction {
		u64 time;		// microseconds
		int x;
		int y;
		int button;		// 0: move only
		enum { kClick, kPress, kRelease } kind;
	};
	std::vector<MouseAction> mouseActions;
	bool haveRtcDate = false;
	s64 rtcDate = 0;
	// a pointer drawn in software: its position as moved (open loop)
	bool softCursor = false;
	int softX = 0;
	int softY = 0;
	int softNegative = 1;		// pixels per count left or down

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		auto next = [&]() -> const char* {
			if (i + 1 >= argc) {
				fprintf(stderr, "missing value for %s\n", arg.c_str());
				exit(2);
			}
			return argv[++i];
		};
		if (arg == "--rom-dir")
			romDir = next();
		else if (arg == "--model") {
			const char* model = next();
			config.model = strcmp(model, "133") == 0
				? MachineConfig::kBeBox133 : strcmp(model, "hobbit") == 0
				? MachineConfig::kHobbit : MachineConfig::kBeBox66;
		} else if (arg == "--kernel")
			config.hobbitKernelPath = next();
		else if (arg == "--ram")
			config.ramMB = atoi(next());
		else if (arg == "--cpus")
			config.cpuCount = atoi(next());
		else if (arg == "--main-rom")
			mainRom = next();
		else if (arg == "--flash")
			config.flashPath = next();
		else if (arg == "--nvram")
			config.nvramPath = next();
		else if (arg == "--threads")
			config.threadPerCpu = true;
		else if (arg == "--rtc-date") {
			int year, month, day, hour = 0, minute = 0;
			const char* value = next();
			if (sscanf(value, "%d-%d-%dT%d:%d", &year, &month, &day, &hour,
					&minute) < 3) {
				fprintf(stderr, "bad --rtc-date %s\n", value);
				return 2;
			}
			rtcDate = Rtc146818::UtcSeconds(year, month, day, hour, minute,
				0);
			haveRtcDate = true;
		}
		else if (arg == "--ide0" || arg == "--ide1")
			config.ideImage[arg[5] - '0'] = next();
		else if (arg.compare(0, 6, "--scsi") == 0 && arg.size() >= 8
			&& arg[6] >= '0' && arg[6] <= '6') {
			// --scsiN-disk FILE, --scsiN-cd FILE
			int id = arg[6] - '0';
			std::string kind = arg.substr(7);
			config.scsi[id].image = next();
			config.scsi[id].kind = kind == "-cd"
				? MachineConfig::ScsiTarget::kCdRom
				: kind == "-share" ? MachineConfig::ScsiTarget::kSharedFolder
				: MachineConfig::ScsiTarget::kDisk;
		} else if (arg == "--floppy")
			config.floppyImage = next();
		else if (arg == "--net")
			config.ethernet = true;
		else if (arg == "--net-forward") {
			config.ethernet = true;
			netForwards = next();
		} else if (arg == "--ne2000") {
			std::string value = next();
			config.ethernet = true;
			config.ethernetPort = (u32)strtoul(value.c_str(), nullptr, 16);
			size_t colon = value.find(':');
			if (colon != std::string::npos)
				config.ethernetIrq = atoi(value.c_str() + colon + 1);
		}
		else if (arg == "--seconds")
			seconds = atof(next());
		else if (arg == "--input") {
			std::string value = next();
			size_t colon1 = value.find(':');
			size_t colon2 = colon1 == std::string::npos ? colon1
				: value.find(':', colon1 + 1);
			int port = atoi(value.c_str());
			if (colon2 == std::string::npos || port < 1 || port > 4) {
				fprintf(stderr, "bad input %s\n", value.c_str());
				return 2;
			}
			std::string text;
			for (size_t j = colon2 + 1; j < value.size(); j++) {
				if (value[j] == '\\' && j + 1 < value.size()) {
					char c = value[++j];
					text += c == 'r' ? '\r' : c == 'n' ? '\n' : c;
				} else
					text += value[j];
			}
			serialInputs.push_back(SerialInput{ port - 1,
				atof(value.c_str() + colon1 + 1), text });
		} else if (arg == "--soft-cursor") {
			softCursor = true;
			if (sscanf(next(), "%d,%d,%d", &softX, &softY, &softNegative)
					< 2 || softNegative < 1) {
				fprintf(stderr, "bad --soft-cursor\n");
				return 2;
			}
		} else if (arg == "--click" || arg == "--press"
			|| arg == "--release" || arg == "--dclick") {
			std::string value = next();
			MouseAction action{ 0, 0, 0, 1, MouseAction::kClick };
			if (arg == "--press")
				action.kind = MouseAction::kPress;
			else if (arg == "--release")
				action.kind = MouseAction::kRelease;
			double ms;
			if (sscanf(value.c_str(), "%lf:%d,%d,%d", &ms, &action.x,
					&action.y, &action.button) < 3) {
				fprintf(stderr, "bad click %s\n", value.c_str());
				return 2;
			}
			action.time = (u64)(ms * 1000);
			mouseActions.push_back(action);
			if (arg == "--dclick")
				mouseActions.push_back(action);
		} else if (arg == "--power-cycle")
			powerCycles.push_back(atof(next()));
		else if (arg == "--debug-boot")
			debugBoot = true;
		else if (arg == "--interrupt")
			interrupts.push_back(atof(next()));
		else if (arg.compare(0, 8, "--serial") == 0 && arg.size() == 9)
			serial[arg[8] - '1'] = next();
		else if (arg == "--log") {
			const char* value = next();
			if (!Log::Configure(value))
				fprintf(stderr, "unknown log category in %s\n", value);
		} else if (arg == "--trace")
			traceCount = strtoull(next(), nullptr, 0);
		else if (arg == "--trace-from")
			traceFromMs = atof(next());
		else if (arg == "--trace-cpus")
			traceCpus = atoi(next());
		else if (arg == "--trace-pc")
			tracePcs.push_back(strtoul(next(), nullptr, 16));
		else if (arg == "--status")
			statusMs = atof(next());
		else if (arg == "--stats")
			stats = true;
		else if (arg == "--stats-from")
			statsFromMs = atof(next());
#ifdef HAVE_PROFILER
		else if (arg == "--profile")
			profile = true;
#endif
		else if (arg == "--video") {
			std::string card = next();
			config.videoCard = card == "none" ? MachineConfig::kVideoNone
				: card == "s3" ? MachineConfig::kVideoS3Vision864
				: MachineConfig::kVideoCirrus5434;
		} else if (arg == "--vram")
			config.vramMB = atoi(next());
		else if (arg == "--screenshot")
			screenshot = next();
		else if (arg == "--audio")
			audioPath = next();
		else if (arg == "--watch")
			watches.push_back(strtoul(next(), nullptr, 16));
		else if (arg == "--watch-write") {
			char* end;
			writeWatchAddress = strtoul(next(), &end, 16);
			writeWatchMs = *end == ':' ? atof(end + 1) : 0;
		}
		else if (arg == "--dump" || arg == "--dump-va") {
			bool virt = arg == "--dump-va";
			std::string value = next();
			char* end;
			u32 address = strtoul(value.c_str(), &end, 0);
			u32 length = *end == ':' ? strtoul(end + 1, &end, 0) : 0;
			if (*end != ':' || length == 0) {
				fprintf(stderr, "bad dump %s\n", value.c_str());
				return 2;
			}
			dumps.push_back(Dump{ address, length, end + 1, virt });
		}
		else if (arg == "--floppy-insert") {
			const char* value = next();
			const char* colon = strchr(value, ':');
			if (colon == nullptr) {
				fprintf(stderr, "bad --floppy-insert %s\n", value);
				return 2;
			}
			floppyChanges.push_back({ atof(value), colon + 1 });
		} else if (arg == "--cd-insert") {
			// MS:N:FILE
			const char* value = next();
			const char* colon = strchr(value, ':');
			const char* second = colon != nullptr ? strchr(colon + 1, ':')
				: nullptr;
			int id = colon != nullptr ? atoi(colon + 1) : -1;
			if (second == nullptr || id < 0 || id > 6) {
				fprintf(stderr, "bad --cd-insert %s\n", value);
				return 2;
			}
			cdChanges.push_back({ atof(value), id, second + 1 });
		} else if (arg == "--key") {
			// MS:NAME[:HOLD] presses a key at MS emulated ms and releases
			// it HOLD ms later (default 100)
			std::string value = next();
			size_t colon = value.find(':');
			size_t colon2 = colon == std::string::npos ? colon
				: value.find(':', colon + 1);
			std::string name = colon == std::string::npos ? ""
				: value.substr(colon + 1, colon2 == std::string::npos
					? std::string::npos : colon2 - colon - 1);
			u32 code = KeyNameToSet2(name.c_str());
			if (code == 0) {
				fprintf(stderr, "bad key %s\n", value.c_str());
				return 2;
			}
			double hold = colon2 == std::string::npos ? 100
				: atof(value.c_str() + colon2 + 1);
			u64 at = (u64)(atof(value.c_str()) * 1000);
			keys.push_back(KeyEvent{ at, code, true });
			keys.push_back(KeyEvent{ at + (u64)(hold * 1000), code, false });
		} else if (arg == "--type") {
			// MS:TEXT types a string, 40 ms per key stroke
			std::string value = next();
			size_t colon = value.find(':');
			if (colon == std::string::npos) {
				fprintf(stderr, "bad type %s\n", value.c_str());
				return 2;
			}
			u64 at = (u64)(atof(value.c_str()) * 1000);
			u32 shift = KeyNameToSet2("LeftShift");
			for (size_t j = colon + 1; j < value.size(); j++) {
				char c = value[j];
				if (c == '\\' && j + 1 < value.size()) {
					char e = value[++j];
					c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
				}
				bool shifted;
				u32 code = AsciiToSet2(c, shifted);
				if (code == 0) {
					fprintf(stderr, "cannot type '%c'\n", c);
					return 2;
				}
				if (shifted)
					keys.push_back(KeyEvent{ at, shift, true });
				keys.push_back(KeyEvent{ at + 5000, code, true });
				keys.push_back(KeyEvent{ at + 25000, code, false });
				if (shifted)
					keys.push_back(KeyEvent{ at + 30000, shift, false });
				at += 40000;
			}
		} else if (arg == "--shots")
			shots = next();
		else {
			Usage();
			return arg == "--help" ? 0 : 2;
		}
	}
	config.nubRomPath = romDir + "/bootnub.rom";
	config.mainRomPath = mainRom.empty() ? romDir + "/bootmain.rom" : mainRom;

	// shared folders: BFS volumes built now, synced back at the end
	std::string error;
	std::vector<std::unique_ptr<SharedFolder> > sharedFolders;
	for (int id = 0; id < 7; id++) {
		MachineConfig::ScsiTarget& target = config.scsi[id];
		if (target.kind != MachineConfig::ScsiTarget::kSharedFolder)
			continue;
		const char* temporary = getenv("TMPDIR");
		std::string image = std::string(temporary != nullptr ? temporary
			: "/tmp") + "/bebox-share-" + std::to_string(getpid()) + "-"
			+ std::to_string(id) + ".qcow2";
		std::unique_ptr<SharedFolder> share(new SharedFolder(target.image,
			image));
		std::string report;
		if (!share->Prepare(report, error)) {
			fprintf(stderr, "bebox-headless: %s\n", error.c_str());
			return 1;
		}
		target.image = image;
		sharedFolders.push_back(std::move(share));
	}

	std::unique_ptr<Machine> ownedMachine = Machine::Create(config);
	Machine& machine = *ownedMachine;
	BeBoxMachine* bebox = dynamic_cast<BeBoxMachine*>(&machine);
	HobbitMachine* hobbit = dynamic_cast<HobbitMachine*>(&machine);
	if (!machine.Init(error)) {
		fprintf(stderr, "bebox-headless: %s\n", error.c_str());
		return 1;
	}
	if (debugBoot)
		machine.HoldDebugOutputKey();
	if (haveRtcDate)
		machine.Rtc().SetTimeOffset(rtcDate - (s64)time(nullptr));
	for (int id = 0; id < 7; id++) {
		if (ScsiCd* cd = machine.CdRom(id)) {
			cd->SetEjectCallback([&machine, id](const std::string& path) {
				Status("[%9.3f ms] CD-ROM %d: the guest ejected %s\n",
					machine.Now() * 1000.0 / machine.TicksPerSecond(), id,
					path.c_str());
			});
		}
	}

	std::unique_ptr<NetworkBackend> network;
	if (config.ethernet) {
		UserNetworkConfig networkConfig;
		if (!ParsePortForwards(netForwards, networkConfig.forwards, error)) {
			fprintf(stderr, "bebox-headless: %s\n", error.c_str());
			return 2;
		}
		network = CreateUserNetwork(networkConfig, error);
		if (!network) {
			fprintf(stderr, "bebox-headless: %s\n", error.c_str());
			return 1;
		}
		machine.SetNetworkBackend(network.get());
	}

	std::vector<std::unique_ptr<FileSerial> > backends;
	std::vector<std::unique_ptr<SerialBackend> > otherBackends;
	for (int port = 0; port < 4; port++) {
		const std::string& destination = serial[port];
		if (destination == "null")
			continue;
		if (destination.compare(0, 4, "tcp-") == 0
			|| destination.compare(0, 5, "host:") == 0
			|| destination.compare(0, 5, "file:") == 0) {
			std::string error;
			std::unique_ptr<SerialBackend> backend
				= CreateSerialBackend(destination, error);
			if (!backend) {
				fprintf(stderr, "serial%d: %s\n", port + 1, error.c_str());
				return 1;
			}
			machine.SetSerialBackend(port, backend.get());
			otherBackends.push_back(std::move(backend));
			continue;
		}
		FILE* file = destination == "stdout" ? stdout
			: fopen(destination.c_str(), "w");
		if (file == nullptr) {
			perror(destination.c_str());
			return 1;
		}
		static const char* const kLabels[] = { "serial1", "serial2",
			"serial3", "serial4" };
		backends.emplace_back(new FileSerial(file, kLabels[port],
			destination == "stdout", machine.GetScheduler()));
		machine.SetSerialBackend(port, backends.back().get());
		for (const SerialInput& input : serialInputs) {
			if (input.port == port) {
				backends.back()->Queue((u64)(input.ms
					* machine.TicksPerSecond() / 1000), input.text);
			}
		}
	}

	FrameCapture capture;
	machine.SetVideoSink(&capture);

	TraceHook trace(stderr, traceCount, machine.TicksPerSecond());
	for (u32 pc : tracePcs)
		trace.AddPc(pc);
	trace.SetStart((u64)(traceFromMs * machine.TicksPerSecond() / 1000));
	trace.SetCpuMask(traceCpus);
	if (traceCount > 0 && bebox != nullptr) {
		for (int i = 0; i < bebox->CpuCount(); i++)
			bebox->Cpu(i)->SetTraceHook(&trace);
	}
	if (traceCount > 0 && hobbit != nullptr) {
		hobbit->SetTrace(stderr, traceCount,
			(u64)(traceFromMs * machine.TicksPerSecond() / 1000), traceCpus,
			tracePcs);
	}

	// profiling from statsFromMs on (and the speed reported for that part)
	bool profiling = false;
	auto startProfiling = [&]() {
		profiling = true;
		if (stats)
			machine.SetProfiling(true);
#ifdef HAVE_PROFILER
		if (profile)
			StartProfile();
#endif
	};
	if ((stats || profile) && statsFromMs <= 0)
		startProfiling();
	auto start = std::chrono::steady_clock::now();
	u64 total = (u64)(seconds * machine.TicksPerSecond());
	u64 step = machine.MicrosecondsToTicks(1000);	// 1 ms
	u64 statusEvery = statusMs > 0
		? machine.MicrosecondsToTicks((u64)(statusMs * 1000)) : 0;
	u64 nextStatus = statusEvery;
	u8 lastLeds = 0xff;
	bool lastDisk = false;
	u64 done = 0;
	std::vector<u32> watchValues;
	for (u32 address : watches)
		watchValues.push_back(machine.Bus().Read32(address, kInitiatorDma));
	std::unique_ptr<WriteWatch> writeWatch;
	std::unique_ptr<WavSink> wav;
	if (!audioPath.empty()) {
		wav.reset(new WavSink(audioPath));
		machine.SetAudioSink(wav.get());
	}
	// mouse script state
	size_t mouseIndex = 0;
	enum { kMouseIdle, kMouseMoving, kMousePressed, kMouseReleased }
		mousePhase = kMouseIdle;
	u64 mouseNext = 0;
	int mouseStuck = 0;
	u8 mouseButtons = 0;
	while (done < total) {
		u64 nowUs = machine.Now() * 1000000 / machine.TicksPerSecond();
		if (writeWatchMs >= 0 && !writeWatch && nowUs >= writeWatchMs * 1000) {
			writeWatch.reset(new WriteWatch(machine.Bus(), writeWatchAddress,
				machine.TicksPerSecond(), machine.GetScheduler()));
			writeWatch->Install();
		}
		if ((stats || profile) && !profiling
			&& nowUs >= (u64)(statsFromMs * 1000))
			startProfiling();
		u64 chunk = std::min(step, total - done);
		machine.RunFor(chunk);
		done += chunk;
		nowUs = machine.Now() * 1000000 / machine.TicksPerSecond();
		if (mouseIndex < mouseActions.size() && nowUs >= mouseNext) {
			const MouseAction& action = mouseActions[mouseIndex];
			int cx, cy;
			switch (mousePhase) {
				case kMouseIdle:
					if (nowUs >= action.time)
						mousePhase = kMouseMoving;
					break;
				case kMouseMoving:
					if (softCursor) {
						cx = softX;
						cy = softY;
					} else if (machine.Video() == nullptr
						|| !machine.Video()->HardwareCursor(cx, cy)) {
						static bool warned = false;
						if (!warned) {
							fprintf(stderr, "mouse: no hardware cursor yet\n");
							warned = true;
						}
						mouseNext = nowUs + 20000;
						break;
					}
					if (cx == action.x && cy == action.y) {
						if (action.kind == MouseAction::kRelease) {
							mouseButtons = 0;
							machine.Keyboard().MouseMoved(0, 0, 0, 0);
							mousePhase = kMouseReleased;
						} else if (action.button > 0) {
							mouseButtons = 1 << (action.button - 1);
							machine.Keyboard().MouseMoved(0, 0, 0,
								mouseButtons);
							mousePhase = action.kind == MouseAction::kPress
								? kMouseReleased : kMousePressed;
						} else
							mousePhase = kMouseReleased;
						mouseStuck = 0;
						mouseNext = nowUs + 60000;
						break;
					}
					if (++mouseStuck > (softCursor ? 2000 : 400)) {
						fprintf(stderr, "mouse: cannot reach %d,%d (at %d,%d)"
							"\n", action.x, action.y, cx, cy);
						mousePhase = kMouseReleased;
						mouseStuck = 0;
						break;
					}
					{
						// small steps: the guest accelerates large ones
						int step = softCursor ? 1 : 6;
						int dx = std::max(-step, std::min(step, action.x - cx));
						int dy = std::max(-step, std::min(step, action.y - cy));
						machine.Keyboard().MouseMoved(dx, -dy, 0,
							mouseButtons);
						// open loop: a count at a time (nothing
						// accelerates it); left and down may move further
						// (an overshoot is undone by a step back)
						softX += dx < 0 ? dx * softNegative : dx;
						softY += dy > 0 ? dy * softNegative : dy;
					}
					mouseNext = nowUs + (softCursor ? 50000 : 15000);
					break;
				case kMousePressed:
					mouseButtons = 0;
					machine.Keyboard().MouseMoved(0, 0, 0, 0);
					mousePhase = kMouseReleased;
					mouseNext = nowUs + 60000;
					break;
				case kMouseReleased:
					Status("[%9.3f ms] mouse at %d,%d%s\n", nowUs / 1000.0,
						action.x, action.y,
						action.kind == MouseAction::kPress ? " pressed"
						: action.kind == MouseAction::kRelease ? " released"
						: action.button > 0 ? " clicked" : "");
					mouseIndex++;
					mousePhase = kMouseIdle;
					break;
			}
		}
		for (double& at : powerCycles) {
			if (at >= 0 && nowUs >= at * 1000) {
				machine.PowerOn();
				if (debugBoot)
					machine.HoldDebugOutputKey();
				at = -1;
			}
		}
		for (double& at : interrupts) {
			if (at >= 0 && nowUs >= at * 1000) {
				machine.PressInterruptSwitch();
				at = -1;
			}
		}
		for (size_t i = 0; i < watches.size(); i++) {
			u32 value = machine.Bus().Read32(watches[i], kInitiatorDma);
			if (value != watchValues[i]) {
				Status("[%9.3f ms] watch %08x: %08x -> %08x\n",
					nowUs / 1000.0, watches[i], watchValues[i], value);
				watchValues[i] = value;
			}
		}
		for (FloppyChange& change : floppyChanges) {
			if (change.ms < 0 || nowUs < change.ms * 1000
				|| machine.Floppy() == nullptr)
				continue;
			change.ms = -1;
			int unit = machine.FloppyUnit();
			machine.Floppy()->EjectDisk(unit);
			if (!change.image.empty()) {
				std::unique_ptr<DiskImage> image = DiskImage::Open(
					change.image, false, error);
				if (!image || !machine.Floppy()->InsertDisk(unit,
						std::move(image), false, error)) {
					fprintf(stderr, "bebox-headless: %s\n", error.c_str());
					continue;
				}
			}
			Status("[%9.3f ms] floppy %s\n", nowUs / 1000.0,
				change.image.empty() ? "ejected" : change.image.c_str());
		}
		for (CdChange& change : cdChanges) {
			if (change.ms < 0 || nowUs < change.ms * 1000)
				continue;
			change.ms = -1;
			ScsiCd* cd = machine.CdRom(change.id);
			if (cd == nullptr) {
				fprintf(stderr, "bebox-headless: no CD-ROM drive on SCSI "
					"ID %d\n", change.id);
				continue;
			}
			std::unique_ptr<CdImage> image;
			if (!change.image.empty()) {
				image = CdImage::Open(change.image, error);
				if (!image) {
					fprintf(stderr, "bebox-headless: %s\n", error.c_str());
					continue;
				}
			}
			cd->Change(std::move(image));
			Status("[%9.3f ms] CD-ROM %d %s\n", nowUs / 1000.0, change.id,
				change.image.empty() ? "ejected" : change.image.c_str());
		}
		for (KeyEvent& key : keys) {
			if (key.code != 0 && nowUs >= key.time) {
				if (key.down)
					machine.Keyboard().KeyDown(key.code);
				else
					machine.Keyboard().KeyUp(key.code);
				key.code = 0;
			}
		}
		u8 leds = bebox != nullptr ? bebox->Board().LedLatch() : 0;
		bool disk = bebox != nullptr && bebox->Board().DiskLed();
		if (bebox != nullptr && (leds != lastLeds || disk != lastDisk)) {
			Status("[%9.3f ms] LEDs %2d %2d disk %s\n",
				machine.Now() * 1000.0 / machine.TicksPerSecond(), leds >> 4,
				leds & 15, disk ? "on" : "off");
			lastLeds = leds;
			lastDisk = disk;
		}
		if (!shots.empty() && done % machine.TicksPerSecond() < step
			&& capture.width > 0) {
			std::string path = shots + "-" + std::to_string(
				done / machine.TicksPerSecond()) + ".png";
			WritePng(path, capture.pixels.data(), capture.width,
				capture.height, capture.width);
		}
		if (statusEvery != 0 && done >= nextStatus) {
			nextStatus += statusEvery;
			for (int i = 0; i < machine.CpuCount(); i++) {
				Status("[%9.3f ms] cpu%d %s\n",
					machine.Now() * 1000.0 / machine.TicksPerSecond(), i,
					machine.CpuState(i).c_str());
			}
		}
	}
	double elapsed = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - start).count();

	if (!sPendingStatus.empty()) {
		printf("\n%s", sPendingStatus.c_str());
		sPendingStatus.clear();
	}
	printf("emulated %.3f s in %.3f s (%.0f%%), %.1f MIPS\n",
		machine.Now() / (double)machine.TicksPerSecond(), elapsed,
		100.0 * machine.Now() / machine.TicksPerSecond() / elapsed,
		machine.InstructionCount() / elapsed / 1e6);
	for (int i = 0; i < machine.CpuCount(); i++)
		printf("cpu%d: %s\n", i, machine.CpuState(i).c_str());
	if (stats)
		printf("%s", machine.Statistics().c_str());
#ifdef HAVE_PROFILER
	if (profile)
		ReportProfile(40);
#endif
	for (const Dump& dump : dumps) {
		std::vector<u8> data(dump.length);
		if (dump.virt) {
			// page by page through CPU0's translation (unmapped: zeroes)
			for (u32 done = 0; done < dump.length;) {
				u32 va = dump.address + done;
				u32 chunk = std::min<u32>(dump.length - done,
					0x1000 - (va & 0xfff));
				u32 pa;
				if (machine.DebugTranslate(0, va, pa))
					machine.Bus().DmaRead(pa, data.data() + done, chunk);
				done += chunk;
			}
		} else
			machine.Bus().DmaRead(dump.address, data.data(), dump.length);
		FILE* file = fopen(dump.file.c_str(), "wb");
		if (file == nullptr
			|| fwrite(data.data(), 1, data.size(), file) != data.size())
			fprintf(stderr, "cannot write %s\n", dump.file.c_str());
		if (file != nullptr)
			fclose(file);
	}
	if (!screenshot.empty() && capture.width > 0) {
		WritePng(screenshot, capture.pixels.data(), capture.width,
			capture.height, capture.width);
		printf("screenshot %dx%d saved to %s (%llu frames)\n", capture.width,
			capture.height, screenshot.c_str(),
			(unsigned long long)capture.frames);
	}
	if (machine.Video() != nullptr) {
		const VgaCore::DisplayMode& mode = machine.Video()->CurrentMode();
		printf("display: format %d, %dx%d, start %u, pitch %u\n",
			(int)mode.format, mode.width, mode.height, mode.start,
			mode.pitch);
	}
	int cursorX, cursorY;
	if (machine.Video() != nullptr
		&& machine.Video()->HardwareCursor(cursorX, cursorY))
		printf("hardware cursor at %d,%d\n", cursorX, cursorY);
	printf("unclaimed accesses: %llu memory, %llu I/O\n",
		(unsigned long long)machine.Bus().UnclaimedAccessCount(),
		(unsigned long long)machine.UnclaimedIoCount());
	if (!machine.SavePersistentState(error))
		fprintf(stderr, "bebox-headless: %s\n", error.c_str());

	// the disks are closed with the machine: then the shared folders sync
	ownedMachine.reset();
	for (auto& share : sharedFolders) {
		std::string report;
		if (!share->Sync(report, error))
			fprintf(stderr, "bebox-headless: %s\n", error.c_str());
		else if (!report.empty())
			printf("%s\n", report.c_str());
	}
	return 0;
}
