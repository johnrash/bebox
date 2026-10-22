/*
 * BeBox emulator - device tests: the game port A/D converter, the S3
 * Vision864's RAMDAC access, graphics engine and cursor, the Cirrus
 * GD5434's cursor, the NE2000, the SCSI CD-ROM's disc changes, the
 * boot ROM's debug output key, the MPC105's memory banks, the threaded
 * mode's choice of one host thread or two, timers destroyed while armed,
 * disk image write errors and the log's settings.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "core/board/BootItems.h"
#include "core/board/Scheduler.h"
#include "core/Log.h"
#include "core/board/ThreadGovernor.h"
#include "core/bus/IoSpace.h"
#include "core/bus/Mpc105.h"
#include "core/bus/PciBus.h"
#include "core/bus/PhysBus.h"
#include "core/host/NetworkBackend.h"
#include "core/host/VideoSink.h"
#include "core/isa/JoystickAdc.h"
#include "core/isa/Kbc8042.h"
#include "core/isa/Ne2000.h"
#include "core/isa/Rtc146818.h"
#include "core/isa/Uart16550.h"
#include "core/pci/ScsiDevice.h"
#include "core/storage/CdImage.h"
#include "core/storage/DiskImage.h"
#include "core/video/CirrusGd5434.h"
#include "core/video/S3Vision864.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

#include <unistd.h>

using namespace bebox;

namespace {

int sFailures = 0;

void
Check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok" : "FAILED", what);
	if (!ok)
		sFailures++;
}


// The BeOS a2d driver's sequence: select the multiplexer input, write the
// MAX186 control byte, wait for done, read the 12-bit result.
u16
ReadAxis(Scheduler& scheduler, u64& now, JoystickAdc& adc, u8 control,
	int mux)
{
	adc.IoWrite(0x36e, (u32)mux << 8, 2);
	adc.IoWrite8(0x360, control);
	now += 1000000;
	scheduler.SetNow(now);
	scheduler.RunDue(now);
	if ((adc.IoRead(0x360, 2) & 0x800) == 0)
		return 0xffff;
	return (u16)(adc.IoRead(0x362, 2) & 0xfff);
}


void
TestGamePorts()
{
	Scheduler scheduler;
	u64 now = 0;
	JoystickAdc adc(scheduler, 264000000, 5);
	adc.SetStick(0, 100, 200, true, false);
	adc.SetStick(1, 300, 400, false, true);
	adc.SetStick(2, 500, 600, false, false);
	adc.SetStick(3, 700, 800, true, true);
	Check(ReadAxis(scheduler, now, adc, 0x2c, 0) == 100, "stick 1 X");
	Check(ReadAxis(scheduler, now, adc, 0x2c, 1) == 200, "stick 1 Y");
	Check(ReadAxis(scheduler, now, adc, 0x2c, 2) == 300, "stick 2 X");
	Check(ReadAxis(scheduler, now, adc, 0x2c, 3) == 400, "stick 2 Y");
	Check(ReadAxis(scheduler, now, adc, 0x6c, 0) == 500, "stick 3 X");
	Check(ReadAxis(scheduler, now, adc, 0x6c, 3) == 800, "stick 4 Y");
	Check(ReadAxis(scheduler, now, adc, 0x3c, 0) == 0xfff,
		"channel 6: I/O card revision 5");
	// active low: stick 1 button 1 (01h), stick 2 button 2 (04h), stick 4
	// both (20h, 40h)
	Check(adc.IoRead8(0x397) == (0xff & ~0x01 & ~0x04 & ~0x20 & ~0x40),
		"buttons");
}


class S3Test {
public:
	S3Test()
		:
		fCard(fBus, fIo, fScheduler, 264000000, 2 << 20)
	{
		Crtc(0x38, 0x48);
		Crtc(0x39, 0xa5);
		Crtc(0x40, 0x01);		// enhanced registers
		Crtc(0x50, 0x40);		// engine: 640 pixels, 8 bpp
	}

	void Crtc(u8 index, u8 value)
	{
		fIo.Write(0x3d4, index, 1);
		fIo.Write(0x3d5, value, 1);
	}

	void Write16(u32 port, u16 value) { fIo.Write(port, value, 2); }
	u8 Pixel(int x, int y) { return fCard.Vram()[y * 640 + x]; }

	Scheduler	fScheduler;
	PhysBus		fBus;
	IoSpace		fIo;
	S3Vision864	fCard;
};


void
TestS3Dac()
{
	S3Test t;
	for (int i = 0; i < 4; i++)
		t.fIo.Read(0x3c6, 1);
	t.fIo.Write(0x3c6, 0x56, 1);		// command register: 32 bpp, 8-bit
	for (int i = 0; i < 4; i++)
		t.fIo.Read(0x3c6, 1);
	u8 command = (u8)t.fIo.Read(0x3c6, 1);
	u8 vendor = (u8)t.fIo.Read(0x3c6, 1);
	u8 device = (u8)t.fIo.Read(0x3c6, 1);
	Check(command == 0x56, "21C498 command register after four reads");
	Check(vendor == 0x84 && device == 0x98, "21C498 identification");
	Check(t.fIo.Read(0x3c6, 1) == 0xff, "pixel mask afterwards");
}


void
TestS3Engine()
{
	S3Test t;
	// rectangle fill 10x4 at (20, 30); the color written as one byte, as
	// BeOS does in 8 bpp
	t.Write16(0x86e8, 20);
	t.Write16(0x82e8, 30);
	t.Write16(0x96e8, 9);
	t.Write16(0xbee8, 0x0000 | 3);
	t.fIo.Write(0xa6e8, 0x5a, 1);
	t.Write16(0xbae8, 0x27);
	t.Write16(0x9ae8, 0x40b3);
	Check(t.Pixel(20, 30) == 0x5a && t.Pixel(29, 33) == 0x5a,
		"rectangle fill");
	Check(t.Pixel(19, 30) == 0 && t.Pixel(30, 30) == 0
		&& t.Pixel(20, 34) == 0, "rectangle fill bounds");

	// BitBLT the rectangle to (100, 50)
	t.Write16(0x86e8, 20);
	t.Write16(0x82e8, 30);
	t.Write16(0x8ee8, 100);
	t.Write16(0x8ae8, 50);
	t.Write16(0x96e8, 9);
	t.Write16(0xbee8, 0x0000 | 3);
	t.Write16(0xbae8, 0x67);
	t.Write16(0x9ae8, 0xc0b3);
	Check(t.Pixel(100, 50) == 0x5a && t.Pixel(109, 53) == 0x5a
		&& t.Pixel(110, 53) == 0, "BitBLT");

	// scissors clip a fill
	t.Write16(0xbee8, 0x1000 | 0);			// top
	t.Write16(0xbee8, 0x2000 | 0);			// left
	t.Write16(0xbee8, 0x3000 | 479);		// bottom
	t.Write16(0xbee8, 0x4000 | 204);		// right
	t.Write16(0x86e8, 200);
	t.Write16(0x82e8, 10);
	t.Write16(0x96e8, 9);
	t.Write16(0xbee8, 0x0000);
	t.fIo.Write(0xa6e8, 0x33, 1);
	t.Write16(0xbae8, 0x27);
	t.Write16(0x9ae8, 0x40b3);
	Check(t.Pixel(204, 10) == 0x33 && t.Pixel(205, 10) == 0,
		"scissors");

	// horizontal Bresenham line of 5 pixels from (300, 100)
	t.Write16(0xbee8, 0x4000 | 639);
	t.Write16(0x86e8, 300);
	t.Write16(0x82e8, 100);
	t.Write16(0x96e8, 4);
	t.Write16(0x8ae8, 0);					// axial step
	t.Write16(0x8ee8, (u16)-8);				// diagonal step
	t.Write16(0x92e8, (u16)-5);				// error term
	t.fIo.Write(0xa6e8, 0x77, 1);
	t.Write16(0x9ae8, 0x2033 | 0x0400);
	Check(t.Pixel(300, 100) == 0x77 && t.Pixel(304, 100) == 0x77
		&& t.Pixel(305, 100) == 0 && t.Pixel(302, 99) == 0, "line");

	// monochrome image transfer: 16x1 with CPU data selecting the mix
	t.Write16(0x86e8, 400);
	t.Write16(0x82e8, 200);
	t.Write16(0x96e8, 15);
	t.Write16(0xbee8, 0x0000);
	t.Write16(0xbee8, 0xa000 | 0x080);		// mix select: CPU data
	t.fIo.Write(0xa6e8, 0x11, 1);
	t.fIo.Write(0xa2e8, 0x22, 1);
	t.Write16(0xbae8, 0x27);
	t.Write16(0xb6e8, 0x07);
	t.Write16(0x9ae8, 0x4000 | 0x1000 | 0x0200 | 0x01b3);	// 16-bit bus
	t.Write16(0xe2e8, 0x0ff0);				// low byte first: f0 0f
	Check(t.Pixel(400, 200) == 0x11 && t.Pixel(403, 200) == 0x11
		&& t.Pixel(404, 200) == 0x22 && t.Pixel(411, 200) == 0x22
		&& t.Pixel(412, 200) == 0x11, "monochrome transfer");
}

class FrameCapture : public VideoSink {
public:
	virtual void FrameReady(const u32* pixels, int width, int height,
		int stride)
	{
		fPixels.assign(pixels, pixels + (size_t)stride * height);
		fWidth = width;
		fStride = stride;
	}

	u32 Pixel(int x, int y) const
	{
		return x < fWidth ? fPixels[(size_t)y * fStride + x] : 0xdeadbeef;
	}

	std::vector<u32> fPixels;
	int fWidth = 0;
	int fStride = 0;
};


void
TestS3Cursor()
{
	S3Test t;
	t.Crtc(0x45, 0x01);			// cursor on
	t.Crtc(0x46, 0x03);			// x = 840
	t.Crtc(0x47, 0x48);
	t.Crtc(0x48, 0x01);			// y = 263
	t.Crtc(0x49, 0x07);
	int x, y;
	Check(t.fCard.HardwareCursor(x, y) && x == 840 && y == 263,
		"cursor position in 8 bpp: pixels");
	// 32 bpp through the 21C498 (CR67 pixel mode 7): BeOS's drivers write
	// twice the pointer's x
	t.Crtc(0x67, 0x70);
	Check(t.fCard.HardwareCursor(x, y) && x == 420 && y == 263,
		"cursor position in 32 bpp: half pixels");

	// The pattern columns are half pixels too (BeOS doubles every pixel of
	// its cursor in 32 bpp): two inverting columns cover one pixel.
	S3Test r;
	FrameCapture frame;
	r.fCard.SetVideoSink(&frame);
	r.Crtc(0x3a, 0x10);				// enhanced mode
	r.Crtc(0x01, 79);				// 640 / 8 - 1
	r.Crtc(0x12, 479 & 0xff);
	r.Crtc(0x07, 0x02);
	r.Crtc(0x13, 80);
	r.fIo.Read(0x3da, 1);
	r.fIo.Write(0x3c0, 0x20, 1);		// display on
	r.Crtc(0x4c, 0x01);				// pattern at 256 KB
	r.Crtc(0x4d, 0x00);
	// row 0, columns 0 and 1: A and B set (invert)
	r.fCard.Vram()[0x40000] = 0xc0;
	r.fCard.Vram()[0x40002] = 0xc0;
	r.Crtc(0x45, 0x01);
	r.Crtc(0x46, 0x00);				// x = 40 (cursor units)
	r.Crtc(0x47, 40);
	r.Crtc(0x48, 0x00);
	r.Crtc(0x49, 10);
	r.fCard.RenderFrame();
	Check(frame.Pixel(40, 10) == 0xffffff && frame.Pixel(41, 10) == 0xffffff
		&& frame.Pixel(42, 10) == 0, "cursor pattern in 8 bpp: pixels");
	r.Crtc(0x67, 0x70);				// 32 bpp pixel mode: 2 units a pixel
	r.fCard.RenderFrame();
	Check(frame.Pixel(20, 10) == 0xffffff && frame.Pixel(21, 10) == 0
		&& frame.Pixel(19, 10) == 0, "cursor pattern in 32 bpp: half pixels");
}




// The cursor patterns are in the last 16 KB of the memory in use: with 4 MB
// installed but DRAM bank switching off (as BeOS's driver sets SR0F), that
// is below 2 MB.
void
TestCirrusCursor()
{
	Scheduler scheduler;
	PhysBus bus;
	IoSpace io;
	CirrusGd5434 card(bus, io, scheduler, 264000000, 4 << 20);
	FrameCapture frame;
	card.SetVideoSink(&frame);
	auto seq = [&](u8 index, u8 value) {
		io.Write(0x3c4, index, 1);
		io.Write(0x3c5, value, 1);
	};
	auto crtc = [&](u8 index, u8 value) {
		io.Write(0x3d4, index, 1);
		io.Write(0x3d5, value, 1);
	};
	// a 640x480 display, all black
	crtc(0x01, 79);
	crtc(0x12, 479 & 0xff);
	crtc(0x07, 0x02);
	crtc(0x13, 80);
	io.Read(0x3da, 1);
	io.Write(0x3c0, 0x20, 1);			// palette address source: display on
	seq(0x06, 0x12);					// unlock the extensions
	seq(0x0f, 0x38);					// 64-bit bus, bank switching off
	seq(0x13, 0x3f);					// 32x32 pattern 63
	seq(0x10 | (4 << 5), 100 >> 3);		// x = 100
	seq(0x11, 50 >> 3);
	seq(0x11 | (2 << 5), 50 >> 3);		// y = 50
	seq(0x12, 0x01);					// cursor on
	// first row of pattern 63 below 2 MB: plane 0 set, plane 1 clear
	// (inverts the screen)
	u32 pattern = (2u << 20) - 16384 + 63 * 256;
	memset(card.Vram() + pattern, 0xff, 4);
	card.RenderFrame();
	Check(frame.Pixel(100, 50) == 0xffffff && frame.Pixel(131, 50) == 0xffffff
		&& frame.Pixel(132, 50) == 0 && frame.Pixel(100, 51) == 0,
		"cursor pattern below 2 MB without bank switching");
	seq(0x0f, 0xb8);					// bank switching: 4 MB in use
	card.RenderFrame();
	Check(frame.Pixel(100, 50) == 0, "cursor pattern below 4 MB with it");
}


// The BeBox clock runs in binary mode (BeOS DR6-PR2 read it so); clocks
// saved in BCD mode by earlier versions become binary.
void
TestRtc()
{
	Scheduler scheduler;
	Rtc146818 rtc(scheduler, 264000000);
	rtc.SetTimeOffset(Rtc146818::UtcSeconds(2026, 10, 11, 12, 34, 20)
		- (s64)time(nullptr));
	auto read = [&](u8 index) {
		rtc.IoWrite8(0x70, index);
		return rtc.IoRead8(0x71);
	};
	Check(read(0x0b) == 0x06, "clock in binary 24 hour mode");
	Check(read(9) == 26 && read(8) == 10 && read(7) == 11 && read(4) == 12
		&& read(2) == 34, "binary date and time");
	// a guest choosing BCD gets BCD
	rtc.IoWrite8(0x70, 0x0b);
	rtc.IoWrite8(0x71, 0x02);
	Check(read(9) == 0x26 && read(8) == 0x10 && read(4) == 0x12,
		"BCD when the guest sets it");
	// saved by an earlier version (BCD mode): binary
	u8 saved[Rtc146818::kRamSize] = {};
	saved[0x0a] = 0x26;
	saved[0x0b] = 0x02;
	rtc.LoadRam(saved);
	Check(read(0x0b) == 0x06 && read(9) == 26, "saved BCD clock becomes "
		"binary");
}


// The boot ROM 1.1d7's PCI table for a kernel that only knows the older
// "pci_table_v2" (BeOS DR6): converted in place at the jump into the kernel.
void
TestBootItems()
{
	PhysBus bus;
	std::vector<u8> ram(8 << 20);
	bus.MapMemory(0, (u32)ram.size(), ram.data(), true);
	u8* m = ram.data();
	const u32 kItem = 0x700000;
	const u32 kData = kItem + 16;
	const u32 kName = kData + 120;
	// the memory test's address pattern: not an entry point yet
	StoreBE32(m + 0x3018, 0x3018);
	Check(BootItems::EntryPoint(bus) == 0, "no entry point during the "
		"memory test");
	StoreBE32(m + 0x3014, 0xfeedbabe);
	StoreBE32(m + 0x3018, 0x36ed0);
	StoreBE32(m + 0x301c, kItem);
	Check(BootItems::EntryPoint(bus) == 0x36ed0, "entry point");
	// one item: the 53C810 with an I/O and a memory BAR
	StoreBE32(m + kItem, 0);
	StoreBE32(m + kItem + 4, kName);
	StoreBE32(m + kItem + 8, 120);
	StoreBE32(m + kItem + 12, kData);
	strcpy((char*)m + kName, "pci_table_v5");
	u8* v5 = m + kData;
	StoreBE16(v5 + 0, 0x1000);			// vendor
	StoreBE16(v5 + 2, 0x0001);			// device
	v5[4] = 0;							// bus
	v5[5] = 12;							// device number
	v5[6] = 0;							// function
	v5[7] = 2;							// revision
	v5[10] = 1;							// class: mass storage
	StoreBE32(v5 + 24, 0xc2002000);		// expansion ROM
	StoreBE32(v5 + 32, 0x800);
	StoreBE32(v5 + 36, 0x81000000);		// BAR 0, processor view
	StoreBE32(v5 + 40, 0xc2002800);
	StoreBE32(v5 + 60, 0x01000000);		// BAR 0, PCI view
	StoreBE32(v5 + 84, 0x100);			// sizes
	StoreBE32(v5 + 88, 0x100);
	v5[108] = 1;						// BAR 0 is I/O space
	v5[114] = 20;						// interrupt line
	v5[115] = 1;						// pin

	// a kernel that knows the newer table: nothing changes
	strcpy((char*)m + 0x20000, "pci_table_v2");
	strcpy((char*)m + 0x20010, "pci_table_v5");
	std::string log;
	Check(!BootItems::AdaptForKernel(bus, log)
		&& strcmp((char*)m + kName, "pci_table_v5") == 0,
		"boot items kept for a kernel that knows pci_table_v5");
	// DR6's kernel
	memset(m + 0x20010, 0, 16);
	Check(BootItems::AdaptForKernel(bus, log), "boot items adapted");
	const u8* v2 = m + kData;
	Check(strcmp((char*)m + kName, "pci_table_v2") == 0
		&& LoadBE32(m + kItem + 8) == 112, "pci_table_v2 item");
	Check(v2[0] == 0 && v2[1] == 12 && v2[2] == 0
		&& LoadBE16(v2 + 4) == 0x1000 && LoadBE16(v2 + 6) == 0x0001
		&& v2[8] == 2 && v2[11] == 1, "v2 record: identification");
	Check(LoadBE32(v2 + 16) == 0xc2002000 && LoadBE32(v2 + 24) == 0x800
		&& LoadBE32(v2 + 28) == 0x81000000 && LoadBE32(v2 + 32) == 0xc2002800
		&& LoadBE32(v2 + 52) == 0x01000000 && LoadBE32(v2 + 76) == 0x100
		&& LoadBE32(v2 + 80) == 0x100 && v2[100] == 1 && v2[106] == 20
		&& v2[107] == 1, "v2 record: resources");
	Check(!BootItems::AdaptForKernel(bus, log), "adapted only once");
}


class FakeNetwork : public NetworkBackend {
public:
	virtual void Send(const u8* frame, size_t length)
	{
		sent.assign(frame, frame + length);
	}

	virtual void Poll() {}

	bool Deliver(const u8* frame, size_t length)
	{
		return fReceiver(frame, length);
	}

	std::vector<u8> sent;
};


void
TestNe2000()
{
	Scheduler scheduler;
	const u8 mac[6] = { 0x00, 0x00, 0xe8, 0x01, 0x02, 0x03 };
	Ne2000 card(scheduler, 264000000, mac);
	FakeNetwork network;
	card.SetBackend(&network);
	bool irq = false;
	card.SetIrqCallback([&irq](bool level) { irq = level; });
	const u32 base = 0x300;
	auto out = [&card](u32 port, u32 value) { card.IoWrite(port, value, 1); };
	auto in = [&card](u32 port) { return card.IoRead(port, 1); };

	// reading the reset port resets; ISR shows RST
	card.IoRead(base + 0x1f, 1);
	Check((in(base + 7) & 0x80) != 0, "reset");

	// station address PROM through byte wide remote DMA
	out(base + 0x0e, 0x48);		// DCR: byte transfers
	out(base + 0x0a, 32);		// RBCR
	out(base + 0x0b, 0);
	out(base + 0x08, 0);		// RSAR
	out(base + 0x09, 0);
	out(base, 0x0a);			// remote read, stop
	u8 prom[32];
	for (int i = 0; i < 32; i++)
		prom[i] = (u8)card.IoRead(base + 0x10, 1);
	bool macOk = true;
	for (int i = 0; i < 6; i++)
		macOk &= prom[i * 2] == mac[i] && prom[i * 2 + 1] == mac[i];
	Check(macOk && prom[28] == 0x57 && prom[30] == 0x57,
		"address PROM (NE2000 signature)");
	Check((in(base + 7) & 0x40) != 0, "remote DMA done");

	// start: ring 46h-80h, broadcasts accepted, interrupts enabled
	out(base + 0x0e, 0x49);		// word transfers
	out(base + 0x01, 0x46);
	out(base + 0x02, 0x80);
	out(base + 0x03, 0x46);
	out(base, 0x61);			// page 1
	for (int i = 0; i < 6; i++)
		out(base + 1 + i, mac[i]);
	out(base + 0x07, 0x47);		// CURR
	out(base, 0x22);			// page 0, start
	out(base + 0x0c, 0x04);		// RCR: broadcast
	out(base + 0x07, 0xff);		// clear ISR
	out(base + 0x0f, 0x03);		// IMR: received, transmitted

	// transmit 64 bytes from page 40h
	u8 frame[64];
	for (int i = 0; i < 64; i++)
		frame[i] = (u8)(i * 3);
	out(base + 0x0a, 64);
	out(base + 0x0b, 0);
	out(base + 0x08, 0x00);
	out(base + 0x09, 0x40);
	out(base, 0x12);			// remote write
	for (int i = 0; i < 64; i += 2)
		card.IoWrite(base + 0x10, frame[i] | (frame[i + 1] << 8), 2);
	out(base + 0x04, 0x40);		// TPSR
	out(base + 0x05, 64);
	out(base + 0x06, 0);
	out(base, 0x26);			// transmit
	Check(network.sent.size() == 64
		&& memcmp(network.sent.data(), frame, 64) == 0, "transmit");
	Check((in(base + 7) & 0x02) != 0 && irq, "transmit interrupt");
	out(base + 0x07, 0xff);
	Check(!irq, "interrupt cleared");

	// a broadcast from the network lands in the ring at CURR
	u8 broadcast[42];
	memset(broadcast, 0xff, 6);
	for (int i = 6; i < 42; i++)
		broadcast[i] = (u8)i;
	Check(network.Deliver(broadcast, sizeof(broadcast)), "receive");
	Check((in(base + 7) & 0x01) != 0 && irq, "receive interrupt");
	out(base + 0x0a, 4 + 60);
	out(base + 0x0b, 0);
	out(base + 0x08, 0x00);
	out(base + 0x09, 0x47);
	out(base, 0x0a);
	u8 received[64];
	for (int i = 0; i < 64; i += 2) {
		u16 word = (u16)card.IoRead(base + 0x10, 2);
		received[i] = (u8)word;
		received[i + 1] = (u8)(word >> 8);
	}
	int length = received[2] | (received[3] << 8);
	Check((received[0] & 1) != 0 && received[1] == 0x48 && length == 64
		&& memcmp(received + 4, broadcast, sizeof(broadcast)) == 0
		&& received[4 + 42] == 0, "receive header and padding");
	out(base, 0x61);
	Check(in(base + 0x07) == 0x48, "CURR advanced");
	out(base, 0x22);

	// a unicast for another station is ignored
	u8 other[60] = { 0x00, 0x00, 0xe8, 0x09, 0x09, 0x09 };
	network.Deliver(other, sizeof(other));
	out(base, 0x61);
	Check(in(base + 0x07) == 0x48, "address filter");
	out(base, 0x22);
}

// One command (CDB bytes) on a SCSI device; returns the status.
u8
RunScsi(ScsiDevice& device, std::initializer_list<u8> cdb,
	std::vector<u8>* dataIn = nullptr)
{
	ScsiCommand command;
	memset(command.cdb, 0, sizeof(command.cdb));
	for (u8 byte : cdb)
		command.cdb[command.cdbLength++] = byte;
	device.Execute(command);
	if (dataIn != nullptr)
		*dataIn = command.dataIn;
	return command.status;
}


// The sense key and code REQUEST SENSE reports, as 0xKKAA.
u32
ScsiSense(ScsiDevice& device)
{
	std::vector<u8> sense;
	if (RunScsi(device, { 0x03, 0, 0, 0, 18, 0 }, &sense) != kScsiStatusGood
		|| sense.size() < 13)
		return 0xffff;
	return (u32)(sense[2] & 0x0f) << 8 | sense[12];
}


// A disc changed while the machine runs: unit attention "medium may have
// changed" once, the guest's eject reported to the front end, and the
// host's eject even of a disc the guest has locked in.
void
TestCdChange()
{
	char path[] = "/tmp/bebox-cd-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) {
		Check(false, "CD: temporary image");
		return;
	}
	std::vector<u8> sectors(16 * 2048, 0);
	memcpy(&sectors[2048], "DISC", 4);
	bool written = write(fd, sectors.data(), sectors.size())
		== (ssize_t)sectors.size();
	close(fd);
	std::string isoPath = std::string(path) + ".iso";
	rename(path, isoPath.c_str());
	Check(written, "CD: temporary image");
	std::string error;

	Scheduler scheduler;
	const u64 ticksPerSecond = 1000;
	ScsiCd cd(scheduler, ticksPerSecond, "TOSHIBA", "CD-ROM XM-3401TA");
	std::string ejected;
	cd.SetEjectCallback([&ejected](const std::string& image) {
		ejected = image;
	});
	cd.Reset();
	static const std::initializer_list<u8> kTestUnitReady
		= { 0x00, 0, 0, 0, 0, 0 };
	Check(RunScsi(cd, kTestUnitReady) == kScsiStatusCheckCondition
		&& ScsiSense(cd) == 0x23a, "CD: empty drive is not ready");

	cd.Insert(CdImage::Open(isoPath, error));
	Check(RunScsi(cd, kTestUnitReady) == kScsiStatusCheckCondition
		&& ScsiSense(cd) == 0x628, "CD: insert reports a medium change");
	Check(RunScsi(cd, kTestUnitReady) == kScsiStatusGood,
		"CD: then ready");
	std::vector<u8> data;
	Check(RunScsi(cd, { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 }, &data)
			== kScsiStatusGood && data.size() == 2048
			&& memcmp(data.data(), "DISC", 4) == 0,
		"CD: reads the new disc");
	Check(cd.MediumPath() == isoPath, "CD: medium path");

	// locked by the guest: its eject is refused, the host's is not
	RunScsi(cd, { 0x1e, 0, 0, 0, 1, 0 });
	Check(RunScsi(cd, { 0x1b, 0, 0, 0, 2, 0 }) == kScsiStatusCheckCondition
		&& cd.HasMedium() && ejected.empty(),
		"CD: guest eject refused while locked");
	cd.Eject();
	Check(RunScsi(cd, kTestUnitReady) == kScsiStatusCheckCondition
		&& ScsiSense(cd) == 0x628
		&& RunScsi(cd, kTestUnitReady) == kScsiStatusCheckCondition
		&& ScsiSense(cd) == 0x23a, "CD: host eject, then not ready");

	// the guest's eject
	cd.Insert(CdImage::Open(isoPath, error));
	RunScsi(cd, kTestUnitReady);
	RunScsi(cd, { 0x1e, 0, 0, 0, 0, 0 });
	Check(RunScsi(cd, { 0x1b, 0, 0, 0, 2, 0 }) == kScsiStatusGood
		&& !cd.HasMedium() && ejected == isoPath,
		"CD: guest eject reported");

	// a disc in the drive at power on raises no unit attention
	cd.Insert(CdImage::Open(isoPath, error));
	cd.Reset();
	Check(RunScsi(cd, kTestUnitReady) == kScsiStatusGood,
		"CD: no unit attention after reset");

	// a change: the old disc comes out at once, the new one goes in
	// kChangeSeconds later
	cd.Change(CdImage::Open(isoPath, error));
	Check(RunScsi(cd, kTestUnitReady) == kScsiStatusCheckCondition
		&& ScsiSense(cd) == 0x628
		&& RunScsi(cd, kTestUnitReady) == kScsiStatusCheckCondition
		&& ScsiSense(cd) == 0x23a, "CD: change takes the old disc out");
	u64 now = (ScsiCd::kChangeSeconds - 1) * ticksPerSecond;
	scheduler.SetNow(now);
	scheduler.RunDue(now);
	Check(!cd.HasMedium(), "CD: drive empty during the change");
	now = ScsiCd::kChangeSeconds * ticksPerSecond;
	scheduler.SetNow(now);
	scheduler.RunDue(now);
	Check(cd.HasMedium()
		&& RunScsi(cd, kTestUnitReady) == kScsiStatusCheckCondition
		&& ScsiSense(cd) == 0x628
		&& RunScsi(cd, kTestUnitReady) == kScsiStatusGood,
		"CD: then the new disc is in");
	// ejected during a change: stays empty
	cd.Change(CdImage::Open(isoPath, error));
	cd.Change(nullptr);
	now += (ScsiCd::kChangeSeconds + 1) * ticksPerSecond;
	scheduler.SetNow(now);
	scheduler.RunDue(now);
	Check(!cd.HasMedium(), "CD: eject cancels a change");
	// into an empty drive: at once
	cd.Change(CdImage::Open(isoPath, error));
	Check(cd.HasMedium(), "CD: change into an empty drive");
	unlink(isoPath.c_str());
}


// F1 held for the boot ROM's debug output (BeBoxMachine::HoldDebugOutputKey)
// goes down once the ROM turns scan code translation off and up as soon as
// serial 4 sends a byte: the UART reports the bytes it sends (not those
// looped back), the keyboard controller presses a key only untranslated and
// releases it before its time.
void
TestDebugKeyRelease()
{
	Scheduler scheduler;
	const u64 ticksPerSecond = 1000000;		// 1 tick = 1 us
	u64 now = 0;
	auto run = [&](u64 ticks) {
		now += ticks;
		scheduler.SetNow(now);
		scheduler.RunDue(now);
	};

	Uart16550 uart("serial4", 0x388, 8000000, scheduler, ticksPerSecond,
		false);
	std::vector<u8> sent;
	uart.SetTransmitCallback([&sent](u8 byte) { sent.push_back(byte); });
	uart.IoWrite8(0x38c, 0x10);		// MCR: loopback
	uart.IoWrite8(0x388, 'x');
	run(1000);
	Check(sent.empty() && uart.IoRead8(0x388) == 'x',
		"debug key: looped back bytes are not sent");
	uart.IoWrite8(0x38c, 0x00);
	uart.IoWrite8(0x388, 'B');
	Check(sent.empty(), "debug key: byte reported once sent");
	run(1000);		// divisor 12 at 8 MHz: 0.24 ms per character
	Check(sent.size() == 1 && sent[0] == 'B', "debug key: sent byte reported");

	Kbc8042 kbc(scheduler, ticksPerSecond);
	auto read = [&]() {
		std::vector<u8> bytes;
		for (int i = 0; i < 100; i++) {
			run(200);
			if ((kbc.IoRead8(0x64) & 0x01) != 0)
				bytes.push_back(kbc.IoRead8(0x60));
		}
		return bytes;
	};
	kbc.HoldKey(0x05, 60 * ticksPerSecond);	// F1 (set 1: 3b)
	std::vector<u8> bytes = read();
	Check(bytes.size() == 1 && bytes[0] == 0x3b, "debug key: F1 down");
	kbc.ReleaseHeldKey();
	bytes = read();
	Check(bytes.size() == 1 && bytes[0] == 0xbb,
		"debug key: F1 up before its time");
	run(61 * ticksPerSecond);
	bytes = read();
	Check(bytes.empty(), "debug key: no repeat or second release");

	// the boot ROM's keyboard setup: controller self-test, keyboard
	// interface on, translation off (command byte 05)
	kbc.Reset();
	kbc.HoldKeyUntranslated(0x05, 60 * ticksPerSecond);
	run(2 * ticksPerSecond);
	kbc.IoWrite8(0x64, 0xaa);
	bytes = read();
	kbc.IoWrite8(0x64, 0xae);
	run(1000);
	Check(bytes.size() == 1 && bytes[0] == 0x55 && read().empty()
			&& !kbc.HoldingKey(),
		"debug key: F1 up while translating");
	kbc.IoWrite8(0x64, 0x60);
	run(100);
	kbc.IoWrite8(0x60, 0x05);
	bytes = read();
	Check(bytes.size() == 1 && bytes[0] == 0x05 && kbc.HoldingKey(),
		"debug key: F1 down once untranslated");
	run(ticksPerSecond);
	bytes = read();
	Check(bytes.size() > 5 && bytes.back() == 0x05,
		"debug key: F1 repeats");
	kbc.ReleaseHeldKey();
	bytes = read();
	Check(bytes.size() == 2 && bytes[0] == 0xf0 && bytes[1] == 0x05,
		"debug key: then up");
}


// The 1.1d7 nub's bank sizing (0xfff00570): the bank gets a 128 MB window
// at 'base'; the row bit count (MCCR1) is the largest whose top row bit
// does not alias the window's start, then the size halves while the top
// column bit aliases (three times at most).
u32
NubBankSize(PhysBus& bus, Mpc105& bridge, int bank, u32 base)
{
	u32 end = base + 0x08000000 - 1;
	bridge.ConfigWrite(0x80 + bank, (base >> 20) & 0xff, 1);
	bridge.ConfigWrite(0x88 + bank, base >> 28, 1);
	bridge.ConfigWrite(0x90 + bank, (end >> 20) & 0xff, 1);
	bridge.ConfigWrite(0x98 + bank, end >> 28, 1);
	bridge.ConfigWrite(0xa0, 1 << bank, 1);
	auto aliases = [&](u32 offset) {	// 0xfff00708
		bus.Write32(base + offset, 0xb6db6db6, 0);
		bus.Write32(base, 0x6db6db6d, 0);
		return bus.Read32(base + offset, 0) != 0xb6db6db6;
	};
	u32 size = 0x08000000;
	int rows = 3;
	for (; rows >= 0; rows--, size >>= 1) {
		u32 mccr1 = bridge.ConfigRead(0xf0, 4, 0);
		mccr1 = (mccr1 & ~(3u << (2 * bank))) | (u32)rows << (2 * bank);
		bridge.ConfigWrite(0xf0, mccr1, 4);
		if (!aliases(size >> 5))
			break;
	}
	if (rows < 0)
		return 0;
	for (int i = 0; i < 3 && aliases(size >> 1); i++)
		size >>= 1;
	return size;
}


// The MPC105 multiplexes the address onto the DRAMs' rows and columns by
// MCCR1's row bit count for the bank: the nub finds every bank size from
// 2 to 128 MB that way (8 MB: 10 x 10 bits), in any probe window, and the
// bank is then linear memory.
void
TestMemoryBanks()
{
	char what[80];
	for (u32 megabytes = 2; megabytes <= 128; megabytes *= 2) {
		for (u32 base = 0; base <= 0x08000000; base += 0x08000000) {
			PhysBus bus;
			IoSpace io;
			PciBus pci;
			Mpc105 bridge(bus, io, pci);
			u32 bytes = megabytes << 20;
			std::vector<u8> memory(bytes);
			bridge.SetBank(2, memory.data(), bytes);
			u32 found = NubBankSize(bus, bridge, 2, base);
			snprintf(what, sizeof(what), "memory: nub sizes a %u MB bank "
				"at %u MB", megabytes, base >> 20);
			Check(found == bytes, what);
			if (found != bytes || base != 0)
				continue;
			// moved to the bottom (the 1.1d7 nub moves the banks down
			// after the ones found before)
			bridge.ConfigWrite(0x80 + 2, 0, 1);
			bridge.ConfigWrite(0x88 + 2, 0, 1);
			bridge.ConfigWrite(0x90 + 2, (bytes - 1) >> 20, 1);
			bridge.ConfigWrite(0x98 + 2, (bytes - 1) >> 28, 1);
			bool linear = true;
			for (u32 address = 0; address < bytes; address += 0x800)
				bus.Write32(address, address, 0);
			for (u32 address = 0; address < bytes; address += 0x800) {
				if (bus.Read32(address, 0) != address
					|| LoadBE32(&memory[address]) != address)
					linear = false;
			}
			snprintf(what, sizeof(what), "memory: %u MB bank linear",
				megabytes);
			Check(linear, what);
		}
	}
}


// A disk image whose writes fail while 'failing' is set.
class FailingImage : public DiskImage {
public:
	virtual u64 Size() const { return 1 << 20; }
	virtual bool IsReadOnly() const { return false; }
	virtual bool Read(u64 offset, void* buffer, size_t length)
		{ memset(buffer, 0, length); return true; }
	virtual bool Write(u64 offset, const void* buffer, size_t length)
		{ return !failing; }
	virtual std::string Path() const { return "/failing.img"; }
	virtual const char* Format() const { return "raw"; }

	bool failing = true;
};


std::vector<std::string> sStorageErrors;

void
StorageErrorSink(LogCategory category, LogLevel level, const char* message,
	void*)
{
	if (category == kLogStorage && level == kLogError)
		sStorageErrors.push_back(message);
}


// A SCSI disk image that cannot be written: BeOS gets a medium error, the
// log (shown by the app) one error until a write works again.
void
TestDiskWriteErrors()
{
	FailingImage* image = new FailingImage;
	ScsiDisk disk(std::unique_ptr<DiskImage>(image), "BeBox", "Disk");
	disk.Reset();
	RunScsi(disk, { 0x00, 0, 0, 0, 0, 0 });		// the unit attention
	ScsiSense(disk);
	auto write = [&disk]() {
		ScsiCommand command;
		memset(command.cdb, 0, sizeof(command.cdb));
		u8 cdb[10] = { 0x2a, 0, 0, 0, 0, 8, 0, 0, 1, 0 };	// WRITE(10)
		memcpy(command.cdb, cdb, sizeof(cdb));
		command.cdbLength = 10;
		disk.Execute(command);
		if (command.status != kScsiStatusGood
			|| command.dataOutLength != 512)
			return (u8)0xff;
		disk.DataOut(command, std::vector<u8>(512, 0x55));
		return command.status;
	};
	sStorageErrors.clear();
	Log::SetSink(StorageErrorSink, nullptr);
	bool failed = write() == kScsiStatusCheckCondition
		&& ScsiSense(disk) == 0x30c;
	write();
	Check(failed && sStorageErrors.size() == 1
		&& sStorageErrors[0].find("/failing.img") != std::string::npos,
		"disk write errors: medium error, logged once");
	image->failing = false;
	bool works = write() == kScsiStatusGood;
	image->failing = true;
	write();
	Check(works && sStorageErrors.size() == 2,
		"disk write errors: logged again after a write worked");
	Log::SetSink(nullptr, nullptr);
}


// "CATEGORY=LEVEL" settings (the app's and bebox-headless' --log).
void
TestLogConfigure()
{
	Check(Log::Configure("scsi=info") && Log::Level(kLogScsi) == kLogInfo
		&& Log::Level(kLogIde) == kLogWarning, "log: a category's level");
	Check(Log::Configure("ide") && Log::Level(kLogIde) == kLogDebug,
		"log: debug without a level");
	Check(!Log::Configure("nonsense=debug"), "log: unknown category");
	Check(Log::Configure("all=warning") && Log::Level(kLogScsi) == kLogWarning
		&& Log::Level(kLogIde) == kLogWarning, "log: all categories");
}


// A timer destroyed while armed takes its queue entries with it (the
// scheduler keeps pointers; AddressSanitizer would see a stale one read).
void
TestTimerDestroyed()
{
	Scheduler scheduler;
	int fired = 0;
	int stayed = 0;
	Timer staying(scheduler, [&stayed]() { stayed++; }, "staying");
	staying.Arm(300);
	{
		std::unique_ptr<Timer> going(new Timer(scheduler,
			[&fired]() { fired++; }, "going"));
		going->Arm(100);
		going->Arm(200);	// a stale entry too
	}
	Check(scheduler.NextEventTime() == 300,
		"timers: a destroyed timer is not due");
	scheduler.RunDue(1000);
	Check(fired == 0 && stayed == 1,
		"timers: the others still fire, the destroyed one does not");
}


// Threaded mode (ThreadGovernor): one host thread once the two wait for
// each other longer than two are faster (the speedup a trial measured on
// a calm host), probes of two threads backing off while they still wait.
void
TestThreadGovernor()
{
	ThreadGovernor governor;
	double now = 100;
	// 1 ms stretches with both CPUs running on two threads that waited
	// this share of the time
	auto both = [&](double seconds, double share) {
		for (int i = 0; i <= (int)(seconds * 1000 + 0.5); i++) {
			governor.Waited(0.001 * share);
			now += 0.001;
			governor.BothRan(now, 0.001);
			governor.RunEnded(now);
		}
	};
	// a trial: 0.1 ms slices at these instructions per second
	auto trial = [&](double oneRate, double twoRate) {
		for (int i = 0; i < 2000 && governor.Timing(); i++) {
			double rate = governor.TwoThreads() ? twoRate : oneRate;
			governor.SliceTimed((u64)(rate / 10000), now, now + 0.0001);
			now += 0.0001;
		}
	};
	auto runEnded = [&](double seconds) {
		now += seconds;
		governor.RunEnded(now);
	};

	Check(governor.TwoThreads() && !governor.Timing(),
		"threads: two at first");
	both(0.2, 0.01);
	Check(governor.Timing(), "threads: a trial while the threads hardly "
		"wait");
	trial(100e6, 150e6);
	Check(governor.TwoThreads() && !governor.Timing()
		&& std::fabs(governor.Speedup() - 1.5) < 0.01,
		"threads: the trial measures the speedup (1.5), two threads");
	// break-even: waiting a third of the time
	both(0.2, 0.25);
	Check(governor.TwoThreads(), "threads: two while waiting 25%");
	both(0.2, 0.5);
	Check(!governor.TwoThreads(), "threads: one when waiting 50%");
	runEnded(0.5);
	Check(!governor.TwoThreads(), "threads: no probe after 0.5 s");
	runEnded(0.6);
	Check(governor.TwoThreads(), "threads: a probe of two after 1 s");
	both(0.06, 0.5);
	Check(!governor.TwoThreads(), "threads: one after a probe waiting "
		"50%");
	runEnded(1.1);
	Check(!governor.TwoThreads(), "threads: no probe 1 s later");
	runEnded(1);
	Check(governor.TwoThreads(), "threads: the next probe after 2 s");
	both(0.06, 0.1);
	Check(governor.TwoThreads() && !governor.Timing(),
		"threads: two after a probe waiting 10%");
	both(0.3, 0.1);
	Check(governor.TwoThreads(), "threads: two keep waiting 10%");
}

}	// namespace


int
main()
{
	TestGamePorts();
	TestBootItems();
	TestRtc();
	TestNe2000();
	TestS3Dac();
	TestS3Engine();
	TestS3Cursor();
	TestCirrusCursor();
	TestCdChange();
	TestDebugKeyRelease();
	TestMemoryBanks();
	TestThreadGovernor();
	TestTimerDestroyed();
	TestDiskWriteErrors();
	TestLogConfigure();
	printf("%d failures\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
