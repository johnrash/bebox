/*
 * BeBox emulator - spinning for the threaded mode.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The emulated CPUs hand over to each other every few microseconds, which
 * a mutex that sleeps in the kernel when contended (Haiku's does at once)
 * makes very slow: they spin instead.
 */
#ifndef BEBOX_SPIN_H
#define BEBOX_SPIN_H

#include <atomic>
#include <thread>

namespace bebox {

inline void
CpuRelax()
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#endif
}


// A lock the holding thread may take again; waiters spin (then yield).
class RecursiveSpinLock {
public:
	void lock()
	{
		const void* self = &sToken;
		if (fOwner.load(std::memory_order_relaxed) == self) {
			fDepth++;
			return;
		}
		for (int spins = 0;; spins++) {
			const void* expected = nullptr;
			if (fOwner.load(std::memory_order_relaxed) == nullptr
				&& fOwner.compare_exchange_weak(expected, self,
					std::memory_order_acquire, std::memory_order_relaxed))
				break;
			if (spins < 1000)
				CpuRelax();
			else
				std::this_thread::yield();
		}
		fDepth = 1;
	}

	void unlock()
	{
		if (--fDepth == 0)
			fOwner.store(nullptr, std::memory_order_release);
	}

private:
	std::atomic<const void*> fOwner{nullptr};
	int fDepth = 0;
	static inline thread_local char sToken;
};

}	// namespace bebox

#endif	// BEBOX_SPIN_H
