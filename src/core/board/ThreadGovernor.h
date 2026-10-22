/*
 * BeBox emulator - threaded mode: one host thread or two.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * In threaded mode the CPUs hand over to each other at the end of every
 * time slice (a few microseconds), spinning. That is fast while both
 * threads have a core of their own. When other work keeps the host's
 * cores busy, the host's scheduler takes one of the threads off its core
 * for milliseconds at a time, the other one waits for it, and running both
 * CPUs one after the other on one thread is faster.
 *
 * The governor chooses one or two threads for the time slices in which
 * both CPUs run ("both running" below):
 * - With two threads it watches the share of the time the threads wait
 *   for each other (beyond a short spin). Two threads are faster than one
 *   by a factor (the speedup) while they never wait; when they wait more
 *   than 1 - 1 / speedup of the time, one thread takes over.
 * - The speedup depends on the guest's work. Trials measure it while the
 *   threads hardly wait: the slices alternate between one thread and two
 *   in blocks of a few milliseconds and are timed (instructions per
 *   second, without the time the threads waited). On a busy host such
 *   short blocks mislead (a thread woken after a block on one thread runs
 *   better or worse than one that never slept, depending on the host's
 *   scheduler), so they are not used there.
 * - With one thread, probes run two threads for a while (after a short
 *   settling time) and keep them if they wait less than the share above.
 *   Each probe that goes back to one thread doubles the time to the next
 *   (1 to 16 s); trials are repeated likewise (1 to 32 s).
 *
 * Times are host seconds (HostSeconds()). Only the machine's thread calls
 * it.
 */
#ifndef BEBOX_THREAD_GOVERNOR_H
#define BEBOX_THREAD_GOVERNOR_H

#include "../Types.h"

namespace bebox {

class ThreadGovernor {
public:
								ThreadGovernor();

			// Whether the next slice with both running uses two threads.
			bool				TwoThreads() const { return fTwoThreads; }
			// Whether that slice is to be timed (SliceTimed()): a trial.
			bool				Timing() const { return fState == kTrial; }
			// A timed slice: the instructions both CPUs executed, the host
			// time it began and ended.
			void				SliceTimed(u64 instructions, double start,
									double end);
			// In a slice on two threads, they waited for each other.
			void				Waited(double seconds)
									{ fWaited += seconds; }
			// A stretch of slices with both running ended (host time 'now',
			// 'seconds' long, the devices' work between the slices
			// included).
			void				BothRan(double now, double seconds);
			// A run of slices (Machine::RunFor()) ended.
			void				RunEnded(double now);

			// Two threads' speed over one thread's while they do not wait.
			double				Speedup() const { return fSpeedup; }
			// Decisions so far (for profiling).
			struct Counts {
				int				trials = 0;
				int				probes = 0;
				int				toOne = 0;		// switches to one thread
				int				toTwo = 0;		// and back
			};
			const Counts&		GetCounts() const { return fCounts; }

	static	double				HostSeconds();

private:
			enum State {
				kOne,
				kTwo,
				kProbe,		// two threads, from one
				kTrial
			};

			double				_BreakEven() const;
			void				_Choose(bool twoThreads, double now);
			void				_StartTrial(bool twoThreads);
			void				_EndTrial(double now);

			State				fState;
			bool				fTwoThreads;
			double				fSpeedup;
			// two threads (and probes): time with both running and the
			// time the threads waited in it
			double				fBoth;
			double				fWaited;
			bool				fSettled;
			double				fNextProbe;
			double				fProbeInterval;
			double				fNextTrial;
			double				fTrialInterval;
			// trial: [0] one thread, [1] two threads
			bool				fTrialFrom;		// the choice before
			u64					fInstructions[2];
			double				fSeconds[2];
			double				fTrialWaited;	// in the two threads' time
			double				fBlock;			// time timed in this block
			bool				fSkip;			// the block's first slice
			Counts				fCounts;
};

}	// namespace bebox

#endif	// BEBOX_THREAD_GOVERNOR_H
