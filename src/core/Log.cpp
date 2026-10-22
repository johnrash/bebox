/*
 * BeBox emulator - logging.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>

namespace bebox {

static const char* const kCategoryNames[kLogCategoryCount] = {
	"cpu", "mmu", "bus", "board", "pci", "isa", "pic", "timer", "dma",
	"serial", "kbd", "rtc", "flash", "floppy", "ide", "scsi", "video",
	"audio", "joystick", "storage", "machine", "host"
};

void
Log::DefaultSink(LogCategory category, LogLevel level, const char* message,
	void*)
{
	static const char* const kLevels[] = { "E", "W", "I", "D", "T" };
	fprintf(stderr, "[%s %s] %s\n", kLevels[level], kCategoryNames[category],
		message);
}

LogLevel Log::sLevels[kLogCategoryCount] = {
	kLogWarning, kLogWarning, kLogWarning, kLogWarning, kLogWarning,
	kLogWarning, kLogWarning, kLogWarning, kLogWarning, kLogWarning,
	kLogWarning, kLogWarning, kLogWarning, kLogWarning, kLogWarning,
	kLogWarning, kLogWarning, kLogWarning, kLogWarning, kLogWarning,
	kLogWarning, kLogWarning
};
LogSink Log::sSink = DefaultSink;
void* Log::sCookie = nullptr;


void
Log::SetSink(LogSink sink, void* cookie)
{
	sSink = sink != nullptr ? sink : DefaultSink;
	sCookie = cookie;
}


void
Log::SetLevel(LogCategory category, LogLevel level)
{
	sLevels[category] = level;
}


void
Log::SetAllLevels(LogLevel level)
{
	for (int i = 0; i < kLogCategoryCount; i++)
		sLevels[i] = level;
}


void
Log::Write(LogCategory category, LogLevel level, const char* format, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	sSink(category, level, buffer, sCookie);
}


const char*
Log::CategoryName(LogCategory category)
{
	return kCategoryNames[category];
}


bool
Log::CategoryFromName(const char* name, LogCategory& category)
{
	for (int i = 0; i < kLogCategoryCount; i++) {
		if (strcasecmp(name, kCategoryNames[i]) == 0) {
			category = (LogCategory)i;
			return true;
		}
	}
	return false;
}


bool
Log::Configure(const char* spec)
{
	static const char* const kLevelNames[] = { "error", "warning", "info",
		"debug", "trace" };
	const char* equal = strchr(spec, '=');
	std::string name(spec, equal != nullptr ? equal - spec : strlen(spec));
	LogLevel level = equal != nullptr ? kLogWarning : kLogDebug;
	for (int i = 0; equal != nullptr && i < 5; i++) {
		if (strcmp(equal + 1, kLevelNames[i]) == 0)
			level = (LogLevel)i;
	}
	LogCategory category;
	if (name == "all")
		SetAllLevels(level);
	else if (CategoryFromName(name.c_str(), category))
		SetLevel(category, level);
	else
		return false;
	return true;
}

}	// namespace bebox
