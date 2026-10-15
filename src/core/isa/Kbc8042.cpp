/*
 * BeBox emulator - 8042 keyboard controller, AT keyboard and PS/2 mouse.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Kbc8042.h"

#include "../Log.h"

namespace bebox {

enum {
	kStatusOutputFull = 0x01,
	kStatusInputFull = 0x02,
	kStatusSystem = 0x04,
	kStatusCommand = 0x08,
	kStatusUnlocked = 0x10,
	kStatusAuxData = 0x20,

	kCommandKeyboardIrq = 0x01,
	kCommandAuxIrq = 0x02,
	kCommandSystem = 0x04,
	kCommandKeyboardDisabled = 0x10,
	kCommandAuxDisabled = 0x20,
	kCommandTranslate = 0x40
};

// Scan code set 2 to set 1 translation of the 8042.
static const u8 kTranslate[256] = {
	0xff, 0x43, 0x41, 0x3f, 0x3d, 0x3b, 0x3c, 0x58, 0x64, 0x44, 0x42, 0x40,
	0x3e, 0x0f, 0x29, 0x59, 0x65, 0x38, 0x2a, 0x70, 0x1d, 0x10, 0x02, 0x5a,
	0x66, 0x71, 0x2c, 0x1f, 0x1e, 0x11, 0x03, 0x5b, 0x67, 0x2e, 0x2d, 0x20,
	0x12, 0x05, 0x04, 0x5c, 0x68, 0x39, 0x2f, 0x21, 0x14, 0x13, 0x06, 0x5d,
	0x69, 0x31, 0x30, 0x23, 0x22, 0x15, 0x07, 0x5e, 0x6a, 0x72, 0x32, 0x24,
	0x16, 0x08, 0x09, 0x5f, 0x6b, 0x33, 0x25, 0x17, 0x18, 0x0b, 0x0a, 0x60,
	0x6c, 0x34, 0x35, 0x26, 0x27, 0x19, 0x0c, 0x61, 0x6d, 0x73, 0x28, 0x74,
	0x1a, 0x0d, 0x62, 0x6e, 0x3a, 0x36, 0x1c, 0x1b, 0x75, 0x2b, 0x63, 0x76,
	0x55, 0x56, 0x77, 0x78, 0x79, 0x7a, 0x0e, 0x7b, 0x7c, 0x4f, 0x7d, 0x4b,
	0x47, 0x7e, 0x7f, 0x6f, 0x52, 0x53, 0x50, 0x4c, 0x4d, 0x48, 0x01, 0x45,
	0x57, 0x4e, 0x51, 0x4a, 0x37, 0x49, 0x46, 0x54, 0x80, 0x81, 0x82, 0x41,
	0x54, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
	0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b,
	0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3,
	0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
	0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb,
	0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
	0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3,
	0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
	0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb,
	0xfc, 0xfd, 0xfe, 0xff
};


Kbc8042::Kbc8042(Scheduler& scheduler, u64 ticksPerSecond)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fIrq1(false),
	fIrq12(false)
{
	fDeliveryTimer.reset(new Timer(scheduler, [this]() { _Deliver(); },
		"kbc"));
	fInputTimer.reset(new Timer(scheduler, [this]() { _ProcessInput(); },
		"kbc input"));
	fRepeatTimer.reset(new Timer(scheduler, [this]() { _Repeat(); },
		"typematic"));
	fHoldTimer.reset(new Timer(scheduler, [this]() {
		if (fHeldCode != 0)
			KeyUp(fHeldCode);
		fHeldCode = 0;
	}, "held key"));
	fHeldCode = 0;
	fTypeTimer.reset(new Timer(scheduler, [this]() {
		if (fTypeQueue.empty())
			return;
		std::pair<u32, bool> event = fTypeQueue.front();
		fTypeQueue.pop_front();
		if (event.second)
			KeyDown(event.first);
		else
			KeyUp(event.first);
		if (!fTypeQueue.empty())
			fTypeTimer->ArmIn(fTypeInterval);
	}, "typed keys"));
	fTypeInterval = 1;
	Reset();
}


void
Kbc8042::Reset()
{
	fCommandByte = kCommandKeyboardIrq | kCommandSystem | kCommandTranslate;
	fStatus = kStatusUnlocked;
	fOutput = 0;
	fOutputAux = false;
	fOutputDevice = false;
	fOutputPort = 0x01;
	fPendingCommand = 0;
	fQueue.clear();
	fKeyboardEnabled = true;
	fKeyboardPendingCommand = 0;
	fLeds = 0;
	fScanSet = 2;
	fBreakPrefix = false;
	fMouseEnabled = false;
	fMouseWrap = false;
	fMousePendingCommand = 0;
	fMouseSampleRate = 100;
	fMouseResolution = 2;
	fMouseScaling21 = false;
	fMouseId = 0;
	memset(fRateHistory, 0, sizeof(fRateHistory));
	fMouseButtons = 0;
	fInputPending = false;
	fDeliveryTimer->Cancel();
	fInputTimer->Cancel();
	_SetTypematic(0x2b);
	fRepeatCode = 0;
	fRepeatTimer->Cancel();
	fHeldCode = 0;
	fHoldTimer->Cancel();
	fTypeQueue.clear();
	fTypeTimer->Cancel();
	_UpdateIrq();
}


void
Kbc8042::_UpdateIrq()
{
	// The controller firmware interrupts for data from the devices, not
	// for its own replies (the boot ROMs poll for those, with interrupts
	// enabled).
	bool full = (fStatus & kStatusOutputFull) != 0 && fOutputDevice;
	bool irq1 = full && !fOutputAux
		&& (fCommandByte & kCommandKeyboardIrq) != 0;
	// On the BeBox the mouse interrupt does not depend on command byte
	// bit 1: the boot ROM clears it and BeOS's kb_mouse driver never sets
	// it, yet takes mouse data from its IRQ12 (Be bit 23) handler.
	bool irq12 = full && fOutputAux;
	if (irq1 != fIrq1) {
		fIrq1 = irq1;
		if (fIrq)
			fIrq(1, irq1);
	}
	if (irq12 != fIrq12) {
		fIrq12 = irq12;
		if (fIrq)
			fIrq(12, irq12);
	}
}


void
Kbc8042::_Queue(u8 value, bool aux, bool device)
{
	if (fQueue.size() > 256)
		return;
	fQueue.push_back(Byte{ value, aux, device });
	if ((fStatus & kStatusOutputFull) == 0 && !fDeliveryTimer->IsArmed())
		fDeliveryTimer->ArmIn(fTicksPerSecond / 10000);	// 100 us
}


// Keyboard bytes go through the set 2 to set 1 translation if enabled.
void
Kbc8042::_QueueKeyboard(u8 value)
{
	if ((fCommandByte & kCommandTranslate) != 0) {
		if (value == 0xf0) {
			fBreakPrefix = true;
			return;
		}
		u8 translated = kTranslate[value];
		if (value == 0xe0 || value == 0xe1 || value == 0xfa || value == 0xaa
			|| value == 0xee || value == 0xfe || value == 0x00
			|| value == 0xff)
			translated = value;		// prefixes and responses pass through
		if (fBreakPrefix) {
			translated |= 0x80;
			fBreakPrefix = false;
		}
		value = translated;
	}
	_Queue(value, false);
}


void
Kbc8042::_Deliver()
{
	if ((fStatus & kStatusOutputFull) != 0 || fQueue.empty())
		return;
	// Disabled devices hold their data back.
	Byte byte = fQueue.front();
	if ((byte.aux && (fCommandByte & kCommandAuxDisabled) != 0)
		|| (!byte.aux && (fCommandByte & kCommandKeyboardDisabled) != 0)) {
		bool found = false;
		for (auto it = fQueue.begin(); it != fQueue.end(); ++it) {
			bool disabled = it->aux
				? (fCommandByte & kCommandAuxDisabled) != 0
				: (fCommandByte & kCommandKeyboardDisabled) != 0;
			if (!disabled) {
				byte = *it;
				fQueue.erase(it);
				found = true;
				break;
			}
		}
		if (!found)
			return;
	} else
		fQueue.pop_front();
	fOutput = byte.value;
	fOutputAux = byte.aux;
	fOutputDevice = byte.device;
	fStatus |= kStatusOutputFull;
	if (byte.aux)
		fStatus |= kStatusAuxData;
	else
		fStatus &= ~kStatusAuxData;
	_UpdateIrq();
}


u8
Kbc8042::IoRead8(u32 port)
{
	if (port == 0x64)
		return fStatus;
	if (port != 0x60)
		return 0xff;
	u8 value = fOutput;
	BB_LOG(kLogKbd, kLogDebug, "read data %02x", value);
	fStatus &= ~(kStatusOutputFull | kStatusAuxData);
	_UpdateIrq();
	if (!fQueue.empty() && !fDeliveryTimer->IsArmed())
		fDeliveryTimer->ArmIn(fTicksPerSecond / 10000);
	return value;
}


// The controller firmware takes a while to pick up a written byte: the
// input buffer full bit stays set meanwhile, and the boot ROM waits to see
// it set before it waits for it to clear again.
void
Kbc8042::IoWrite8(u32 port, u8 value)
{
	if (port != 0x60 && port != 0x64)
		return;
	BB_LOG(kLogKbd, kLogDebug, "write %02x to %s", value,
		port == 0x64 ? "command" : "data");
	if (fInputPending)
		_ProcessInput();
	fInputPending = true;
	fInputPort = port;
	fInputValue = value;
	fStatus |= kStatusInputFull;
	if (port == 0x64)
		fStatus |= kStatusCommand;
	else
		fStatus &= ~kStatusCommand;
	fInputTimer->ArmIn(fTicksPerSecond / 20000);	// 50 us
}


void
Kbc8042::_ProcessInput()
{
	if (!fInputPending)
		return;
	fInputPending = false;
	fInputTimer->Cancel();
	fStatus &= ~kStatusInputFull;
	u8 value = fInputValue;
	if (fInputPort == 0x64) {
		_ControllerCommand(value);
		return;
	}
	u8 pending = fPendingCommand;
	fPendingCommand = 0;
	switch (pending) {
		case 0x60:
			fCommandByte = value;
			if ((value & kCommandSystem) != 0)
				fStatus |= kStatusSystem;
			else
				fStatus &= ~kStatusSystem;
			_UpdateIrq();
			if (!fQueue.empty() && !fDeliveryTimer->IsArmed())
				fDeliveryTimer->ArmIn(fTicksPerSecond / 10000);
			return;
		case 0xd1:
			fOutputPort = value;
			if ((value & 1) == 0) {
				BB_LOG(kLogKbd, kLogInfo, "reset through the output port");
				if (fReset)
					fReset();
			}
			return;
		case 0xd2:
			_Queue(value, false);
			return;
		case 0xd3:
			_Queue(value, true);
			return;
		case 0xd4:
			_MouseCommand(value);
			return;
	}
	// Data for the keyboard enables its interface.
	fCommandByte &= ~kCommandKeyboardDisabled;
	_KeyboardCommand(value);
}


void
Kbc8042::_ControllerCommand(u8 value)
{
	switch (value) {
		case 0x20:
			_Queue(fCommandByte, false, false);
			break;
		case 0x60: case 0xd1: case 0xd2: case 0xd3: case 0xd4:
			fPendingCommand = value;
			break;
		case 0xa7:
			fCommandByte |= kCommandAuxDisabled;
			break;
		case 0xa8:
			fCommandByte &= ~kCommandAuxDisabled;
			break;
		case 0xa9:		// test aux interface
			_Queue(0x00, false, false);
			break;
		case 0xaa:		// self test
			fStatus |= kStatusSystem;
			_Queue(0x55, false, false);
			break;
		case 0xab:		// keyboard interface test
			_Queue(0x00, false, false);
			break;
		case 0xad:
			fCommandByte |= kCommandKeyboardDisabled;
			break;
		case 0xae:
			fCommandByte &= ~kCommandKeyboardDisabled;
			if (!fQueue.empty() && !fDeliveryTimer->IsArmed())
				fDeliveryTimer->ArmIn(fTicksPerSecond / 10000);
			break;
		case 0xc0:		// input port: keyboard not inhibited
			_Queue(0x80, false, false);
			break;
		case 0xd0:
			_Queue(fOutputPort, false, false);
			break;
		case 0xe0:
			_Queue(0x00, false, false);
			break;
		default:
			if (value >= 0xf0) {
				// pulse output port bits; bit 0 resets the system
				if ((value & 1) == 0) {
					BB_LOG(kLogKbd, kLogInfo, "reset command");
					if (fReset)
						fReset();
				}
				break;
			}
			BB_LOG(kLogKbd, kLogInfo, "unknown controller command %02x",
				value);
			break;
	}
}


void
Kbc8042::_KeyboardCommand(u8 value)
{
	u8 pending = fKeyboardPendingCommand;
	if (pending != 0) {
		fKeyboardPendingCommand = 0;
		switch (pending) {
			case 0xed:
				fLeds = value & 7;
				_QueueKeyboard(0xfa);
				return;
			case 0xf3:
				_SetTypematic(value);
				_QueueKeyboard(0xfa);
				return;
			case 0xf0:
				_QueueKeyboard(0xfa);
				if (value == 0)
					_QueueKeyboard(fScanSet);
				else if (value <= 3)
					fScanSet = value;
				return;
		}
	}
	switch (value) {
		case 0xed: case 0xf3: case 0xf0:
			fKeyboardPendingCommand = value;
			_QueueKeyboard(0xfa);
			break;
		case 0xee:
			_QueueKeyboard(0xee);
			break;
		case 0xf2:
			_QueueKeyboard(0xfa);
			_Queue(0xab, false);
			_Queue((fCommandByte & kCommandTranslate) != 0 ? 0x41 : 0x83,
				false);
			break;
		case 0xf4:
			fKeyboardEnabled = true;
			_QueueKeyboard(0xfa);
			break;
		case 0xf5:
			fKeyboardEnabled = false;
			_SetTypematic(0x2b);
			_QueueKeyboard(0xfa);
			break;
		case 0xf6:
			fKeyboardEnabled = true;
			_SetTypematic(0x2b);
			_QueueKeyboard(0xfa);
			break;
		case 0xff:
			fKeyboardEnabled = true;
			fScanSet = 2;
			fLeds = 0;
			_SetTypematic(0x2b);
			_QueueKeyboard(0xfa);
			_Queue(0xaa, false);
			break;
		default:
			if (value >= 0xf7 && value <= 0xfd) {
				_QueueKeyboard(0xfa);
				break;
			}
			_QueueKeyboard(0xfe);	// resend
			break;
	}
}


void
Kbc8042::_MouseCommand(u8 value)
{
	if (fMouseWrap && value != 0xec && value != 0xff) {
		_Queue(value, true);
		return;
	}
	u8 pending = fMousePendingCommand;
	if (pending != 0) {
		fMousePendingCommand = 0;
		if (pending == 0xf3) {
			fMouseSampleRate = value;
			// IntelliMouse detection: rates 200, 100, 80
			fRateHistory[0] = fRateHistory[1];
			fRateHistory[1] = fRateHistory[2];
			fRateHistory[2] = value;
			if (fRateHistory[0] == 200 && fRateHistory[1] == 100
				&& fRateHistory[2] == 80)
				fMouseId = 3;
		} else if (pending == 0xe8)
			fMouseResolution = value & 3;
		_Queue(0xfa, true);
		return;
	}
	switch (value) {
		case 0xe6:
			fMouseScaling21 = false;
			_Queue(0xfa, true);
			break;
		case 0xe7:
			fMouseScaling21 = true;
			_Queue(0xfa, true);
			break;
		case 0xe8: case 0xf3:
			fMousePendingCommand = value;
			_Queue(0xfa, true);
			break;
		case 0xe9:
			_Queue(0xfa, true);
			_Queue((fMouseEnabled ? 0x20 : 0) | (fMouseScaling21 ? 0x10 : 0)
				| (fMouseButtons & 1 ? 4 : 0) | (fMouseButtons & 4 ? 2 : 0)
				| (fMouseButtons & 2 ? 1 : 0), true);
			_Queue(fMouseResolution, true);
			_Queue(fMouseSampleRate, true);
			break;
		case 0xea:
			_Queue(0xfa, true);
			break;
		case 0xec:
			fMouseWrap = false;
			_Queue(0xfa, true);
			break;
		case 0xee:
			fMouseWrap = true;
			_Queue(0xfa, true);
			break;
		case 0xf2:
			_Queue(0xfa, true);
			_Queue(fMouseId, true);
			break;
		case 0xf4:
			fMouseEnabled = true;
			_Queue(0xfa, true);
			break;
		case 0xf5:
			fMouseEnabled = false;
			_Queue(0xfa, true);
			break;
		case 0xf6:
			fMouseSampleRate = 100;
			fMouseResolution = 2;
			fMouseScaling21 = false;
			_Queue(0xfa, true);
			break;
		case 0xff:
			fMouseEnabled = false;
			fMouseWrap = false;
			fMouseId = 0;
			fMouseSampleRate = 100;
			fMouseResolution = 2;
			_Queue(0xfa, true);
			_Queue(0xaa, true);
			_Queue(0x00, true);
			break;
		default:
			_Queue(0xfe, true);
			break;
	}
}


void
Kbc8042::_SendScanCode(u32 code, bool release)
{
	if (!fKeyboardEnabled)
		return;
	if (code == 0xe11477) {
		// Pause has no break code.
		if (!release) {
			static const u8 kPause[] = { 0xe1, 0x14, 0x77, 0xe1, 0xf0, 0x14,
				0xf0, 0x77 };
			for (u8 byte : kPause)
				_QueueKeyboard(byte);
		}
		return;
	}
	if ((code & 0xe000) == 0xe000)
		_QueueKeyboard(0xe0);
	if (release)
		_QueueKeyboard(0xf0);
	_QueueKeyboard((u8)code);
}


// The keyboard repeats the last key pressed while it is held.
void
Kbc8042::KeyDown(u32 code)
{
	_SendScanCode(code, false);
	if (code == 0xe11477)
		return;
	fRepeatCode = code;
	fRepeatTimer->ArmIn(fTicksPerSecond * fRepeatDelayMs / 1000);
}


void
Kbc8042::KeyUp(u32 code)
{
	if (code == fRepeatCode) {
		fRepeatCode = 0;
		fRepeatTimer->Cancel();
	}
	_SendScanCode(code, true);
}


void
Kbc8042::TypeKeys(const std::vector<std::pair<u32, bool> >& events,
	u64 interval)
{
	bool idle = fTypeQueue.empty();
	fTypeQueue.insert(fTypeQueue.end(), events.begin(), events.end());
	fTypeInterval = interval;
	if (idle && !fTypeQueue.empty())
		fTypeTimer->ArmIn(1);
}


void
Kbc8042::HoldKey(u32 code, u64 ticks)
{
	if (fHeldCode != 0)
		KeyUp(fHeldCode);
	fHeldCode = code;
	KeyDown(code);
	fHoldTimer->ArmIn(ticks);
}


void
Kbc8042::_Repeat()
{
	if (fRepeatCode == 0)
		return;
	// A real keyboard buffers only a few codes while the host is not
	// reading; skip the repeat rather than pile them up.
	if (fQueue.size() < 8)
		_SendScanCode(fRepeatCode, false);
	fRepeatTimer->ArmIn(fTicksPerSecond * fRepeatPeriodUs / 1000000);
}


// Typematic rate/delay byte: bits 5-6 delay (250 ms steps), bits 0-4 the
// period (8 + A) * 2^B * 4.17 ms with A = bits 0-2, B = bits 3-4.
void
Kbc8042::_SetTypematic(u8 value)
{
	fRepeatDelayMs = 250 * (((value >> 5) & 3) + 1);
	fRepeatPeriodUs = (8 + (value & 7)) * (1u << ((value >> 3) & 3)) * 4170;
}


void
Kbc8042::MouseMoved(int dx, int dy, int dz, u8 buttons)
{
	fMouseButtons = buttons;
	if (!fMouseEnabled || fMouseWrap)
		return;
	// Do not let a fast mouse flood the queue.
	if (fQueue.size() > 64)
		return;
	while (true) {
		int x = dx < -255 ? -255 : dx > 255 ? 255 : dx;
		int y = dy < -255 ? -255 : dy > 255 ? 255 : dy;
		u8 status = 0x08 | (buttons & 7);
		if (x < 0)
			status |= 0x10;
		if (y < 0)
			status |= 0x20;
		_Queue(status, true);
		_Queue((u8)x, true);
		_Queue((u8)y, true);
		if (fMouseId == 3) {
			int z = dz < -8 ? -8 : dz > 7 ? 7 : dz;
			_Queue((u8)z, true);
		}
		dx -= x;
		dy -= y;
		dz = 0;
		if (dx == 0 && dy == 0)
			break;
	}
}

}	// namespace bebox
