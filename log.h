
#pragma once
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
    #include <string>
#endif

#define LOG_SOURCE "GC"

#define LOG_LEVEL_DEBUG   0
#define LOG_LEVEL_INFO    1
#define LOG_LEVEL_WARNING 2
#define LOG_LEVEL_NOTICE  3
#define LOG_LEVEL_ERROR   4 
#define LOG_LEVEL_FATAL   5

#define LOG_COLOR_RESET  "\033[0m"
#define LOG_COLOR_RED    "\033[31m"
#define LOG_COLOR_GREEN  "\033[32m"
#define LOG_COLOR_YELLOW "\033[33m"
#define LOG_COLOR_BLUE   "\033[34m"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
static const char* _log_levels[] = {
    "DEBUG",
    "INFO",
    "WARNING",
    "NOTICE",
    "ERROR",
    "FATAL"
};

static const char* _log_colors[] = {
    LOG_COLOR_BLUE,
    LOG_COLOR_GREEN,
    LOG_COLOR_YELLOW,
    LOG_COLOR_YELLOW,
    LOG_COLOR_RED,
    LOG_COLOR_RED
};
#pragma GCC diagnostic pop

#define LOG_STDOUT stdout
#define LOG_STDERR stderr

#define LOG_PRINT(file,level,fmt,...) do { \
        fprintf(file, "%s[%s %s]%s: ", _log_colors[level], LOG_SOURCE, _log_levels[level], LOG_COLOR_RESET); \
        fprintf(file, fmt, ##__VA_ARGS__); \
        fprintf(file, "\n"); \
    } while (0)

#if !defined DEBUG
#define LOG_DEBUG(fmt,...) ((void)0)
#else
#define LOG_DEBUG(fmt,...) LOG_PRINT(LOG_STDOUT, LOG_LEVEL_DEBUG  , fmt, ##__VA_ARGS__)
#endif
#define LOG_INFO(fmt,...)  LOG_PRINT(LOG_STDOUT, LOG_LEVEL_INFO   , fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt,...)  LOG_PRINT(LOG_STDOUT, LOG_LEVEL_WARNING, fmt, ##__VA_ARGS__)
#define LOG_NOTE(fmt,...)  LOG_PRINT(LOG_STDERR, LOG_LEVEL_NOTICE , fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt,...) LOG_PRINT(LOG_STDERR, LOG_LEVEL_ERROR  , fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt,...) LOG_PRINT(LOG_STDERR, LOG_LEVEL_FATAL  , fmt, ##__VA_ARGS__); exit(1)

#define LOG_INT(v)    LOG_DEBUG(#v " = %d (%s:%d)",   v, __FILE__, __LINE__)
#define LOG_UINT(v)   LOG_DEBUG(#v " = %u (%s:%d)",   v, __FILE__, __LINE__)
#define LOG_LONG(v)   LOG_DEBUG(#v " = %ld (%s:%d)",  v, __FILE__, __LINE__)
#define LOG_ULONG(v)  LOG_DEBUG(#v " = %lu (%s:%d)",  v, __FILE__, __LINE__)
#define LOG_FLOAT(v)  LOG_DEBUG(#v " = %f (%s:%d)",   v, __FILE__, __LINE__)
#define LOG_DOUBLE(v) LOG_DEBUG(#v " = %lf (%s:%d)",  v, __FILE__, __LINE__)
#define LOG_STRING(v) LOG_DEBUG(#v " = %s (%s:%d)",   v, __FILE__, __LINE__)
#define LOG_BOOL(v)   LOG_DEBUG(#v " = %s (%s:%d)",   v ? "true" : "false", __FILE__, __LINE__)
#define LOG_HEX(v)    LOG_DEBUG(#v " = 0x%x (%s:%d)", v, __FILE__, __LINE__)
#define LOG_PTR(v)    LOG_DEBUG(#v " = %p (%s:%d)",   v, __FILE__, __LINE__)

#ifdef __cplusplus
    #define LOG_WATCH(v) do { \
        LOG_DEBUG(#v " = %s (%s:%d)", std::to_string(v).c_str(), __FILE__, __LINE__); \
        } while (0)
#endif
