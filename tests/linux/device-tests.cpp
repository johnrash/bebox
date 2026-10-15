/*
 * BeBox emulator - device tests: the game port A/D converter, the S3
 * Vision864's RAMDAC access, graphics engine and cursor, the Cirrus
 * GD5434's cursor and the NE2000.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "core/board/BootItems.h"
#include "core/board/Scheduler.h"
#include "core/bus/IoSpace.h"
#include "core/bus/PhysBus.h"
#include "core/host/NetworkBackend.h"
#include "core/host/VideoSink.h"
#include "core/isa/JoystickAdc.h"
#include "core/isa/Ne2000.h"
#include "core/isa/Rtc146818.h"
#include "core/video/CirrusGd5434.h"
#include "core/video/S3Vision864.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

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
	printf("%d failures\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
