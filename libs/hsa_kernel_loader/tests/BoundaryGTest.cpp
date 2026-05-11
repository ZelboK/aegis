//===-- BoundaryGTest.cpp - loader boundary tests --------------*- C++ -*-===//

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

} // namespace

TEST(HsaKernelLoaderBoundaryTest, PublicHeadersAvoidRewriteAndConcreteHsaDeps) {
  const std::filesystem::path root = NEW_AEGIS_HSA_KERNEL_LOADER_DIR;
  const std::vector<std::string> forbidden = {
      "#include \"llvm/", "#include <llvm/", "llvm::", "amdgpu_rewrite_core",
      "aegisbit_runtime", "<hsa/"};

  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(root / "include")) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto path = entry.path();
    if (path.string().find("/tests/") != std::string::npos) {
      continue;
    }
    std::string contents = readFile(path);
    for (const auto &token : forbidden) {
      EXPECT_EQ(contents.find(token), std::string::npos)
          << path << " contains forbidden token " << token;
    }
  }
}

TEST(HsaKernelLoaderBoundaryTest, ImplementationAvoidsRewriteAndOldRuntime) {
  const std::filesystem::path root = NEW_AEGIS_HSA_KERNEL_LOADER_DIR;
  const std::vector<std::string> forbidden = {
      "amdgpu_rewrite_core", "aegisbit_runtime", "ProfilingRuntime",
      "BinaryRewriter", "LoadedKernelCache"};

  const std::filesystem::path src = root / "src";
  if (!std::filesystem::exists(src)) {
    GTEST_SKIP() << "loader implementation source not present";
  }

  for (const auto &entry : std::filesystem::recursive_directory_iterator(src)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string contents = readFile(entry.path());
    for (const auto &token : forbidden) {
      EXPECT_EQ(contents.find(token), std::string::npos)
          << entry.path() << " contains forbidden token " << token;
    }
  }
}
