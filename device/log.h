// Logging of the native code: spdlog's default logger, with NRI's messages routed into it.
//
//   nr::InitLogging(/*to_stderr=*/false);
//   spdlog::info("{}: {} MB", name, megabytes);
//   desc.callbackInterface = nr::NriLogCallback();  // any NRI device creation desc

#pragma once

#include <spdlog/spdlog.h>

#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

namespace nr {

// A colored console logger; to_stderr when stdout carries a program's data.
void InitLogging(bool to_stderr);

nri::CallbackInterface NriLogCallback();

}  // namespace nr
