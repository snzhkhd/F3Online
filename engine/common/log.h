#pragma once
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace f3::log {

inline void Write(const char* level, const char* fmt, va_list ap) {
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::fprintf(stdout, "[%02d:%02d:%02d] %-5s %s\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, level, msg);
    std::fflush(stdout);
}
inline void Info(const char* fmt, ...) { va_list ap; va_start(ap, fmt); Write("INFO", fmt, ap); va_end(ap); }
inline void Warn(const char* fmt, ...) { va_list ap; va_start(ap, fmt); Write("WARN", fmt, ap); va_end(ap); }
inline void Error(const char* fmt, ...) { va_list ap; va_start(ap, fmt); Write("ERROR", fmt, ap); va_end(ap); }

}  // namespace f3::log
