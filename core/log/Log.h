/* The core's log: straight to the frontend's (libretro.cpp's log_cb), with
 * nothing in between. Each format ends with its own "\n"
 * (tools/log/log_check.py). INFO_LOG and DEBUG_LOG are there in debug
 * builds only. (The first argument, the part of the machine logging, is
 * for the source's reader.) */
#pragma once

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NULL without a frontend's log */
extern retro_log_printf_t log_cb;

#ifdef __cplusplus
}
#endif

#define CORE_LOG(level, ...) \
	do \
	{ \
		if (log_cb) \
			log_cb(level, __VA_ARGS__); \
	} while (0)

#define ERROR_LOG(t, ...) CORE_LOG(RETRO_LOG_ERROR, __VA_ARGS__)
#define WARN_LOG(t, ...) CORE_LOG(RETRO_LOG_WARN, __VA_ARGS__)
#define NOTICE_LOG(t, ...) CORE_LOG(RETRO_LOG_INFO, __VA_ARGS__)
#if !defined(NDEBUG) || defined(DEBUGFAST)
#define INFO_LOG(t, ...) CORE_LOG(RETRO_LOG_INFO, __VA_ARGS__)
#define DEBUG_LOG(t, ...) CORE_LOG(RETRO_LOG_DEBUG, __VA_ARGS__)
#else
/* (seen by the compiler, so what they log is still used, and left out) */
#define INFO_LOG(t, ...) do { if (0) log_cb(RETRO_LOG_INFO, __VA_ARGS__); } while (0)
#define DEBUG_LOG(t, ...) do { if (0) log_cb(RETRO_LOG_DEBUG, __VA_ARGS__); } while (0)
#endif
