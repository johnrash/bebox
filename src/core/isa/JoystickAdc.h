/*
 * BeBox emulator - game port A/D converter and buttons.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * A Maxim MAX186 (8 channel, 12 bit) behind the I/O card's glue logic.
 * The kernel writes a MAX186 control byte to 0x360, waits for bit 0x800
 * of the 16-bit status at 0x360 and reads the result from 0x362. Channel 6
 * identifies the I/O card revision (above 256: revision 5, else 4).
 *
 * The two game ports (two sticks each with Y cables) are channels 4 and 5;
 * an analog multiplexer selected by bits 8-9 of the 16-bit register at
 * 0x36e picks stick 1 X, stick 1 Y, stick 2 X or stick 2 Y of the port
 * (BeOS's a2d driver). The buttons are at 0x397, active low: sticks 1-4
 * have buttons 01h/08h, 02h/04h, 10h/80h and 20h/40h.
 */
#ifndef BEBOX_JOYSTICK_ADC_H
#define BEBOX_JOYSTICK_ADC_H

#include "../bus/IoSpace.h"
#include "../board/Scheduler.h"

#include <memory>

namespace bebox {

class JoystickAdc : public ByteIoDevice {
public:
	static const int kChannels = 8;

								JoystickAdc(Scheduler& scheduler,
									u64 ticksPerSecond, int ioCardRevision);

			void				Reset();

			// Host input: 12-bit channel values and the button port value
			// (active low, all released = 0xff).
			void				SetChannel(int channel, u16 value);
			void				SetButtons(u8 value) { fButtons = value; }
			// Stick 0-3: axes 0-4095 (2048 = centered), button states.
			void				SetStick(int stick, u16 x, u16 y,
									bool button1, bool button2);

	virtual	u8					IoRead8(u32 port);
	virtual	void				IoWrite8(u32 port, u8 value);
	virtual	const char*			IoName() const { return "a/d"; }

private:
			void				_ConversionDone();

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			std::unique_ptr<Timer> fTimer;
			u16					fInputs[kChannels];
			u8					fControl;
			bool				fDone;
			u16					fResult;
			u8					fButtons;
			u16					fMux;				// 0x36e
			u16					fAxes[4][2];		// [stick][x, y]
};

}	// namespace bebox

#endif	// BEBOX_JOYSTICK_ADC_H
