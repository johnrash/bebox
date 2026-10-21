/*
 * BeBox emulator for Haiku - work off the window thread.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Runs jobs one after the other on a thread of its own (started with the
 * first job): the shared folders' copies at power on and off, which take
 * seconds with a big folder. The destructor finishes the queued jobs.
 */
#ifndef BEBOX_JOB_QUEUE_H
#define BEBOX_JOB_QUEUE_H

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

class JobQueue {
public:
	typedef std::function<void()> Job;

								JobQueue();
								~JobQueue();

			void				Add(Job job);
			// Until every job added is done.
			void				Wait();

private:
			void				_Run();

			std::thread			fThread;
			std::mutex			fLock;
			std::condition_variable fCondition;
			std::deque<Job>		fJobs;
			bool				fBusy;
			bool				fQuit;
};

#endif	// BEBOX_JOB_QUEUE_H
