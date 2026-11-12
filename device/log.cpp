#include "device/log.h"

#include <spdlog/sinks/stdout_color_sinks.h>

namespace nr {

void InitLogging(bool to_stderr) {
    auto logger = to_stderr ? spdlog::stderr_color_mt("nr") : spdlog::stdout_color_mt("nr");
    logger->set_pattern("%H:%M:%S.%e %^%l%$ %v");
    spdlog::set_default_logger(logger);
}

nri::CallbackInterface NriLogCallback() {
    nri::CallbackInterface callbacks = {};
    callbacks.MessageCallback = [](nri::Message type, const char* file, uint32_t line, const char* message, void*) {
        const spdlog::level::level_enum level = type == nri::Message::ERROR     ? spdlog::level::err
                                                : type == nri::Message::WARNING ? spdlog::level::warn
                                                                                : spdlog::level::info;
        spdlog::log(level, "NRI: {} ({}:{})", message, file ? file : "", line);
    };
    return callbacks;
}

}  // namespace nr
