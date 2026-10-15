/*
 * BeBox emulator - logging.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#ifndef BEBOX_LOG_H
#define BEBOX_LOG_H

#include "Types.h"

namespace bebox {

enum LogCategory {
	kLogCpu = 0,
	kLogMmu,
	kLogBus,		// unclaimed accesses
	kLogBoard,
	kLogPci,
	kLogIsa,
	kLogPic,
	kLogTimer,
	kLogDma,
	kLogSerial,
	kLogKbd,
	kLogRtc,
	kLogFlash,
	kLogFloppy,
	kLogIde,
	kLogScsi,
	kLogVideo,
	kLogAudio,
	kLogJoystick,
	kLogStorage,
	kLogMachine,
	kLogHost,
	kLogCategoryCount
};

enum LogLevel {
	kLogError = 0,
	kLogWarning,
	kLogInfo,
	kLogDebug,
	kLogTrace
};

typedef void (*LogSink)(LogCategory category, LogLevel level,
	const char* message, void* cookie);

class Log {
public:
	static void SetSink(LogSink sink, void* cookie);
	static void SetLevel(LogCategory category, LogLevel level);
	static void SetAllLevels(LogLevel level);
	static LogLevel Level(LogCategory category)
		{ return sLevels[category]; }
	static bool Enabled(LogCategory category, LogLevel level)
		{ return level <= sLevels[category]; }
	static void Write(LogCategory category, LogLevel level,
		const char* format, ...) __attribute__((format(printf, 3, 4)));
	static const char* CategoryName(LogCategory category);
	static bool CategoryFromName(const char* name, LogCategory& category);

private:
	static LogLevel sLevels[kLogCategoryCount];
	static LogSink sSink;
	static void* sCookie;
};

}	// namespace bebox

#define BB_LOG(cat, lvl, ...) \
	do { \
		if (bebox::Log::Enabled(bebox::cat, bebox::lvl)) \
			bebox::Log::Write(bebox::cat, bebox::lvl, __VA_ARGS__); \
	} while (0)

#endif	// BEBOX_LOG_H
