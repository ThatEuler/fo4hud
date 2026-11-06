#include "logging.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <system_error>
#include <unistd.h>

namespace {
std::string log_path(const char* log_name) {
    const char* state_home = std::getenv("XDG_STATE_HOME");
    if (state_home && state_home[0] == '/')
        return std::string(state_home) + "/fo4hud/" + log_name + ".log";
    const char* home = std::getenv("HOME");
    if (home && home[0] == '/')
        return std::string(home) + "/.local/state/fo4hud/" + log_name + ".log";
    return "/tmp/fo4hud-" + std::string(log_name) + "-" + std::to_string(getuid()) + ".log";
}
}

bool fo4hud_aim_trace_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("FO4HUD_AIM_TRACE");
        return value && value[0] && value[0] != '0';
    }();
    return enabled;
}

void fo4hud_log(const char* log_name, const char* format, ...) {
    const std::string path = log_path(log_name);
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    int fd = error ? -1 : open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    FILE* file = fd >= 0 ? fdopen(fd, "a") : nullptr;
    if (!file) {
        if (fd >= 0) close(fd);
        file = stderr;
    }

    std::time_t now = std::time(nullptr);
    std::tm local{};
    char timestamp[32]{};
    if (localtime_r(&now, &local))
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &local);
    std::fprintf(file, "%s ", timestamp[0] ? timestamp : "unknown-time");
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    if (file != stderr) std::fclose(file);
}
