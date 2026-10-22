/*
 * BeBox emulator - threaded mode: one host thread or two.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */

#include "ThreadGovernor.h"

#include "../Log.h"

#include <algorithm>
#include <chrono>

namespace bebox {

// two threads: the share of the time they waited, over this long with
// both running; one thread takes over this much above the break-even
// share, a probe keeps two threads this much below it
static const double kWatchTime = 0.2;
static const double kHysteresis = 0.05;
// the speedup until a trial measured it
static const double kDefaultSpeedup = 1.3;
// trials: when the threads waited less than this share; blocks of this
// long on one thread and two, until each had this long; the other choice
// wins when it is this much faster
static const double kCalm = 0.05;
static const double kBlockTime = 0.005;
static const double kTrialTime = 0.02;
static const double kMargin = 1.05;
// probes: settling, then measuring
static const double kSettleTime = 0.01;
static const double kProbeTime = 0.04;
// from one probe (trial) to the next
static const double kMinInterval = 1;
static const double kMaxProbeInterval = 16;
static const double kMaxTrialInterval = 32;


ThreadGovernor::ThreadGovernor()
	:
	fState(kTwo),
	fTwoThreads(true),
	fSpeedup(kDefaultSpeedup),
	fBoth(0),
	fWaited(0),
	fSettled(false),
	fNextProbe(0),
	fProbeInterval(kMinInterval),
	fNextTrial(0),
	fTrialInterval(kMinInterval),
	fTrialFrom(true),
	fTrialWaited(0),
	fBlock(0),
	fSkip(false)
{
}


double
ThreadGovernor::HostSeconds()
{
	return std::chrono::duration<double>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}


void
ThreadGovernor::SliceTimed(u64 instructions, double start, double end)
{
	if (fState != kTrial)
		return;
	double waited = fWaited;
	fWaited = 0;
	// The first slice of a block may wait for the other thread to wake.
	if (fSkip) {
		fSkip = false;
		return;
	}
	int index = fTwoThreads ? 1 : 0;
	fInstructions[index] += instructions;
	fSeconds[index] += end - start;
	if (fTwoThreads)
		fTrialWaited += waited;
	fBlock += end - start;
	if (fBlock < kBlockTime)
		return;
	if (fSeconds[0] >= kTrialTime && fSeconds[1] >= kTrialTime)
		_EndTrial(end);
	else {
		fTwoThreads = !fTwoThreads;
		fBlock = 0;
		fSkip = true;
	}
}


void
ThreadGovernor::BothRan(double now, double seconds)
{
	if (fState != kTwo && fState != kProbe)
		return;
	fBoth += seconds;
	if (fState == kProbe && !fSettled) {
		if (fBoth < kSettleTime)
			return;
		fSettled = true;
		fBoth = 0;
		fWaited = 0;
		return;
	}
	if (fBoth < (fState == kTwo ? kWatchTime : kProbeTime))
		return;
	double share = fWaited / fBoth;
	fBoth = 0;
	fWaited = 0;
	double breakEven = _BreakEven();
	if (fState == kTwo) {
		if (share > breakEven + kHysteresis) {
			BB_LOG(kLogMachine, kLogDebug, "threads: waited %.0f%% of the "
				"time (speedup %.2f): one thread", share * 100, fSpeedup);
			_Choose(false, now);
		} else if (share < kCalm && now >= fNextTrial)
			_StartTrial(true);
		return;
	}
	// a probe
	if (share < kCalm && now >= fNextTrial)
		_StartTrial(false);
	else {
		bool twoThreads = share < breakEven - kHysteresis;
		BB_LOG(kLogMachine, kLogDebug, "threads: probe waited %.0f%% of the "
			"time (speedup %.2f): %s", share * 100, fSpeedup,
			twoThreads ? "two threads" : "one thread");
		_Choose(twoThreads, now);
	}
}


void
ThreadGovernor::RunEnded(double now)
{
	if (fState == kOne && now >= fNextProbe) {
		fCounts.probes++;
		fState = kProbe;
		fTwoThreads = true;
		fSettled = false;
		fBoth = 0;
		fWaited = 0;
	}
}


// The share of the time two threads may wait and still be as fast as one.
double
ThreadGovernor::_BreakEven() const
{
	return std::max(0.0, 1 - 1 / fSpeedup);
}


void
ThreadGovernor::_Choose(bool twoThreads, double now)
{
	// Probes back off while one thread stays faster.
	bool fromOne = fState == kOne || fState == kProbe
		|| (fState == kTrial && !fTrialFrom);
	if (fromOne && twoThreads)
		fCounts.toTwo++;
	else if (!fromOne && !twoThreads)
		fCounts.toOne++;
	if (twoThreads)
		fProbeInterval = kMinInterval;
	else if (fromOne)
		fProbeInterval = std::min(kMaxProbeInterval, fProbeInterval * 2);
	fState = twoThreads ? kTwo : kOne;
	fTwoThreads = twoThreads;
	fNextProbe = now + fProbeInterval;
	fBoth = 0;
	fWaited = 0;
}


void
ThreadGovernor::_StartTrial(bool from)
{
	fCounts.trials++;
	fState = kTrial;
	fTrialFrom = from;
	fInstructions[0] = fInstructions[1] = 0;
	fSeconds[0] = fSeconds[1] = 0;
	fTrialWaited = 0;
	fWaited = 0;
	fBlock = 0;
	fSkip = true;
	fTwoThreads = !from;
}


void
ThreadGovernor::_EndTrial(double now)
{
	double one = fInstructions[0] / fSeconds[0];
	double two = fInstructions[1] / fSeconds[1];
	// the speedup while the threads do not wait: the waits are what the
	// share watched with two threads counts
	double running = std::max(fSeconds[1] - fTrialWaited, fSeconds[1] / 10);
	fSpeedup = one > 0 ? fInstructions[1] / running / one : kDefaultSpeedup;
	bool twoThreads = fTrialFrom ? one <= two * kMargin : two > one * kMargin;
	fNextTrial = now + fTrialInterval;
	fTrialInterval = std::min(kMaxTrialInterval, fTrialInterval * 2);
	BB_LOG(kLogMachine, kLogDebug, "threads: trial: one %.1f, two %.1f MIPS "
		"(waited %.0f%%, speedup %.2f): %s", one / 1e6, two / 1e6,
		fTrialWaited / fSeconds[1] * 100, fSpeedup,
		twoThreads ? "two threads" : "one thread");
	_Choose(twoThreads, now);
}

}	// namespace bebox
