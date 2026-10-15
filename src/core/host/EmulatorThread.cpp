/*
 * BeBox emulator - runs a Machine on its own thread in real time.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "EmulatorThread.h"

#include "../Log.h"
#include "../board/BeBoxBoard.h"
#include "../board/Machine.h"
#include "../video/VgaCore.h"

#include <chrono>

namespace bebox {

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

		machine.RunFor(slice);

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

		// Pacing
		int limit = fSpeedLimit;
		if (limit != lastLimit) {
			lastLimit = limit;
			realStart = Clock::now();
			emulatedStart = machine.Now();
		}
		Clock::time_point now = Clock::now();
		if (limit > 0) {
			double emulated = (machine.Now() - emulatedStart)
				/ (double)ticksPerSecond;
			double target = emulated * 100.0 / limit;
			double real = std::chrono::duration<double>(now - realStart)
				.count();
			if (target > real + 0.001) {
				std::unique_lock<std::mutex> lock(fLock);
				fCondition.wait_for(lock, std::chrono::duration<double>(
					target - real));
			} else if (real - target > 0.25) {
				// too far behind: do not try to catch up
				realStart = now;
				emulatedStart = machine.Now();
			}
		}

		double statsSpan = std::chrono::duration<double>(now - statsTime)
			.count();
		if (statsSpan >= 0.5) {
			u64 instructions = machine.InstructionCount();
			std::lock_guard<std::mutex> lock(fStatusLock);
			fStatus.speedPercent = 100.0 * (machine.Now() - statsEmulated)
				/ (double)ticksPerSecond / statsSpan;
			fStatus.mips = (instructions - statsInstructions) / statsSpan
				/ 1e6;
			statsTime = now;
			statsEmulated = machine.Now();
			statsInstructions = instructions;
		}
	}
}

}	// namespace bebox
