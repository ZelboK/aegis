//===-- BoundaryGTest.cpp - AMDGPU PC reloc boundary tests -----*- C++ -*-===//

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string readFile(const std::filesystem::path &Path) {
  std::ifstream Input(Path);
  return std::string(std::istreambuf_iterator<char>(Input),
                     std::istreambuf_iterator<char>());
}

void expectNoForbiddenTokens(const std::filesystem::path &Root,
                             const std::vector<std::string> &Forbidden) {
  for (const auto &Entry : std::filesystem::recursive_directory_iterator(Root)) {
    if (!Entry.is_regular_file()) {
      continue;
    }
    std::string Contents = readFile(Entry.path());
    for (const std::string &Token : Forbidden) {
      EXPECT_EQ(Contents.find(Token), std::string::npos)
          << Entry.path() << " contains forbidden token " << Token;
    }
  }
}

} // namespace

TEST(AmdgpuPCRelocBoundaryTest, FrontendHasNoAegisRocmOrLlvmDependencies) {
  const std::filesystem::path Root = NEW_AEGIS_AMDGPU_PC_RELOC_DIR;
  const std::vector<std::string> Forbidden = {
      "llvm/", "llvm::", "aegis/", "aegisbit/", "<hsa/", "rocprofiler"};

  expectNoForbiddenTokens(Root / "include", Forbidden);
  expectNoForbiddenTokens(Root / "src", Forbidden);
}

TEST(AmdgpuPCRelocBoundaryTest, BackendInterfaceHasNoImplementationDeps) {
  const std::filesystem::path Root = NEW_AEGIS_AMDGPU_INSTR_BACKEND_DIR;
  const std::vector<std::string> Forbidden = {
      "llvm/", "llvm::", "aegis/", "aegisbit/", "<hsa/", "rocprofiler",
      "amdgpu_instr_backend_llvm"};

  expectNoForbiddenTokens(Root / "include", Forbidden);
}
