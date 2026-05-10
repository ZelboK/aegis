//===-- RuntimeArtifactSmokeGTest.cpp - real parser smoke test --*- C++ -*-===//

#include "aegisbit_runtime/Runtime.h"
#include "amdgpu_code_object_llvm/LlvmCodeObjectParser.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

class MockBackend final : public amdgpu_instr_backend::InstructionBackend {
public:
  const amdgpu_instr_backend::OpcodeInfo &opcodes() const override {
    return opcodes_;
  }

  amdgpu_instr_backend::Result<amdgpu_instr_backend::Instruction>
  decode(amdgpu_instr_backend::ByteView, uint64_t) const override {
    return amdgpu_instr_backend::Result<
        amdgpu_instr_backend::Instruction>::failure("not implemented");
  }

  amdgpu_instr_backend::Result<std::vector<amdgpu_instr_backend::Instruction>>
  decodeAll(amdgpu_instr_backend::ByteView, uint64_t baseAddress) const override {
    amdgpu_instr_backend::Instruction branch;
    branch.BackendOpcode = 1;
    branch.Address = baseAddress;
    branch.Size = 4;
    branch.IsPCRelativeBranch = true;
    branch.Operands.push_back(amdgpu_instr_backend::Operand::imm(1));
    return amdgpu_instr_backend::Result<
        std::vector<amdgpu_instr_backend::Instruction>>::success({branch});
  }

  amdgpu_instr_backend::Result<std::vector<uint8_t>>
  encode(const amdgpu_instr_backend::Instruction &) const override {
    return amdgpu_instr_backend::Result<std::vector<uint8_t>>::failure(
        "not implemented");
  }

  amdgpu_instr_backend::Result<std::vector<uint8_t>>
  encodeSBranch(int16_t dwordOffset) const override {
    return amdgpu_instr_backend::Result<std::vector<uint8_t>>::success(
        {static_cast<uint8_t>(dwordOffset & 0xFF),
         static_cast<uint8_t>((dwordOffset >> 8) & 0xFF), 0x82, 0xBF});
  }

  amdgpu_instr_backend::Result<std::vector<uint8_t>> encodeNop() const override {
    return amdgpu_instr_backend::Result<std::vector<uint8_t>>::success(
        {0x00, 0x00, 0x80, 0xBF});
  }

  amdgpu_instr_backend::Result<uint64_t>
  branchTarget(const amdgpu_instr_backend::Instruction &inst,
               uint64_t currentPc) const override {
    return amdgpu_instr_backend::Result<uint64_t>::success(
        static_cast<uint64_t>(static_cast<int64_t>(currentPc + 4) +
                              inst.Operands.back().Immediate * 4));
  }

  const amdgpu_instr_backend::SgprPairInfo *
  getSgprPairInfo(unsigned) const override {
    return nullptr;
  }

private:
  amdgpu_instr_backend::OpcodeInfo opcodes_;
};

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
  bytes[4] = 2;
  bytes[5] = 1;
  bytes[6] = 1;
  write16(bytes, 16, 1);
  write16(bytes, 18, 0xe0);
  write32(bytes, 20, 1);
  write64(bytes, 40, sectionHeaderOffset);
  write32(bytes, 48, 0x04c);
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
              8);
  writeSymbol(bytes, symtabOffset + 2 * symbolSize, descriptorName, 0x11, 2,
              0x2000, 64);

  writeBytes(bytes, strtabOffset, strtab);
  writeBytes(bytes, shstrtabOffset, shstrtab);
  writeSectionHeader(bytes, sectionHeaderOffset + 64, textName, 1, 0x6, 0x1000,
                     textOffset, 16, 0, 0, 4, 0);
  writeSectionHeader(bytes, sectionHeaderOffset + 2 * 64, rodataName, 1, 0x2,
                     0x2000, rodataOffset, 64, 0, 0, 8, 0);
  writeSectionHeader(bytes, sectionHeaderOffset + 3 * 64, symtabName, 2, 0,
                     0, symtabOffset, 3 * symbolSize, 4, 1, 8, symbolSize);
  writeSectionHeader(bytes, sectionHeaderOffset + 4 * 64, strtabName, 3, 0, 0,
                     strtabOffset, strtab.size(), 0, 0, 1, 0);
  writeSectionHeader(bytes, sectionHeaderOffset + 5 * 64, shstrtabName, 3, 0,
                     0, shstrtabOffset, shstrtab.size(), 0, 0, 1, 0);
  return bytes;
}

aegis::hsa_intercept::CapturedKernelSymbol symbol() {
  aegis::hsa_intercept::CapturedKernelSymbol out;
  out.codeObjectId = 1;
  out.kernelName = "kernel";
  out.kernelObject = 0xABC;
  return out;
}

} // namespace

TEST(AegisbitRuntimeArtifactSmokeTest,
     GeneratedAmdgpuElfProducesArtifactBundle) {
  MockBackend backend;
  auto parser = amdgpu_code_object_llvm::createLlvmCodeObjectParser();
  std::vector<aegisbit_output::ReproducerBundle> bundles;
  aegisbit_runtime::RuntimeCallbacks callbacks;
  callbacks.onArtifact =
      [&bundles](const aegisbit_output::ReproducerBundle &bundle) {
        bundles.push_back(bundle);
      };
  aegisbit_runtime::Runtime runtime({}, &backend, parser.get(), nullptr,
                                    callbacks);

  aegis::hsa_intercept::CapturedCodeObject object;
  object.codeObjectId = 1;
  object.bytes = makeMinimalAmdgpuElf();
  runtime.handleCodeObject(object);
  runtime.handleKernelSymbol(symbol());

  aegis::hsa_intercept::DispatchEvent event;
  event.originalKernelObject = 0xABC;
  event.packet.kernelObject = 0xABC;
  auto decision = runtime.handleDispatch(event);

  EXPECT_EQ(decision, aegis::hsa_intercept::DispatchDecision::proceed);
  ASSERT_EQ(bundles.size(), 1u);
  EXPECT_EQ(bundles[0].files[0].name, "original_code_object.bin");
  EXPECT_EQ(bundles[0].files[1].name, "patched_code_object.bin");
  auto rewrite = runtime.lastRewriteResult();
  ASSERT_TRUE(rewrite.has_value());
  EXPECT_EQ(rewrite->trace.kernel.metadataSource, "parser");
  EXPECT_EQ(rewrite->trace.kernel.textOffset, 0x100u);
  EXPECT_EQ(runtime.dispatchTraces()[0].status, "rewritten-artifact");
}
