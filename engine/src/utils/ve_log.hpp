#pragma once

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <ctime>
#include <filesystem>

namespace ve { namespace log {

enum Level {
	Error = 0,
	Warn  = 1,
	Info  = 2,
	Debug = 3,
};

}} // namespace ve { namespace log {

#ifndef VE_LOG_LEVEL
#  ifndef NDEBUG
#    define VE_LOG_LEVEL ve::log::Debug
#  else
#    define VE_LOG_LEVEL ve::log::Warn
#  endif
#endif

#ifndef VE_LOG_ENABLE_COLOR
#  define VE_LOG_ENABLE_COLOR 1
#endif

namespace ve { namespace detail {

inline const char* level_to_str(int lvl) {
	switch (lvl) {
		case log::Error: return "ERROR";
		case log::Warn:  return "WARN";
		case log::Info:  return "INFO";
		case log::Debug: return "DEBUG";
		default:         return "LOG";
	}
}

inline const char* level_to_color(int lvl) {

#if VE_LOG_ENABLE_COLOR
	if (std::getenv("NO_COLOR")) return "";
	switch (lvl) {
		case log::Error: return "\033[31m"; // red
		case log::Warn:  return "\033[33m"; // yellow
		case log::Info:  return "\033[36m"; // cyan
		case log::Debug: return "\033[90m"; // gray
		default:         return "";
	}
#else
        (void)lvl; return "";
#endif

}

inline const char* reset_color() {
#if VE_LOG_ENABLE_COLOR
	if (std::getenv("NO_COLOR")) return "";
	return "\033[0m";
#else
	return "";
#endif
}

inline void log_line(int lvl, const char* file, int line, const std::string& msg) {
	if (lvl > VE_LOG_LEVEL) return;
	// Optional timestamp
	std::time_t now = std::time(nullptr);
	std::tm* local_tm = std::localtime(&now);
	char timestamp[32] = {0};
	if (local_tm) {
		std::strftime(timestamp, sizeof(timestamp), "%H:%M:%S", local_tm);
	}
	std::fprintf(stderr, "%s[%s]%s %s %s:%d: %s\n",
			level_to_color(lvl), level_to_str(lvl), reset_color(), timestamp, file, line, msg.c_str());
}

}} // namespace ve { namespace detail {

#define VE_LOG_IMPL(LVL, EXPR) do { \
    if ((LVL) <= VE_LOG_LEVEL) { \
        std::ostringstream _ve_log_oss; \
        _ve_log_oss << EXPR; \
		std::filesystem::path p = std::filesystem::path(__FILE__).filename(); \
        std::string filename = p.string(); \
        ::ve::detail::log_line((LVL), filename.c_str(), __LINE__, _ve_log_oss.str()); \
    } \
} while(0)

#define VE_LOGE(EXPR) VE_LOG_IMPL(::ve::log::Error, EXPR)
#define VE_LOGW(EXPR) VE_LOG_IMPL(::ve::log::Warn,  EXPR)
#define VE_LOGI(EXPR) VE_LOG_IMPL(::ve::log::Info,  EXPR)
#define VE_LOGD(EXPR) VE_LOG_IMPL(::ve::log::Debug, EXPR)
