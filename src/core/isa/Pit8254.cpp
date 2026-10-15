/*
 * BeBox emulator - Intel 8254 programmable interval timer.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Pit8254.h"

#include "../Log.h"

namespace bebox {

static u16
ToBcd(u32 value)
{
	value %= 10000;
	return (u16)((value % 10) | ((value / 10 % 10) << 4)
		| ((value / 100 % 10) << 8) | ((value / 1000) << 12));
}


static u32
FromBcd(u16 value)
{
	return (value & 0xf) + ((value >> 4) & 0xf) * 10
		+ ((value >> 8) & 0xf) * 100 + ((value >> 12) & 0xf) * 1000;
}


Pit8254::Pit8254(Scheduler& scheduler, u64 ticksPerSecond)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond)
{
	for (int i = 0; i < 3; i++) {
		Counter& counter = fCounters[i];
		counter.index = i;
		counter.timer.reset(new Timer(scheduler, [this, i]() {
			_Reschedule(fCounters[i]);
		}, "pit"));
	}
	Reset();
}


Pit8254::~Pit8254()
{
}


void
Pit8254::Reset()
{
	for (int i = 0; i < 3; i++) {
		Counter& counter = fCounters[i];
		counter.mode = 0;
		counter.access = 3;
		counter.bcd = false;
		counter.count = 0x10000;
		counter.loadTick = 0;
		counter.counting = false;
		counter.gate = i != 2;	// counter 2's gate comes from port 0x61
		counter.writeLow = true;
		counter.pendingLow = 0;
		counter.readLow = true;
		counter.latched = false;
		counter.latch = 0;
		counter.statusLatched = false;
		counter.status = 0;
		counter.output = false;
		counter.timer->Cancel();
	}
	// The firmware expects the refresh counter running (port 0x61 bit 4).
	Counter& refresh = fCounters[1];
	refresh.mode = 2;
	refresh.count = 18;
	refresh.counting = true;
}


void
Pit8254::SetOutputCallback(int counter, OutputCallback callback)
{
	fCounters[counter].callback = callback;
}


u64
Pit8254::_PitTicks() const
{
	return (u64)(((unsigned __int128)fScheduler.Now() * kClock)
		/ fTicksPerSecond);
}


u64
Pit8254::_MachineTime(u64 pitTick) const
{
	unsigned __int128 product = (unsigned __int128)pitTick * fTicksPerSecond;
	return (u64)((product + kClock - 1) / kClock);
}


u32
Pit8254::_Period(const Counter& counter) const
{
	u32 count = counter.count == 0 ? 0x10000 : counter.count;
	if (counter.bcd && count == 0x10000)
		count = 10000;
	return count;
}


// Output level at 'pitTick' and the tick of the next output change
// (~0 if none).
bool
Pit8254::_ComputeOutput(Counter& counter, u64 pitTick, u64& nextChange)
{
	nextChange = ~(u64)0;
	if (!counter.counting)
		return counter.mode != 0;
	u64 elapsed = pitTick - counter.loadTick;
	u32 period = _Period(counter);
	switch (counter.mode) {
		case 0:
		case 1:
			if (elapsed < period) {
				nextChange = counter.loadTick + period;
				return false;
			}
			return true;
		case 2:
		case 6:
		{
			u64 phase = elapsed % period;
			u64 base = pitTick - phase;
			if (phase == period - 1) {
				nextChange = base + period;
				return false;
			}
			nextChange = base + period - 1;
			return true;
		}
		case 3:
		case 7:
		{
			u64 phase = elapsed % period;
			u64 base = pitTick - phase;
			u32 high = (period + 1) / 2;
			if (phase < high) {
				nextChange = base + high;
				return true;
			}
			nextChange = base + period;
			return false;
		}
		case 4:
		case 5:
			if (elapsed == period) {
				nextChange = counter.loadTick + period + 1;
				return false;
			}
			if (elapsed < period)
				nextChange = counter.loadTick + period;
			return true;
	}
	return true;
}


void
Pit8254::_SetOutput(Counter& counter, bool level)
{
	if (counter.output == level)
		return;
	counter.output = level;
	if (counter.callback)
		counter.callback(level);
}


void
Pit8254::_Reschedule(Counter& counter)
{
	u64 now = _PitTicks();
	u64 nextChange;
	bool level = _ComputeOutput(counter, now, nextChange);
	_SetOutput(counter, level);
	// Only counters with an observer need timers (counter 0: IRQ0).
	if (counter.callback && nextChange != ~(u64)0)
		counter.timer->Arm(_MachineTime(nextChange));
	else
		counter.timer->Cancel();
}


void
Pit8254::_Load(Counter& counter, u32 count)
{
	counter.count = count;
	if (counter.bcd)
		counter.count = FromBcd((u16)count);
	counter.loadTick = _PitTicks() + 1;
	counter.counting = counter.gate || counter.mode == 1 || counter.mode == 5;
	if (counter.mode == 0)
		_SetOutput(counter, false);
	_Reschedule(counter);
	BB_LOG(kLogTimer, kLogDebug, "pit counter %d: mode %d count %u",
		counter.index, counter.mode, counter.count);
}


u16
Pit8254::_CurrentCount(Counter& counter)
{
	if (!counter.counting)
		return (u16)counter.count;
	u64 now = _PitTicks();
	u64 elapsed = now >= counter.loadTick ? now - counter.loadTick : 0;
	u32 period = _Period(counter);
	u32 value;
	switch (counter.mode) {
		case 2:
		case 6:
			value = period - (u32)(elapsed % period);
			break;
		case 3:
		case 7:
			value = (period - 2 * (u32)(elapsed % ((period + 1) / 2)))
				& 0xfffe;
			break;
		default:
			value = (u32)((period - elapsed) & 0xffff);
			break;
	}
	if (counter.bcd)
		return ToBcd(value);
	return (u16)value;
}


void
Pit8254::SetGate(int index, bool level)
{
	Counter& counter = fCounters[index];
	if (counter.gate == level)
		return;
	bool rising = level && !counter.gate;
	counter.gate = level;
	if (counter.mode == 1 || counter.mode == 5) {
		if (rising) {
			counter.loadTick = _PitTicks() + 1;
			counter.counting = true;
		}
	} else if (counter.mode == 2 || counter.mode == 3 || counter.mode == 6
		|| counter.mode == 7) {
		if (rising) {
			counter.loadTick = _PitTicks() + 1;
			counter.counting = true;
		} else if (!level) {
			counter.counting = false;
		}
	} else {
		counter.counting = level;
	}
	_Reschedule(counter);
}


bool
Pit8254::Output(int index)
{
	Counter& counter = fCounters[index];
	u64 nextChange;
	return _ComputeOutput(counter, _PitTicks(), nextChange);
}


u8
Pit8254::IoRead8(u32 port)
{
	int index = port & 3;
	if (index == 3)
		return 0xff;
	Counter& counter = fCounters[index];
	if (counter.statusLatched) {
		counter.statusLatched = false;
		return counter.status;
	}
	u16 value = counter.latched ? counter.latch : _CurrentCount(counter);
	u8 result;
	switch (counter.access) {
		case 1:
			result = (u8)value;
			counter.latched = false;
			break;
		case 2:
			result = (u8)(value >> 8);
			counter.latched = false;
			break;
		default:
			if (counter.readLow) {
				result = (u8)value;
				counter.readLow = false;
				if (!counter.latched) {
					counter.latch = value;
					counter.latched = true;	// keep the high byte coherent
				}
			} else {
				result = (u8)(value >> 8);
				counter.readLow = true;
				counter.latched = false;
			}
			break;
	}
	return result;
}


void
Pit8254::IoWrite8(u32 port, u8 value)
{
	int index = port & 3;
	if (index == 3) {
		int select = value >> 6;
		if (select == 3) {
			// read-back command
			for (int i = 0; i < 3; i++) {
				if ((value & (2 << i)) == 0)
					continue;
				Counter& counter = fCounters[i];
				if ((value & 0x20) == 0 && !counter.latched) {
					counter.latch = _CurrentCount(counter);
					counter.latched = true;
					counter.readLow = true;
				}
				if ((value & 0x10) == 0) {
					counter.status = (Output(i) ? 0x80 : 0)
						| (counter.counting ? 0 : 0x40)
						| (counter.access << 4) | (counter.mode << 1)
						| (counter.bcd ? 1 : 0);
					counter.statusLatched = true;
				}
			}
			return;
		}
		Counter& counter = fCounters[select];
		int access = (value >> 4) & 3;
		if (access == 0) {
			// counter latch command
			if (!counter.latched) {
				counter.latch = _CurrentCount(counter);
				counter.latched = true;
				counter.readLow = true;
			}
			return;
		}
		counter.access = access;
		counter.mode = (value >> 1) & 7;
		counter.bcd = (value & 1) != 0;
		counter.writeLow = true;
		counter.readLow = true;
		counter.latched = false;
		counter.counting = false;
		_SetOutput(counter, counter.mode != 0);
		counter.timer->Cancel();
		return;
	}

	Counter& counter = fCounters[index];
	switch (counter.access) {
		case 1:
			_Load(counter, value);
			break;
		case 2:
			_Load(counter, (u32)value << 8);
			break;
		default:
			if (counter.writeLow) {
				counter.pendingLow = value;
				counter.writeLow = false;
				if (counter.mode == 0)
					counter.counting = false;
			} else {
				counter.writeLow = true;
				_Load(counter, counter.pendingLow | ((u32)value << 8));
			}
			break;
	}
}

}	// namespace bebox
