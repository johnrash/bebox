/*
 * BeBox emulator - 8042 keyboard controller with an AT keyboard (scan code
 * set 2, optionally translated to set 1) and a PS/2 mouse.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Ports 0x60 (data) and 0x64 (status/command); keyboard on IRQ1, mouse on
 * IRQ12 (Be interrupt controller bits 15 and 23).
 */
#ifndef BEBOX_KBC_8042_H
#define BEBOX_KBC_8042_H

#include "../bus/IoSpace.h"
#include "../board/Scheduler.h"

#include <deque>
#include <utility>
#include <vector>
#include <functional>
#include <memory>

namespace bebox {

class Kbc8042 : public ByteIoDevice {
public:
	typedef std::function<void(int irq, bool level)> IrqCallback;
	typedef std::function<void()> ResetCallback;

								Kbc8042(Scheduler& scheduler,
									u64 ticksPerSecond);

			void				Reset();
			void				SetIrqCallback(IrqCallback callback)
									{ fIrq = callback; }
			void				SetResetCallback(ResetCallback callback)
									{ fReset = callback; }

			// Host input. Keys are scan code set 2 make codes; codes with
			// 0xe000 set are extended (E0 prefix). Pause is 0xe11477.
			void				KeyDown(u32 code);
			void				KeyUp(u32 code);
			// Presses a key and releases it after 'ticks' of emulated
			// time (boot ROM key checks), repeating meanwhile.
			void				HoldKey(u32 code, u64 ticks);
			// Key events (code, down) one after another, 'interval' ticks
			// apart (typing text for scripts).
			void				TypeKeys(
									const std::vector<std::pair<u32, bool> >&
										events, u64 interval);
			// Relative motion (positive y = up) and buttons (bit 0 left,
			// 1 right, 2 middle).
			void				MouseMoved(int dx, int dy, int dz,
									u8 buttons);
			u8					KeyboardLeds() const { return fLeds; }

	virtual	u8					IoRead8(u32 port);
	virtual	void				IoWrite8(u32 port, u8 value);
	virtual	const char*			IoName() const { return "kbc"; }

private:
	struct Byte {
		u8 value;
		bool aux;
		bool device;	// from the keyboard or mouse (interrupts)
	};

			void				_Queue(u8 value, bool aux, bool device = true);
			void				_QueueKeyboard(u8 value);
			void				_Deliver();
			void				_ProcessInput();
			void				_UpdateIrq();
			void				_KeyboardCommand(u8 value);
			void				_MouseCommand(u8 value);
			void				_ControllerCommand(u8 value);
			void				_SendScanCode(u32 code, bool release);
			void				_Repeat();
			void				_SetTypematic(u8 value);

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			IrqCallback			fIrq;
			ResetCallback		fReset;
			std::unique_ptr<Timer> fDeliveryTimer;
			std::unique_ptr<Timer> fInputTimer;
			bool				fInputPending;
			u32					fInputPort;
			u8					fInputValue;

			// controller
			u8					fCommandByte;
			u8					fStatus;
			u8					fOutput;		// output buffer
			bool				fOutputAux;
			bool				fOutputDevice;
			u8					fOutputPort;
			u8					fPendingCommand;	// awaiting a data byte
			std::deque<Byte>	fQueue;
			bool				fIrq1;
			bool				fIrq12;

			// keyboard
			bool				fKeyboardEnabled;
			u8					fKeyboardPendingCommand;
			u8					fLeds;
			u8					fScanSet;
			std::unique_ptr<Timer> fRepeatTimer;
			std::unique_ptr<Timer> fHoldTimer;
			u32					fHeldCode;
			std::unique_ptr<Timer> fTypeTimer;
			std::deque<std::pair<u32, bool> > fTypeQueue;
			u64					fTypeInterval;
			u32					fRepeatCode;	// key repeating, or 0
			u32					fRepeatDelayMs;
			u32					fRepeatPeriodUs;
			bool				fBreakPrefix;	// set 1 translation state

			// mouse
			bool				fMouseEnabled;
			bool				fMouseWrap;
			u8					fMousePendingCommand;
			u8					fMouseSampleRate;
			u8					fMouseResolution;
			bool				fMouseScaling21;
			u8					fMouseId;		// 0 or 3 (IntelliMouse)
			u8					fRateHistory[3];
			u8					fMouseButtons;
};

}	// namespace bebox

#endif	// BEBOX_KBC_8042_H
