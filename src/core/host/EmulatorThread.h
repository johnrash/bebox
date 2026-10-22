/*
 * BeBox emulator - runs a Machine on its own thread in real time.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The machine is only touched by the emulation thread. Front ends post
 * commands (run on that thread between time slices) and read a status
 * snapshot.
 *
 * Emulated time follows real time; the speed limit sets the processors'
 * speed, so BeOS's clock and timers and the keyboard's repeat keep real
 * time also when the processors run faster than a real BeBox. When the
 * host cannot keep up, faster processors give back their extra speed;
 * at a real BeBox's speed time slows for a while instead (and catches up).
 */
#ifndef BEBOX_EMULATOR_THREAD_H
#define BEBOX_EMULATOR_THREAD_H

#include "../board/MachineConfig.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bebox {

class Machine;

class EmulatorThread {
public:
	typedef std::function<void(Machine&)> Command;

	struct Status {
		bool running = false;
		bool paused = false;
		// percent of the model's speed (processors and time)
		double speedPercent = 0;
		double mips = 0;
		float leds[2] = { 0, 0 };	// bar graph levels 0-15
		float disk = 0;				// disk LED duty cycle
		bool cpuActive[2] = { false, false };
		// the guest's hardware cursor (-1: none shown)
		int cursorX = -1;
		int cursorY = -1;
		double emulatedSeconds = 0;
	};

								EmulatorThread();
								~EmulatorThread();

			// Creates the machine and starts the thread. 'setup' runs on
			// the emulation thread after Machine::Init() (attach sinks and
			// backends there).
			bool				Start(const MachineConfig& config,
									std::string& error,
									Command setup = nullptr);
			// Stops the thread and saves the persistent state.
			void				Stop();
			bool				IsRunning() const { return fThread.joinable(); }

			// Commands posted before Start() run when the thread starts.
			void				Post(Command command);
			// Drops the commands not run yet.
			void				DiscardCommands();
			void				SetPaused(bool paused);
			bool				IsPaused() const { return fPaused; }
			// The processors' speed in percent of the model's (100: a real
			// BeBox), 0 = as fast as possible.
			void				SetSpeedLimit(int percent)
									{ fSpeedLimit = percent; }

			Status				GetStatus();

private:
			void				_Run();

			std::unique_ptr<Machine> fMachine;
			std::thread			fThread;
			std::mutex			fLock;
			std::condition_variable fCondition;
			std::vector<Command> fCommands;
			std::atomic<bool>	fQuit;
			std::atomic<bool>	fPaused;
			std::atomic<int>	fSpeedLimit;
			Status				fStatus;
			std::mutex			fStatusLock;
};

}	// namespace bebox

#endif	// BEBOX_EMULATOR_THREAD_H
