/*
 * BeBox emulator - device timers in emulated time.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Scheduler.h"

namespace bebox {

thread_local const Scheduler* Scheduler::sClockOwner = nullptr;
thread_local const u64* Scheduler::sClock = nullptr;

Timer::Timer(Scheduler& scheduler, Callback callback, const char* name)
	:
	fScheduler(scheduler),
	fCallback(callback),
	fName(name),
	fWhen(0),
	fGeneration(0),
	fArmed(false)
{
}


Timer::~Timer()
{
	// Queue entries of a destroyed timer must not fire: the scheduler keeps
	// raw pointers, so timers live as long as the scheduler (they are
	// members of devices owned by the machine).
	fGeneration++;
	fArmed = false;
}


void
Timer::Arm(u64 when)
{
	fGeneration++;
	fWhen = when;
	fArmed = true;
	fScheduler._Push(this);
}


void
Timer::ArmIn(u64 ticks)
{
	Arm(fScheduler.Now() + ticks);
}


void
Timer::Cancel()
{
	fGeneration++;
	fArmed = false;
}


Scheduler::Scheduler()
	:
	fNow(0)
{
}


Scheduler::~Scheduler()
{
	SetClock(nullptr);
}


void
Scheduler::_Push(Timer* timer)
{
	fQueue.push(Entry{ timer->fWhen, timer->fGeneration, timer });
}


u64
Scheduler::NextEventTime()
{
	while (!fQueue.empty()) {
		const Entry& top = fQueue.top();
		if (top.timer->fArmed && top.generation == top.timer->fGeneration)
			return top.when;
		fQueue.pop();	// stale entry
	}
	return ~(u64)0;
}


void
Scheduler::RunDue(u64 now)
{
	while (!fQueue.empty()) {
		Entry top = fQueue.top();
		if (top.when > now)
			break;
		fQueue.pop();
		Timer* timer = top.timer;
		if (!timer->fArmed || top.generation != timer->fGeneration)
			continue;
		timer->fArmed = false;
		// Callbacks see the exact time the timer was due.
		const Scheduler* savedOwner = sClockOwner;
		const u64* savedClock = sClock;
		u64 saved = fNow;
		fNow = top.when;
		sClockOwner = nullptr;
		timer->fCallback();
		fNow = saved;
		sClockOwner = savedOwner;
		sClock = savedClock;
	}
}

}	// namespace bebox
