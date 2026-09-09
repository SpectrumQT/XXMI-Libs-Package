#pragma once

using namespace std;

#include <string>
#include <ctime>

// Wrappers to make logging cleaner.

enum class LogVerbosity : int8_t {
	INVALID = -1,
	DISABLED = 0,
	WARNING = 1,
	INFO = 2,
	DEBUG = 3,
};

extern FILE* LogFile;
extern LogVerbosity gLogVerbosity;
extern bool gLogDebug;

// Note that for now I've left the definitions of LogFile and LogDebug as they
// were - either declared locally in a file, as an extern, or from another
// namespace altogether. At some point this needs to be cleaned up, but it's
// probably not worth doing so unless we were switching to use a central
// logging framework.

#define LogWarning(fmt, ...) \
	do { if (gLogVerbosity >= LogVerbosity::WARNING) fprintf(LogFile, fmt, __VA_ARGS__); } while (0)
#define vLogWarning(fmt, va_args) \
	do { if (gLogVerbosity >= LogVerbosity::WARNING) vfprintf(LogFile, fmt, va_args); } while (0)
#define LogWarningW(fmt, ...) \
	do { if (gLogVerbosity >= LogVerbosity::WARNING) fwprintf(LogFile, fmt, __VA_ARGS__); } while (0)
#define vLogWarningW(fmt, va_args) \
	do { if (gLogVerbosity >= LogVerbosity::WARNING) vfwprintf(LogFile, fmt, va_args); } while (0)

#define LogInfo(fmt, ...) \
	do { if (gLogVerbosity >= LogVerbosity::INFO) fprintf(LogFile, fmt, __VA_ARGS__); } while (0)
#define vLogInfo(fmt, va_args) \
	do { if (gLogVerbosity >= LogVerbosity::INFO) vfprintf(LogFile, fmt, va_args); } while (0)
#define LogInfoW(fmt, ...) \
	do { if (gLogVerbosity >= LogVerbosity::INFO) fwprintf(LogFile, fmt, __VA_ARGS__); } while (0)
#define vLogInfoW(fmt, va_args) \
	do { if (gLogVerbosity >= LogVerbosity::INFO) vfwprintf(LogFile, fmt, va_args); } while (0)

#define LogDebug(fmt, ...) \
	do { if (gLogVerbosity >= LogVerbosity::DEBUG) fprintf(LogFile, fmt, __VA_ARGS__); } while (0)
#define vLogDebug(fmt, va_args) \
	do { if (gLogVerbosity >= LogVerbosity::DEBUG) vfprintf(LogFile, fmt, va_args); } while (0)
#define LogDebugW(fmt, ...) \
	do { if (gLogVerbosity >= LogVerbosity::DEBUG) fwprintf(LogFile, fmt, __VA_ARGS__); } while (0)
#define vLogDebugW(fmt, va_args) \
	do { if (gLogVerbosity >= LogVerbosity::DEBUG) vfwprintf(LogFile, fmt, va_args); } while (0)

// Aliases for the above functions that we use to denote that omitting the
// newline was done intentionally. For now this is just for our reference, but
// later we might actually make the default function insert a newline:
#define LogInfoNoNL LogInfo
#define LogInfoWNoNL LogInfoW
#define LogDebugNoNL LogDebug
#define LogDebugWNoNL LogDebugW

static string LogTime()
{
	string timeStr;
	char cTime[32];
	tm timestruct;

	time_t ltime = time(0);
	localtime_s(&timestruct, &ltime);
	asctime_s(cTime, sizeof(cTime), &timestruct);

	timeStr = cTime;
	return timeStr;
}

