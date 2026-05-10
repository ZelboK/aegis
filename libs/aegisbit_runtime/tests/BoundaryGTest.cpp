//===-- BoundaryGTest.cpp - runtime boundary tests ------------*- C++ -*-===//

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

TEST(AegisbitRuntimeBoundaryTest, RuntimeDoesNotUseOldCoordinator) {
  const std::filesystem::path root = NEW_AEGIS_AEGISBIT_RUNTIME_DIR;
  const std::vector<std::string> forbidden = {
      "ProfilingRuntime", "RuntimeConfig", "BinaryRewriter",
      "#include \"llvm/", "#include <llvm/", "llvm::", "<hsa/"};

  for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
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
