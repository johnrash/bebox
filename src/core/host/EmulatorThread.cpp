/*
 * BeBox emulator - runs a Machine on its own thread in real time.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "EmulatorThread.h"

#include "../Log.h"
#include "../board/BeBoxBoard.h"
#include "../board/Machine.h"
#include "../video/VgaCore.h"

#include <algorithm>
#include <chrono>

namespace bebox {

// The processors' speed control (factors of the model's speed) above a
// real BeBox: slower while the emulation falls behind real time with busy
// processors (not while it catches up), faster again (up to the limit)
// while it waits for real time. Never slower than the setting or a real
// BeBox: BeOS's sound (an MP3 decoder) needs every instruction it gets in
// an emulated second, so below that the host's lag slows time instead.
static const double kBehind = 0.005;		// s
static const double kSlower = 0.97;			// per 1 ms run
static const double kFaster = 1.01;


EmulatorThread::EmulatorThread()
	:
	fQuit(false),
	fPaused(false),
	fSpeedLimit(100)
{
}


EmulatorThread::~EmulatorThread()
{
	Stop();
}


bool
EmulatorThread::Start(const MachineConfig& config, std::string& error,
	Command setup)
{
	Stop();
	fMachine = Machine::Create(config);
	if (!fMachine->Init(error)) {
		fMachine.reset();
		return false;
	}
	if (setup)
		setup(*fMachine);
	fQuit = false;
	fThread = std::thread([this]() { _Run(); });
	return true;
}


void
EmulatorThread::Stop()
{
	if (fThread.joinable()) {
		fQuit = true;
		fCondition.notify_all();
		fThread.join();
	}
	if (fMachine) {
		std::string error;
		if (!fMachine->SavePersistentState(error))
			BB_LOG(kLogHost, kLogError, "%s", error.c_str());
		fMachine.reset();
	}
	std::lock_guard<std::mutex> lock(fStatusLock);
	fStatus = Status();
}


void
EmulatorThread::Post(Command command)
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		fCommands.push_back(command);
	}
	fCondition.notify_all();
}


void
EmulatorThread::DiscardCommands()
{
	std::lock_guard<std::mutex> lock(fLock);
	fCommands.clear();
}


void
EmulatorThread::SetPaused(bool paused)
{
	fPaused = paused;
	fCondition.notify_all();
}


EmulatorThread::Status
EmulatorThread::GetStatus()
{
	std::lock_guard<std::mutex> lock(fStatusLock);
	return fStatus;
}


void
EmulatorThread::_Run()
{
	typedef std::chrono::steady_clock Clock;
	Machine& machine = *fMachine;
	const u64 ticksPerSecond = machine.TicksPerSecond();
	const u64 slice = ticksPerSecond / 1000;			// 1 ms
	const u64 ledPeriod = ticksPerSecond / 60;
	const u64 savePeriod = ticksPerSecond * 30;

	Clock::time_point realStart = Clock::now();
	u64 emulatedStart = machine.Now();
	u64 nextLeds = machine.Now() + ledPeriod;
	u64 nextSave = machine.Now() + savePeriod;
	Clock::time_point statsTime = realStart;
	u64 statsEmulated = machine.Now();
	u64 statsInstructions = machine.InstructionCount();
	int lastLimit = fSpeedLimit;

	// An instruction takes nominal / speed ticks; the fraction carries
	// over to the next run.
	const double nominalTicks = machine.NominalInstructionTicks();
	const int cpus = std::max(1, machine.CpuCount());
	double speed = 1.0;
	double tickCarry = 0;
	u32 instructionTicks = 0;
	double speedSum = 0;
	int speedRuns = 0;
	double lastLead = 0;

	while (!fQuit) {
		std::vector<Command> commands;
		{
			std::unique_lock<std::mutex> lock(fLock);
			if (fPaused && fCommands.empty()) {
				{
					std::lock_guard<std::mutex> statusLock(fStatusLock);
					fStatus.paused = true;
				}
				fCondition.wait_for(lock, std::chrono::milliseconds(100));
				// do not count the pause as lag
				realStart = Clock::now();
				emulatedStart = machine.Now();
				continue;
			}
			commands.swap(fCommands);
		}
		for (Command& command : commands)
			command(machine);
		if (fPaused)
			continue;

		int limit = fSpeedLimit;
		// as fast as possible: an instruction a tick
		double maxSpeed = limit > 0 ? limit / 100.0 : nominalTicks;
		double minSpeed = std::min(1.0, maxSpeed);
		speed = std::max(minSpeed, std::min(speed, maxSpeed));
		tickCarry += nominalTicks / speed;
		u32 ticks = std::max<u32>(1, (u32)tickCarry);
		tickCarry = std::max(0.0, tickCarry - ticks);
		if (ticks != instructionTicks) {
			machine.SetInstructionTicks(ticks);
			instructionTicks = ticks;
		}
		u64 instructionsBefore = machine.InstructionCount();
		machine.RunFor(slice);
		// the share of the run the processors were not dozing
		double busy = (machine.InstructionCount() - instructionsBefore)
			* (double)ticks / ((double)slice * cpus);
		speedSum += speed;
		speedRuns++;

		if (machine.Now() >= nextLeds) {
			nextLeds += ledPeriod;
			float left, right, disk;
			machine.SampleLeds(left, right, disk);
			std::lock_guard<std::mutex> lock(fStatusLock);
			fStatus.running = true;
			fStatus.paused = false;
			fStatus.leds[0] = left;
			fStatus.leds[1] = right;
			fStatus.disk = disk;
			fStatus.cpuActive[0] = machine.CpuActive(0);
			fStatus.cpuActive[1] = machine.CpuActive(1);
			int cursorX = -1, cursorY = -1;
			if (machine.Video() == nullptr
				|| !machine.Video()->HardwareCursor(cursorX, cursorY))
				cursorX = cursorY = -1;
			fStatus.cursorX = cursorX;
			fStatus.cursorY = cursorY;
			fStatus.emulatedSeconds = machine.Now()
				/ (double)ticksPerSecond;
		}
		if (machine.Now() >= nextSave) {
			nextSave += savePeriod;
			std::string error;
			machine.SavePersistentState(error);
		}

		// Pacing: emulated time follows real time
		if (limit != lastLimit) {
			lastLimit = limit;
			realStart = Clock::now();
			emulatedStart = machine.Now();
		}
		Clock::time_point now = Clock::now();
		double emulated = (machine.Now() - emulatedStart)
			/ (double)ticksPerSecond;
		double real = std::chrono::duration<double>(now - realStart).count();
		double lead = emulated - real;
		if (lead > 0.001) {
			speed = std::min(maxSpeed, speed * kFaster);
			std::unique_lock<std::mutex> lock(fLock);
			fCondition.wait_for(lock, std::chrono::duration<double>(lead));
		} else if (lead < -kBehind) {
			if (busy > 0.1 && lead < lastLead)
				speed = std::max(minSpeed, speed * kSlower);
			if (lead < -0.25) {
				// too far behind (the host was busy): do not catch up
				realStart = now;
				emulatedStart = machine.Now();
				lead = 0;
			}
		}
		lastLead = lead;

		double statsSpan = std::chrono::duration<double>(now - statsTime)
			.count();
		if (statsSpan >= 0.5) {
			u64 instructions = machine.InstructionCount();
			std::lock_guard<std::mutex> lock(fStatusLock);
			// time against real time, times the processors' speed
			fStatus.speedPercent = 100.0 * (machine.Now() - statsEmulated)
				/ (double)ticksPerSecond / statsSpan
				* (speedRuns > 0 ? speedSum / speedRuns : speed);
			speedSum = 0;
			speedRuns = 0;
			fStatus.mips = (instructions - statsInstructions) / statsSpan
				/ 1e6;
			statsTime = now;
			statsEmulated = machine.Now();
			statsInstructions = instructions;
		}
	}
}

}	// namespace bebox
