/* Copyright 2009 Dolphin Emulator Project
 * Licensed under GPLv2+
 * Refer to the license.txt file included. */

#pragma once

/* The kinds and levels, LogTypes:: in C++ and LOG_ in C - names like
 * INPUT are taken there (the macros below name them either way) */
#ifdef __cplusplus
namespace LogTypes
{
#define LOG_ENUM(x) x
#define LOG_TYPE_OF(x) LogTypes::x
#define LOG_TYPE_NAME(x) LogTypes::x
#else
#define LOG_ENUM(x) LOG_##x
#define LOG_TYPE_OF(x) LOG_##x
#define LOG_TYPE_NAME(x) x
#endif
enum LOG_TYPE
{
	LOG_ENUM(AICA),
	LOG_ENUM(AICA_ARM),
	LOG_ENUM(AUDIO),
	LOG_ENUM(BOOT),
	LOG_ENUM(COMMON),
	LOG_ENUM(DYNAREC),
	LOG_ENUM(FLASHROM),
	LOG_ENUM(GDROM),
	LOG_ENUM(HOLLY),
	LOG_ENUM(INPUT),
	LOG_ENUM(JVS),
	LOG_ENUM(MAPLE),
	LOG_ENUM(INTERPRETER),
	LOG_ENUM(MEMORY),
	LOG_ENUM(NETWORK),
	LOG_ENUM(VMEM),
	LOG_ENUM(MODEM),
	LOG_ENUM(NAOMI),
	LOG_ENUM(PVR),
	LOG_ENUM(REIOS),
	LOG_ENUM(RENDERER),
	LOG_ENUM(SAVESTATE),
	LOG_ENUM(SH4),

	LOG_ENUM(NUMBER_OF_LOGS)  /* Must be last */
};

enum LOG_LEVELS
{
	LOG_ENUM(LNOTICE) = 1,   /* VERY important information that is NOT errors. Like startup and OSReports. */
	LOG_ENUM(LERROR) = 2,    /* Critical errors */
	LOG_ENUM(LWARNING) = 3,  /* Something is suspicious. */
	LOG_ENUM(LINFO) = 4,     /* General information. */
	LOG_ENUM(LDEBUG) = 5     /* Detailed debugging - might make things slow. */
};

static const char LOG_LEVEL_TO_CHAR[7] = "-NEWID";

#ifdef __cplusplus
}  /* namespace LogTypes */
extern "C" {
#endif

void GenericLog(enum LOG_TYPE_NAME(LOG_LEVELS) level, enum LOG_TYPE_NAME(LOG_TYPE) type, const char* file, int line,
		const char* fmt, ...)
#if defined(__GNUC__) && !defined(__MINGW32__)
__attribute__((format(printf, 5, 6)))
#endif
;

#ifdef __cplusplus
}
#endif

#if !defined(NDEBUG) || defined(DEBUGFAST)
#define MAX_LOGLEVEL LOG_TYPE_OF(LDEBUG)
#else
#ifndef MAX_LOGLEVEL
#define MAX_LOGLEVEL LOG_TYPE_OF(LWARNING)
#endif  /* loglevel */
#endif  /* logging */

/* Let the compiler optimize this out */
#define GENERIC_LOG(t, v, ...)                                                                     \
		do                                                                                               \
		{                                                                                                \
			if (v <= MAX_LOGLEVEL)                                                                         \
			GenericLog(v, t, __FILE__, __LINE__, __VA_ARGS__);                                           \
		} while (0)

#define ERROR_LOG(t, ...)                                                                          \
		do                                                                                               \
		{                                                                                                \
			GENERIC_LOG(LOG_TYPE_OF(t), LOG_TYPE_OF(LERROR), __VA_ARGS__);                                 \
		} while (0)
#define WARN_LOG(t, ...)                                                                           \
		do                                                                                               \
		{                                                                                                \
			GENERIC_LOG(LOG_TYPE_OF(t), LOG_TYPE_OF(LWARNING), __VA_ARGS__);                               \
		} while (0)
#define NOTICE_LOG(t, ...)                                                                         \
		do                                                                                               \
		{                                                                                                \
			GENERIC_LOG(LOG_TYPE_OF(t), LOG_TYPE_OF(LNOTICE), __VA_ARGS__);                                \
		} while (0)
#define INFO_LOG(t, ...)                                                                           \
		do                                                                                               \
		{                                                                                                \
			GENERIC_LOG(LOG_TYPE_OF(t), LOG_TYPE_OF(LINFO), __VA_ARGS__);                                  \
		} while (0)
#define DEBUG_LOG(t, ...)                                                                          \
		do                                                                                               \
		{                                                                                                \
			GENERIC_LOG(LOG_TYPE_OF(t), LOG_TYPE_OF(LDEBUG), __VA_ARGS__);                                 \
		} while (0)
