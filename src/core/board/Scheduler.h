/*
 * BeBox emulator - device timers in emulated time.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Time is counted in machine ticks (8 per bus clock, see Machine.h). Devices
 * own Timer objects and arm them for an absolute time; the machine fires
 * due timers between CPU time slices.
 */
#ifndef BEBOX_SCHEDULER_H
#define BEBOX_SCHEDULER_H

#include "../Types.h"

#include <functional>
#include <queue>
#include <vector>

namespace bebox {

class Scheduler;

class Timer {
public:
	typedef std::function<void()> Callback;

								Timer(Scheduler& scheduler, Callback callback,
									const char* name = "timer");
								~Timer();

			void				Arm(u64 when);
			void				ArmIn(u64 ticks);
			void				Cancel();
			bool				IsArmed() const { return fArmed; }
			u64					When() const { return fWhen; }
			const char*			Name() const { return fName; }

private:
	friend class Scheduler;

			Scheduler&			fScheduler;
			Callback			fCallback;
			const char*			fName;
			u64					fWhen;
			u64					fGeneration;
			bool				fArmed;
};

class Scheduler {
public:
								Scheduler();
								~Scheduler();

			// The current time. While a CPU executes, this is that CPU's
			// local time: the machine points the clock of the thread
			// running it at the CPU's (a thread per CPU in threaded mode).
			u64					Now() const
									{ return sClockOwner == this
										? *sClock : fNow; }
			void				SetNow(u64 now) { fNow = now; }
			// The calling thread's clock (null: the machine time).
			void				SetClock(const u64* clock)
									{
										if (clock != nullptr) {
											sClockOwner = this;
											sClock = clock;
										} else if (sClockOwner == this)
											sClockOwner = nullptr;
									}

			// Time of the earliest armed timer, or ~0.
			u64					NextEventTime();
			// Fires all timers that are due at or before 'now'.
			void				RunDue(u64 now);

private:
	friend class Timer;

	struct Entry {
		u64 when;
		u64 generation;
		Timer* timer;
		bool operator>(const Entry& other) const
			{ return when > other.when; }
	};

			void				_Push(Timer* timer);

			u64					fNow;
	static thread_local const Scheduler* sClockOwner;
	static thread_local const u64* sClock;
			std::priority_queue<Entry, std::vector<Entry>,
				std::greater<Entry> > fQueue;
};

}	// namespace bebox

#endif	// BEBOX_SCHEDULER_H
