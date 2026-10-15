/*
 * BeBox emulator - Benchmarq bq3285 real time clock.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Rtc146818.h"

#include "../Log.h"

namespace bebox {

enum {
	kSeconds = 0, kSecondsAlarm, kMinutes, kMinutesAlarm, kHours,
	kHoursAlarm, kDayOfWeek, kDayOfMonth, kMonth, kYear,
	kRegisterA = 0x0a, kRegisterB, kRegisterC, kRegisterD
};

enum {
	kA_UIP = 0x80,
	kB_SET = 0x80, kB_PIE = 0x40, kB_AIE = 0x20, kB_UIE = 0x10,
	kB_BINARY = 0x04, kB_24H = 0x02,
	kC_IRQF = 0x80, kC_PF = 0x40, kC_AF = 0x20, kC_UF = 0x10,
	kD_VRT = 0x80
};


s64
Rtc146818::UtcSeconds(int year, int month, int day, int hour, int minute,
	int second)
{
	// days from civil (H. Hinnant's algorithm)
	year -= month <= 2 ? 1 : 0;
	s64 era = (year >= 0 ? year : year - 399) / 400;
	s64 yearOfEra = year - era * 400;
	s64 dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	s64 dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100
		+ dayOfYear;
	s64 days = era * 146097 + dayOfEra - 719468;
	return days * 86400 + hour * 3600 + minute * 60 + second;
}


Rtc146818::Rtc146818(Scheduler& scheduler, u64 ticksPerSecond)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fIndex(0),
	fExtendedIndex(0),
	fOffset(0),
	fHostBase(time(nullptr)),
	fDirty(false),
	fIrqLevel(false)
{
	memset(fRam, 0, sizeof(fRam));
	fRam[kRegisterA] = 0x26;	// 32.768 kHz time base, 1024 Hz periodic
	fRam[kRegisterB] = kB_BINARY | kB_24H;
	fRam[kRegisterD] = kD_VRT;
	fPeriodic.reset(new Timer(scheduler, [this]() {
		fRam[kRegisterC] |= kC_PF;
		_UpdateIrq();
		_UpdateTimers();
	}, "rtc-periodic"));
	fSecond.reset(new Timer(scheduler, [this]() {
		fRam[kRegisterC] |= kC_UF;
		s64 now = _GuestTime();
		struct tm parts;
		time_t value = (time_t)now;
		gmtime_r(&value, &parts);
		u8 alarmSeconds = fRam[kSecondsAlarm];
		u8 alarmMinutes = fRam[kMinutesAlarm];
		u8 alarmHours = fRam[kHoursAlarm];
		auto matches = [this](u8 alarm, int value) {
			return (alarm & 0xc0) == 0xc0 || _Decode(alarm) == value;
		};
		if (matches(alarmSeconds, parts.tm_sec)
			&& matches(alarmMinutes, parts.tm_min)
			&& matches(alarmHours, parts.tm_hour))
			fRam[kRegisterC] |= kC_AF;
		_UpdateIrq();
		_UpdateTimers();
	}, "rtc-second"));
}


void
Rtc146818::LoadRam(const u8* data)
{
	memcpy(fRam, data, kRamSize);
	if (fRam[kRegisterB] == kB_24H) {
		BB_LOG(kLogRtc, kLogInfo, "clock switched from BCD to binary mode");
		fRam[kRegisterB] = kB_BINARY | kB_24H;
		fDirty = true;
	}
}


void
Rtc146818::Reset()
{
	// Interrupt enables and flags are cleared by a reset; the clock and
	// the RAM keep their contents.
	fRam[kRegisterB] &= ~(kB_PIE | kB_AIE | kB_UIE);
	fRam[kRegisterC] = 0;
	fIrqLevel = false;
	if (fIrq)
		fIrq(false);
	_UpdateTimers();
}


s64
Rtc146818::_GuestTime() const
{
	return (s64)fHostBase + (s64)(fScheduler.Now() / fTicksPerSecond)
		+ fOffset;
}


u8
Rtc146818::_Encode(int value) const
{
	if ((fRam[kRegisterB] & kB_BINARY) != 0)
		return (u8)value;
	return (u8)(((value / 10) << 4) | (value % 10));
}


int
Rtc146818::_Decode(u8 value) const
{
	if ((fRam[kRegisterB] & kB_BINARY) != 0)
		return value;
	return (value >> 4) * 10 + (value & 0xf);
}


void
Rtc146818::_ToRegisters(s64 now)
{
	struct tm parts;
	time_t value = (time_t)now;
	gmtime_r(&value, &parts);
	fRam[kSeconds] = _Encode(parts.tm_sec);
	fRam[kMinutes] = _Encode(parts.tm_min);
	int hours = parts.tm_hour;
	if ((fRam[kRegisterB] & kB_24H) == 0) {
		bool pm = hours >= 12;
		hours %= 12;
		if (hours == 0)
			hours = 12;
		fRam[kHours] = _Encode(hours) | (pm ? 0x80 : 0);
	} else
		fRam[kHours] = _Encode(hours);
	fRam[kDayOfWeek] = _Encode(parts.tm_wday + 1);
	fRam[kDayOfMonth] = _Encode(parts.tm_mday);
	fRam[kMonth] = _Encode(parts.tm_mon + 1);
	fRam[kYear] = _Encode(parts.tm_year % 100);
}


s64
Rtc146818::_FromRegisters() const
{
	int second = _Decode(fRam[kSeconds]);
	int minute = _Decode(fRam[kMinutes]);
	u8 hours = fRam[kHours];
	int hour;
	if ((fRam[kRegisterB] & kB_24H) == 0) {
		hour = _Decode(hours & 0x7f) % 12;
		if ((hours & 0x80) != 0)
			hour += 12;
	} else
		hour = _Decode(hours);
	int day = _Decode(fRam[kDayOfMonth]);
	int month = _Decode(fRam[kMonth]);
	int year = _Decode(fRam[kYear]);
	// Two digit years: 70-99 are the 1900s.
	year += year < 70 ? 2000 : 1900;
	return UtcSeconds(year, month, day, hour, minute, second);
}


u64
Rtc146818::_PeriodicTicks() const
{
	int rate = fRam[kRegisterA] & 0x0f;
	if (rate == 0)
		return 0;
	if (rate < 3)
		rate += 7;
	u64 hz = 65536 >> rate;
	return fTicksPerSecond / hz;
}


void
Rtc146818::_UpdateTimers()
{
	u8 b = fRam[kRegisterB];
	u64 periodic = _PeriodicTicks();
	if ((b & kB_PIE) != 0 && periodic != 0) {
		u64 now = fScheduler.Now();
		fPeriodic->Arm((now / periodic + 1) * periodic);
	} else
		fPeriodic->Cancel();
	if ((b & (kB_UIE | kB_AIE)) != 0 && (b & kB_SET) == 0) {
		u64 now = fScheduler.Now();
		fSecond->Arm((now / fTicksPerSecond + 1) * fTicksPerSecond);
	} else
		fSecond->Cancel();
}


void
Rtc146818::_UpdateIrq()
{
	u8 c = fRam[kRegisterC] & 0x70;
	u8 enabled = fRam[kRegisterB] & 0x70;
	bool level = (c & enabled) != 0;
	if (level)
		fRam[kRegisterC] |= kC_IRQF;
	else
		fRam[kRegisterC] &= ~kC_IRQF;
	if (level != fIrqLevel) {
		fIrqLevel = level;
		if (fIrq)
			fIrq(level);
	}
}


u8
Rtc146818::IoRead8(u32 port)
{
	u8 value = _Read(port);
	BB_LOG(kLogRtc, kLogTrace, "read %02x[%02x] = %02x", port,
		port == 0x71 ? fIndex & 0x7f : fExtendedIndex, value);
	return value;
}


u8
Rtc146818::_Read(u32 port)
{
	if (port == 0x70 || port == 0x72 || port == 0x74)
		return 0xff;
	if (port == 0x73 || port == 0x75)
		return fRam[0x80 | (fExtendedIndex & 0x7f)];
	u8 index = fIndex & 0x7f;
	switch (index) {
		case kSeconds: case kMinutes: case kHours: case kDayOfWeek:
		case kDayOfMonth: case kMonth: case kYear:
			if ((fRam[kRegisterB] & kB_SET) == 0)
				_ToRegisters(_GuestTime());
			return fRam[index];
		case kRegisterA:
		{
			// Update in progress during the last 244 us of each second.
			u64 phase = fScheduler.Now() % fTicksPerSecond;
			u64 window = fTicksPerSecond / 4096;
			u8 value = fRam[kRegisterA] & ~kA_UIP;
			if (phase >= fTicksPerSecond - window
				&& (fRam[kRegisterB] & kB_SET) == 0)
				value |= kA_UIP;
			return value;
		}
		case kRegisterC:
		{
			u8 value = fRam[kRegisterC];
			fRam[kRegisterC] = 0;
			_UpdateIrq();
			return value;
		}
		case kRegisterD:
			return kD_VRT;
	}
	return fRam[index];
}


void
Rtc146818::IoWrite8(u32 port, u8 value)
{
	if (port == 0x71 || port == 0x73 || port == 0x75) {
		BB_LOG(kLogRtc, kLogTrace, "write %02x[%02x] = %02x", port,
			port == 0x71 ? fIndex & 0x7f : fExtendedIndex, value);
	}
	if (port == 0x70) {
		fIndex = value;	// bit 7 is the NMI mask on PCs
		return;
	}
	if (port == 0x72 || port == 0x74) {
		fExtendedIndex = value;
		return;
	}
	if (port == 0x73 || port == 0x75) {
		fRam[0x80 | (fExtendedIndex & 0x7f)] = value;
		fDirty = true;
		return;
	}
	u8 index = fIndex & 0x7f;
	switch (index) {
		case kSeconds: case kMinutes: case kHours: case kDayOfWeek:
		case kDayOfMonth: case kMonth: case kYear:
			if ((fRam[kRegisterB] & kB_SET) == 0)
				_ToRegisters(_GuestTime());
			fRam[index] = value;
			if ((fRam[kRegisterB] & kB_SET) == 0) {
				fOffset += _FromRegisters() - _GuestTime();
				fDirty = true;
			}
			return;
		case kRegisterA:
			fRam[kRegisterA] = value & 0x7f;
			_UpdateTimers();
			fDirty = true;
			return;
		case kRegisterB:
		{
			bool wasSet = (fRam[kRegisterB] & kB_SET) != 0;
			if (!wasSet && (value & kB_SET) != 0)
				_ToRegisters(_GuestTime());	// freeze the time for writing
			// Changing the data mode re-encodes nothing: the guest writes
			// the registers again in the new format.
			fRam[kRegisterB] = value;
			if (wasSet && (value & kB_SET) == 0) {
				fOffset += _FromRegisters() - _GuestTime();
				BB_LOG(kLogRtc, kLogInfo, "time set, offset %lld s",
					(long long)fOffset);
			}
			_UpdateTimers();
			_UpdateIrq();
			fDirty = true;
			return;
		}
		case kRegisterC:
		case kRegisterD:
			return;		// read only
	}
	fRam[index] = value;
	fDirty = true;
}

}	// namespace bebox
