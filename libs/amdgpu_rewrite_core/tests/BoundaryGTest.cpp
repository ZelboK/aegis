//===-- BoundaryGTest.cpp - rewrite core boundary tests --------*- C++ -*-===//

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void expectNoTokens(const std::filesystem::path &root,
                    const std::vector<std::string> &tokens) {
  for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string contents = readFile(entry.path());
    for (const auto &token : tokens) {
      EXPECT_EQ(contents.find(token), std::string::npos)
          << entry.path() << " contains forbidden token " << token;
    }
  }
}

} // namespace

TEST(AmdgpuRewriteCoreBoundaryTest, PublicApiAvoidsRuntimeHsaOutputAndLlvm) {
  const std::filesystem::path root = NEW_AEGIS_AMDGPU_REWRITE_CORE_DIR;
  expectNoTokens(root / "include",
                 {"#include \"llvm/", "#include <llvm/", "llvm::",
                  "hsa_intercept", "<hsa/", "ProfilingRuntime",
                  "RuntimeConfig", "aegisbit_output", "aegisbit_runtime"});
}

TEST(AmdgpuRewriteCoreBoundaryTest, ImplementationAvoidsObsoleteStrategies) {
  const std::filesystem::path root = NEW_AEGIS_AMDGPU_REWRITE_CORE_DIR;
  expectNoTokens(root / "src",
                 {"SwapPC", "Adaptive", "RelayEmitter", "SharedBodyStrategy",
                  "SwapPCSharedBody", "ClobberAuditor", "ForceRelay",
                  "MinimalRelay", "NopRelay", "VccOnlyRelay"});
}
