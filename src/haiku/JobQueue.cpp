/*
 * BeBox emulator for Haiku - work off the window thread.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "JobQueue.h"


JobQueue::JobQueue()
	:
	fBusy(false),
	fQuit(false)
{
}


JobQueue::~JobQueue()
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		fQuit = true;
	}
	fCondition.notify_all();
	if (fThread.joinable())
		fThread.join();
}


void
JobQueue::Add(Job job)
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		fJobs.push_back(job);
		if (!fThread.joinable())
			fThread = std::thread([this]() { _Run(); });
	}
	fCondition.notify_all();
}


void
JobQueue::Wait()
{
	std::unique_lock<std::mutex> lock(fLock);
	fCondition.wait(lock, [this]() { return fJobs.empty() && !fBusy; });
}


void
JobQueue::_Run()
{
	std::unique_lock<std::mutex> lock(fLock);
	while (true) {
		fCondition.wait(lock, [this]() { return fQuit || !fJobs.empty(); });
		if (fJobs.empty())
			break;		// quitting, and every job done
		Job job = fJobs.front();
		fJobs.pop_front();
		fBusy = true;
		lock.unlock();
		job();
		lock.lock();
		fBusy = false;
		fCondition.notify_all();
	}
}
