//===-- BoundaryGTest.cpp - code object boundary tests ----------*- C++ -*-===//

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

} // namespace

TEST(AmdgpuCodeObjectBoundaryTest, PublicHeadersAvoidLlvmTypes) {
  const std::filesystem::path root = NEW_AEGIS_AMDGPU_CODE_OBJECT_DIR;
  const std::filesystem::path includeDir = root / "include";
  const std::vector<std::string> forbidden = {
      "#include \"llvm/", "#include <llvm/", "llvm::", "MCInst"};

  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(includeDir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".h") {
      continue;
    }

    const std::string contents = readFile(entry.path());
    for (const auto &token : forbidden) {
      EXPECT_EQ(contents.find(token), std::string::npos)
          << entry.path() << " contains forbidden token " << token;
    }
  }
}
