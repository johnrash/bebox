/*
 * BeBox emulator - Benchmarq bq3285 real time clock (MC146818 compatible)
 * with 242 bytes of battery backed RAM.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Ports 0x70/0x71: index/data of bank 0 (clock registers and 114 bytes of
 * RAM); 0x72/0x73 (also 0x74/0x75): index/data of the extended 128 bytes.
 * The clock runs with emulated time, in binary 24 hour mode as on the
 * BeBox: BeOS DR6-PR2 read the registers as binary numbers (the boot ROM
 * never sets the mode; R3 and later honor it).
 */
#ifndef BEBOX_RTC_146818_H
#define BEBOX_RTC_146818_H

#include "../bus/IoSpace.h"
#include "../board/Scheduler.h"

#include <ctime>
#include <functional>
#include <memory>

namespace bebox {

class Rtc146818 : public ByteIoDevice {
public:
	static const int kRamSize = 256;	// bank 0 registers + RAM, bank 1
	typedef std::function<void(bool)> IrqCallback;

								Rtc146818(Scheduler& scheduler,
									u64 ticksPerSecond);

			void				Reset();
			void				SetIrqCallback(IrqCallback callback)
									{ fIrq = callback; }

			// Battery backed contents (persisted by the machine).
			u8*					Ram() { return fRam; }
			// Contents saved by an earlier run. Earlier versions of the
			// emulator started the clock in BCD mode: that becomes binary.
			void				LoadRam(const u8* data);
			// Seconds between host and guest time (persisted too).
			s64					TimeOffset() const { return fOffset; }
			void				SetTimeOffset(s64 offset) { fOffset = offset; }
			// Seconds since 1970 for a UTC date (a portable timegm()).
	static	s64					UtcSeconds(int year, int month, int day,
									int hour, int minute, int second);
			bool				IsDirty() const { return fDirty; }
			void				ClearDirty() { fDirty = false; }

	virtual	u8					IoRead8(u32 port);
	virtual	void				IoWrite8(u32 port, u8 value);
	virtual	const char*			IoName() const { return "rtc"; }

private:
			u8					_Read(u32 port);
			s64					_GuestTime() const;
			void				_ToRegisters(s64 time);
			s64					_FromRegisters() const;
			u8					_Encode(int value) const;
			int					_Decode(u8 value) const;
			void				_UpdateTimers();
			void				_UpdateIrq();
			u64					_PeriodicTicks() const;

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			u8					fRam[kRamSize];
			u8					fIndex;
			u8					fExtendedIndex;
			s64					fOffset;		// guest - host seconds
			time_t				fHostBase;		// host time at power on
			bool				fDirty;
			IrqCallback			fIrq;
			std::unique_ptr<Timer> fPeriodic;
			std::unique_ptr<Timer> fSecond;
			bool				fIrqLevel;
};

}	// namespace bebox

#endif	// BEBOX_RTC_146818_H
