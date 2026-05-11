//===-- HsaKernelLoaderHsaSmokeGTest.cpp - guarded HSA smoke ----*- C++ -*-===//

#include "hsa_kernel_loader/HsaKernelLoader.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace {

bool runSmoke() {
  const char *value = std::getenv("AEGIS_RUN_HSA_LOADER_SMOKE");
  return value && (std::string(value) == "1" || std::string(value) == "true");
}

} // namespace

TEST(HsaKernelLoaderHsaSmokeTest, CreatesLoaderForDefaultGpuAgent) {
  if (!runSmoke()) {
    GTEST_SKIP() << "set AEGIS_RUN_HSA_LOADER_SMOKE=1 to run guarded HSA smoke";
  }

  const uint64_t agent = hsa_kernel_loader::defaultGpuAgent();
  ASSERT_NE(agent, 0u);
  auto loader = hsa_kernel_loader::createHsaKernelLoader(agent);
  ASSERT_TRUE(loader) << loader.error();
  ASSERT_NE(loader.value(), nullptr);
}
