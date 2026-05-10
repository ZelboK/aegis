//===-- BoundaryGTest.cpp - output boundary tests -------------*- C++ -*-===//

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

TEST(AegisbitOutputBoundaryTest, OutputAvoidsRuntimeHsaAndLlvmDeps) {
  const std::filesystem::path root = NEW_AEGIS_AEGISBIT_OUTPUT_DIR;
  const std::vector<std::string> forbidden = {
      "#include \"llvm/", "#include <llvm/", "llvm::", "hsa_intercept",
      "<hsa/", "ProfilingRuntime", "RuntimeConfig"};

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
