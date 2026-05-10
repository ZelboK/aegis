//===-- LlvmCodeObjectParserGTest.cpp - LLVM code-object tests --*- C++ -*-===//

#include "amdgpu_code_object_llvm/LlvmCodeObjectParser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void ensureSize(std::vector<uint8_t> &bytes, uint64_t size) {
  if (bytes.size() < size) {
    bytes.resize(size, 0);
  }
}

void write8(std::vector<uint8_t> &bytes, uint64_t offset, uint8_t value) {
  ensureSize(bytes, offset + 1);
  bytes[offset] = value;
}

void write16(std::vector<uint8_t> &bytes, uint64_t offset, uint16_t value) {
  ensureSize(bytes, offset + 2);
  bytes[offset] = static_cast<uint8_t>(value & 0xff);
  bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void write32(std::vector<uint8_t> &bytes, uint64_t offset, uint32_t value) {
  ensureSize(bytes, offset + 4);
  for (uint64_t i = 0; i < 4; ++i) {
    bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
  }
}

void write64(std::vector<uint8_t> &bytes, uint64_t offset, uint64_t value) {
  ensureSize(bytes, offset + 8);
  for (uint64_t i = 0; i < 8; ++i) {
    bytes[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
  }
}

uint32_t appendString(std::string &table, const std::string &value) {
  const uint32_t offset = static_cast<uint32_t>(table.size());
  table.append(value);
  table.push_back('\0');
  return offset;
}

void writeBytes(std::vector<uint8_t> &bytes, uint64_t offset,
                const std::string &value) {
  ensureSize(bytes, offset + value.size());
  std::copy(value.begin(), value.end(), bytes.begin() + offset);
}

void writeSectionHeader(std::vector<uint8_t> &bytes, uint64_t offset,
                        uint32_t name, uint32_t type, uint64_t flags,
                        uint64_t address, uint64_t fileOffset, uint64_t size,
                        uint32_t link, uint32_t info, uint64_t align,
                        uint64_t entrySize) {
  write32(bytes, offset + 0, name);
  write32(bytes, offset + 4, type);
  write64(bytes, offset + 8, flags);
  write64(bytes, offset + 16, address);
  write64(bytes, offset + 24, fileOffset);
  write64(bytes, offset + 32, size);
  write32(bytes, offset + 40, link);
  write32(bytes, offset + 44, info);
  write64(bytes, offset + 48, align);
  write64(bytes, offset + 56, entrySize);
}

void writeSymbol(std::vector<uint8_t> &bytes, uint64_t offset, uint32_t name,
                 uint8_t info, uint16_t sectionIndex, uint64_t value,
                 uint64_t size) {
  write32(bytes, offset + 0, name);
  write8(bytes, offset + 4, info);
  write8(bytes, offset + 5, 0);
  write16(bytes, offset + 6, sectionIndex);
  write64(bytes, offset + 8, value);
  write64(bytes, offset + 16, size);
}

std::vector<uint8_t> makeMinimalAmdgpuElf() {
  constexpr uint64_t textOffset = 0x100;
  constexpr uint64_t rodataOffset = 0x110;
  constexpr uint64_t symtabOffset = 0x150;
  constexpr uint64_t strtabOffset = 0x1b0;
  constexpr uint64_t shstrtabOffset = 0x1d0;
  constexpr uint64_t sectionHeaderOffset = 0x200;
  constexpr uint64_t symbolSize = 24;

  std::string strtab;
  strtab.push_back('\0');
  const uint32_t kernelName = appendString(strtab, "kernel");
  const uint32_t otherName = appendString(strtab, "other");
  const uint32_t descriptorName = appendString(strtab, "kernel.kd");

  std::string shstrtab;
  shstrtab.push_back('\0');
  const uint32_t textName = appendString(shstrtab, ".text");
  const uint32_t rodataName = appendString(shstrtab, ".rodata");
  const uint32_t symtabName = appendString(shstrtab, ".symtab");
  const uint32_t strtabName = appendString(shstrtab, ".strtab");
  const uint32_t shstrtabName = appendString(shstrtab, ".shstrtab");

  std::vector<uint8_t> bytes(sectionHeaderOffset + 6 * 64, 0);
  bytes[0] = 0x7f;
  bytes[1] = 'E';
  bytes[2] = 'L';
  bytes[3] = 'F';
  bytes[4] = 2; // ELFCLASS64
  bytes[5] = 1; // ELFDATA2LSB
  bytes[6] = 1; // EV_CURRENT
  write16(bytes, 16, 1); // ET_REL
  write16(bytes, 18, 0xe0); // EM_AMDGPU
  write32(bytes, 20, 1); // EV_CURRENT
  write64(bytes, 40, sectionHeaderOffset);
  write32(bytes, 48, 0x04c); // gfx942
  write16(bytes, 52, 64);
  write16(bytes, 58, 64);
  write16(bytes, 60, 6);
  write16(bytes, 62, 5);

  for (uint64_t i = 0; i < 16; ++i) {
    bytes[textOffset + i] = static_cast<uint8_t>(0xc0 + i);
  }
  for (uint64_t i = 0; i < 64; ++i) {
    bytes[rodataOffset + i] = static_cast<uint8_t>(0x40 + i);
  }

  writeSymbol(bytes, symtabOffset + symbolSize, kernelName, 0x12, 1, 0x1000,
              0);
  writeSymbol(bytes, symtabOffset + 2 * symbolSize, otherName, 0x12, 1,
              0x1008, 8);
  writeSymbol(bytes, symtabOffset + 3 * symbolSize, descriptorName, 0x11, 2,
              0x2000, 64);

  writeBytes(bytes, strtabOffset, strtab);
  writeBytes(bytes, shstrtabOffset, shstrtab);

  writeSectionHeader(bytes, sectionHeaderOffset + 64, textName, 1, 0x6, 0x1000,
                     textOffset, 16, 0, 0, 4, 0);
  writeSectionHeader(bytes, sectionHeaderOffset + 2 * 64, rodataName, 1, 0x2,
                     0x2000, rodataOffset, 64, 0, 0, 8, 0);
  writeSectionHeader(bytes, sectionHeaderOffset + 3 * 64, symtabName, 2, 0,
                     0, symtabOffset, 4 * symbolSize, 4, 1, 8, symbolSize);
  writeSectionHeader(bytes, sectionHeaderOffset + 4 * 64, strtabName, 3, 0, 0,
                     strtabOffset, strtab.size(), 0, 0, 1, 0);
  writeSectionHeader(bytes, sectionHeaderOffset + 5 * 64, shstrtabName, 3, 0,
                     0, shstrtabOffset, shstrtab.size(), 0, 0, 1, 0);
  return bytes;
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

} // namespace

TEST(AmdgpuCodeObjectLlvmTest, ParsesKernelTextAndDescriptorFacts) {
  auto parser = amdgpu_code_object_llvm::createLlvmCodeObjectParser();
  amdgpu_code_object::ParseRequest request;
  request.bytes = makeMinimalAmdgpuElf();
  request.kernelName = "kernel";

  auto parsed = parser->parseKernel(request);
  ASSERT_TRUE(parsed) << parsed.error();
  EXPECT_EQ(parsed.value().name, "kernel");
  EXPECT_EQ(parsed.value().arch, "gfx942");
  EXPECT_EQ(parsed.value().entryPc, 0x1000u);
  EXPECT_EQ(parsed.value().textBase, 0x1000u);
  EXPECT_EQ(parsed.value().textOffset, 0x100u);
  EXPECT_EQ(parsed.value().textSize, 8u);
  ASSERT_TRUE(parsed.value().descriptor.has_value());
  EXPECT_EQ(parsed.value().descriptor->fileOffset, 0x110u);
  EXPECT_EQ(parsed.value().descriptor->size, 64u);
}

TEST(AmdgpuCodeObjectLlvmTest, AcceptsDescriptorSuffixedKernelName) {
  auto parser = amdgpu_code_object_llvm::createLlvmCodeObjectParser();
  amdgpu_code_object::ParseRequest request;
  request.bytes = makeMinimalAmdgpuElf();
  request.kernelName = "kernel.kd";

  auto parsed = parser->parseKernel(request);
  ASSERT_TRUE(parsed) << parsed.error();
  EXPECT_EQ(parsed.value().name, "kernel");
}

TEST(AmdgpuCodeObjectLlvmBoundaryTest, PublicHeadersAvoidLlvmTypes) {
  const std::filesystem::path root = NEW_AEGIS_AMDGPU_CODE_OBJECT_LLVM_DIR;
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
