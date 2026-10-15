/*
 * BeBox emulator - Intel 8254 programmable interval timer.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Counters are computed from the emulated time instead of being clocked;
 * timers fire only at output transitions (IRQ0 for counter 0).
 */
#ifndef BEBOX_PIT_8254_H
#define BEBOX_PIT_8254_H

#include "../bus/IoSpace.h"
#include "../board/Scheduler.h"

#include <functional>
#include <memory>

namespace bebox {

class Pit8254 : public ByteIoDevice {
public:
	static const u32 kClock = 1193182;
	typedef std::function<void(bool)> OutputCallback;

								Pit8254(Scheduler& scheduler,
									u64 ticksPerSecond);
								~Pit8254();

			void				Reset();
			void				SetOutputCallback(int counter,
									OutputCallback callback);
			void				SetGate(int counter, bool level);
			bool				Output(int counter);

	virtual	u8					IoRead8(u32 port);
	virtual	void				IoWrite8(u32 port, u8 value);
	virtual	const char*			IoName() const { return "pit"; }

private:
	struct Counter {
		int index;
		int mode;
		int access;			// 1 LSB, 2 MSB, 3 LSB then MSB
		bool bcd;
		u32 count;			// initial count (0 = 65536)
		u64 loadTick;		// PIT clock at which counting started
		bool counting;
		bool gate;
		bool writeLow;		// next write is the low byte (access 3)
		u8 pendingLow;
		bool readLow;		// next read is the low byte (access 3)
		bool latched;
		u16 latch;
		bool statusLatched;
		u8 status;
		bool output;
		OutputCallback callback;
		std::unique_ptr<Timer> timer;
	};

			u64					_PitTicks() const;
			u64					_MachineTime(u64 pitTick) const;
			u32					_Period(const Counter& counter) const;
			u16					_CurrentCount(Counter& counter);
			bool				_ComputeOutput(Counter& counter, u64 pitTick,
									u64& nextChange);
			void				_Reschedule(Counter& counter);
			void				_Load(Counter& counter, u32 count);
			void				_SetOutput(Counter& counter, bool level);

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			Counter				fCounters[3];
};

}	// namespace bebox

#endif	// BEBOX_PIT_8254_H
