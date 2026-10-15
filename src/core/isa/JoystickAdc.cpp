/*
 * BeBox emulator - game port A/D converter and buttons.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "JoystickAdc.h"

#include "../Log.h"

namespace bebox {

enum {
	kControlUnipolar = 0x08,
	kStatusDone = 0x08		// in the high byte (0x361)
};

// MAX186 single-ended channel for the SEL2-SEL0 bits of the control byte.
static const int kChannelForSelect[8] = { 0, 2, 4, 6, 1, 3, 5, 7 };


JoystickAdc::JoystickAdc(Scheduler& scheduler, u64 ticksPerSecond,
	int ioCardRevision)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fButtons(0xff)
{
	fTimer.reset(new Timer(scheduler, [this]() { _ConversionDone(); },
		"a/d"));
	// Joystick axes rest at mid scale.
	for (int i = 0; i < kChannels; i++)
		fInputs[i] = 0x800;
	fInputs[6] = ioCardRevision >= 5 ? 0xfff : 0;
	for (int i = 0; i < 4; i++)
		fAxes[i][0] = fAxes[i][1] = 0x800;
	Reset();
}


void
JoystickAdc::Reset()
{
	fControl = 0;
	fMux = 0;
	fDone = false;
	fResult = 0;
	fTimer->Cancel();
}


void
JoystickAdc::SetChannel(int channel, u16 value)
{
	if (channel >= 0 && channel < kChannels)
		fInputs[channel] = value & 0xfff;
}


void
JoystickAdc::SetStick(int stick, u16 x, u16 y, bool button1, bool button2)
{
	static const u8 kButtons[4][2] = { { 0x01, 0x08 }, { 0x02, 0x04 },
		{ 0x10, 0x80 }, { 0x20, 0x40 } };
	if (stick < 0 || stick > 3)
		return;
	fAxes[stick][0] = x & 0xfff;
	fAxes[stick][1] = y & 0xfff;
	for (int i = 0; i < 2; i++) {
		bool pressed = i == 0 ? button1 : button2;
		if (pressed)
			fButtons &= ~kButtons[stick][i];
		else
			fButtons |= kButtons[stick][i];
	}
}


u8
JoystickAdc::IoRead8(u32 port)
{
	switch (port) {
		case 0x360:
			return fControl;
		case 0x361:
			return fDone ? kStatusDone : 0;
		case 0x362:
			return (u8)fResult;
		case 0x363:
			return (u8)(fResult >> 8);
		case 0x36e:
			return (u8)fMux;
		case 0x36f:
			return (u8)(fMux >> 8);
		case 0x397:
			return fButtons;
	}
	return 0xff;
}


void
JoystickAdc::IoWrite8(u32 port, u8 value)
{
	if (port == 0x36e || port == 0x36f) {
		int shift = (port & 1) * 8;
		fMux = (u16)((fMux & ~(0xff << shift)) | (value << shift));
		return;
	}
	if (port != 0x360)
		return;
	// A control byte starts a conversion (about 10 us plus the serial
	// transfer of the result).
	fControl = value;
	fDone = false;
	fTimer->ArmIn(fTicksPerSecond / 66666);		// 15 us
}


void
JoystickAdc::_ConversionDone()
{
	int channel = kChannelForSelect[(fControl >> 4) & 7];
	u16 value = fInputs[channel];
	if (channel == 4 || channel == 5) {
		// game port 1 or 2: the multiplexer picks a stick's axis
		int select = (fMux >> 8) & 3;
		value = fAxes[(channel - 4) * 2 + select / 2][select & 1];
	}
	if ((fControl & kControlUnipolar) == 0)
		value = (value - 0x800) & 0xfff;	// bipolar: two's complement
	fResult = value;
	fDone = true;
	BB_LOG(kLogJoystick, kLogDebug, "a/d channel %d: %03x", channel, value);
}

}	// namespace bebox
