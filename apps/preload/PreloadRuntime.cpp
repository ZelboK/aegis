//===-- PreloadRuntime.cpp - libaegis preload composition -------*- C++ -*-===//

#include "aegis/hsa_intercept/HsaIntercept.h"

#include <iostream>
#include <utility>

namespace {

void logMessage(const std::string& message) {
  std::cerr << "aegis: " << message << '\n';
}

} // namespace

extern "C" void aegis_preload_initialize() {
  aegis::hsa_intercept::Callbacks callbacks;
  callbacks.log = logMessage;
  callbacks.onDispatch = [](aegis::hsa_intercept::DispatchEvent&) {
    return aegis::hsa_intercept::DispatchDecision::proceed;
  };

  auto status = aegis::hsa_intercept::install({}, std::move(callbacks));
  if (!status.ok()) {
    logMessage(status.message());
  }
}

extern "C" void aegis_preload_finalize() {
  aegis::hsa_intercept::uninstall();
}

__attribute__((constructor)) static void aegisPreloadConstructor() {
  aegis_preload_initialize();
}

__attribute__((destructor)) static void aegisPreloadDestructor() {
  aegis_preload_finalize();
}
