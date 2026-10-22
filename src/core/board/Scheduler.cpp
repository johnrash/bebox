/*
 * BeBox emulator - device timers in emulated time.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Scheduler.h"

#include <algorithm>
#include <chrono>

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


// The scheduler must outlive its timers (it does: devices and the machine
// declare it first).
Timer::~Timer()
{
	fScheduler._Remove(this);
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
	fQueue.push_back(Entry{ timer->fWhen, timer->fGeneration, timer });
	std::push_heap(fQueue.begin(), fQueue.end(), std::greater<Entry>());
}


// A timer goes away: its entries with it (the queue holds pointers).
void
Scheduler::_Remove(const Timer* timer)
{
	size_t count = fQueue.size();
	fQueue.erase(std::remove_if(fQueue.begin(), fQueue.end(),
		[timer](const Entry& entry) { return entry.timer == timer; }),
		fQueue.end());
	if (fQueue.size() != count)
		std::make_heap(fQueue.begin(), fQueue.end(), std::greater<Entry>());
}


void
Scheduler::_Pop()
{
	std::pop_heap(fQueue.begin(), fQueue.end(), std::greater<Entry>());
	fQueue.pop_back();
}


u64
Scheduler::NextEventTime()
{
	while (!fQueue.empty()) {
		const Entry& top = fQueue.front();
		if (top.timer->fArmed && top.generation == top.timer->fGeneration)
			return top.when;
		_Pop();		// stale entry
	}
	return ~(u64)0;
}


void
Scheduler::RunDue(u64 now)
{
	while (!fQueue.empty()) {
		Entry top = fQueue.front();
		if (top.when > now)
			break;
		_Pop();
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
		if (fProfiling) {
			auto start = std::chrono::steady_clock::now();
			timer->fCallback();
			TimerProfile& profile = fProfile[timer->fName];
			profile.calls++;
			profile.seconds += std::chrono::duration<double>(
				std::chrono::steady_clock::now() - start).count();
		} else
			timer->fCallback();
		fNow = saved;
		sClockOwner = savedOwner;
		sClock = savedClock;
	}
}

}	// namespace bebox
