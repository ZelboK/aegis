//===-- HsaInterceptGTest.cpp - hsa_intercept tests ------------*- C++ -*-===//

#include "aegis/hsa_intercept/HsaIntercept.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace aegis::hsa_intercept;

struct TestApiTableVersion {
  uint32_t major_id;
  uint32_t minor_id;
  uint32_t step_id;
  uint32_t reserved;
};

struct TestHsaApiTable {
  TestApiTableVersion version;
  void* core_;
  void* amd_ext_;
  void* finalizer_ext_;
  void* image_ext_;
  void* tools_;
  void* pc_sampling_ext_;
};

struct TestKernelDispatchPacket {
  uint16_t header = 0;
  uint16_t setup = 0;
  uint16_t workgroupSizeX = 0;
  uint16_t workgroupSizeY = 0;
  uint16_t workgroupSizeZ = 0;
  uint16_t reserved0 = 0;
  uint32_t gridSizeX = 0;
  uint32_t gridSizeY = 0;
  uint32_t gridSizeZ = 0;
  uint32_t privateSegmentSize = 0;
  uint32_t groupSegmentSize = 0;
  uint64_t kernelObject = 0;
  uint64_t kernargAddress = 0;
  uint64_t reserved2 = 0;
  uint64_t completionSignal = 0;
};

static_assert(sizeof(TestKernelDispatchPacket) == 64);

std::vector<TestKernelDispatchPacket> writtenPackets;

void testWriter(const void* packets, uint64_t packetCount) {
  const auto* typedPackets =
      static_cast<const TestKernelDispatchPacket*>(packets);
  for (uint64_t i = 0; i < packetCount; ++i) {
    writtenPackets.push_back(typedPackets[i]);
  }
}

std::string readFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

class HsaInterceptApiTest : public ::testing::Test {
protected:
  void SetUp() override {
    uninstall();
    clearCallbacks();
    resetStats();
    writtenPackets.clear();
  }

  void TearDown() override {
    uninstall();
    clearCallbacks();
    resetStats();
    writtenPackets.clear();
  }
};

} // namespace

TEST(HsaInterceptLayoutTest, ApiTableVersionSize) {
  EXPECT_EQ(sizeof(TestApiTableVersion), 16u);
  EXPECT_EQ(alignof(TestApiTableVersion), 4u);
}

TEST(HsaInterceptLayoutTest, HsaApiTableLayout) {
  size_t expectedSize = sizeof(TestApiTableVersion) + 6 * sizeof(void*);
  EXPECT_EQ(sizeof(TestHsaApiTable), expectedSize);
}

TEST(HsaInterceptLayoutTest, AmdExtPointerOffset) {
  TestHsaApiTable table{};
  auto* tableBase = reinterpret_cast<uint8_t*>(&table);
  auto* amdExtAddr = reinterpret_cast<uint8_t*>(&table.amd_ext_);
  size_t expectedOffset = sizeof(TestApiTableVersion) + sizeof(void*);
  EXPECT_EQ(static_cast<size_t>(amdExtAddr - tableBase), expectedOffset);
}

TEST(HsaInterceptOffsetTest, QueueInterceptCreateOffset) {
  constexpr size_t expectedIndex = 37;
  constexpr size_t expectedOffset =
      sizeof(TestApiTableVersion) + (expectedIndex * sizeof(void*));
  if constexpr (sizeof(void*) == 8) {
    EXPECT_EQ(expectedOffset, 312u);
  } else {
    EXPECT_EQ(expectedOffset, 164u);
  }
}

TEST(HsaInterceptOffsetTest, QueueInterceptRegisterOffset) {
  constexpr size_t expectedIndex = 38;
  constexpr size_t expectedOffset =
      sizeof(TestApiTableVersion) + (expectedIndex * sizeof(void*));
  if constexpr (sizeof(void*) == 8) {
    EXPECT_EQ(expectedOffset, 320u);
  } else {
    EXPECT_EQ(expectedOffset, 168u);
  }
}

TEST_F(HsaInterceptApiTest, InitiallyNotInstalled) {
  EXPECT_FALSE(isInstalled());
}

TEST_F(HsaInterceptApiTest, StatsInitiallyZero) {
  auto currentStats = stats();
  EXPECT_EQ(currentStats.totalDispatches, 0u);
  EXPECT_EQ(currentStats.modifiedDispatches, 0u);
  EXPECT_EQ(currentStats.skippedDispatches, 0u);
  EXPECT_EQ(currentStats.errorDispatches, 0u);
}

TEST_F(HsaInterceptApiTest, SetAndClearCallbacks) {
  Callbacks callbacks;
  callbacks.onDispatch = [](DispatchEvent&) {
    return DispatchDecision::proceed;
  };
  setCallbacks(std::move(callbacks));
  clearCallbacks();
  SUCCEED();
}

TEST_F(HsaInterceptApiTest, RegisterNullQueueFails) {
  auto status = registerQueue(nullptr);
  EXPECT_FALSE(status.ok());
}

TEST_F(HsaInterceptApiTest, UninstallIsIdempotent) {
  uninstall();
  uninstall();
  uninstall();
  EXPECT_FALSE(isInstalled());
}

TEST_F(HsaInterceptApiTest, HandlePacketWritePassesThroughWithoutCallback) {
  TestKernelDispatchPacket packet;
  packet.header = 2;
  packet.kernelObject = 0x1234;
  packet.kernargAddress = 0x5678;

  handlePacketWrite(&packet, 1, 0, nullptr,
                    reinterpret_cast<void*>(&testWriter));

  ASSERT_EQ(writtenPackets.size(), 1u);
  EXPECT_EQ(writtenPackets[0].kernelObject, 0x1234u);
  EXPECT_EQ(writtenPackets[0].kernargAddress, 0x5678u);
  EXPECT_EQ(stats().totalDispatches, 1u);
}

TEST_F(HsaInterceptApiTest, HandlePacketWriteMutatesDispatchPacket) {
  Callbacks callbacks;
  callbacks.onDispatch = [](DispatchEvent& event) {
    event.packet.kernelObject = 0xABCDEF;
    event.packet.kernargAddress = 0x5555;
    return DispatchDecision::proceed;
  };
  setCallbacks(std::move(callbacks));

  TestKernelDispatchPacket packet;
  packet.header = 2;
  packet.kernelObject = 0x1234;
  packet.kernargAddress = 0x5678;

  handlePacketWrite(&packet, 1, 0, nullptr,
                    reinterpret_cast<void*>(&testWriter));

  ASSERT_EQ(writtenPackets.size(), 1u);
  EXPECT_EQ(writtenPackets[0].kernelObject, 0xABCDEFu);
  EXPECT_EQ(writtenPackets[0].kernargAddress, 0x5555u);
  EXPECT_EQ(stats().modifiedDispatches, 1u);
}

TEST_F(HsaInterceptApiTest, HandlePacketWriteSkipsDispatchPacket) {
  Callbacks callbacks;
  callbacks.onDispatch = [](DispatchEvent&) {
    return DispatchDecision::skip;
  };
  setCallbacks(std::move(callbacks));

  TestKernelDispatchPacket packet;
  packet.header = 2;
  packet.kernelObject = 0x1234;

  handlePacketWrite(&packet, 1, 0, nullptr,
                    reinterpret_cast<void*>(&testWriter));

  EXPECT_TRUE(writtenPackets.empty());
  EXPECT_EQ(stats().skippedDispatches, 1u);
}

TEST(HsaInterceptConcurrencyTest, ConcurrentStatsAccess) {
  std::atomic<bool> stop{false};
  std::atomic<int> iterations{0};

  std::thread reader([&]() {
    while (!stop.load()) {
      auto currentStats = stats();
      (void)currentStats;
      iterations++;
    }
  });

  std::thread resetter([&]() {
    while (!stop.load()) {
      resetStats();
      std::this_thread::yield();
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop.store(true);
  reader.join();
  resetter.join();

  EXPECT_GT(iterations.load(), 0);
}

TEST(HsaInterceptBoundaryTest, PublicHeadersHaveNoExternalDependencies) {
  const std::filesystem::path includeDir =
      std::filesystem::path(NEW_AEGIS_HSA_INTERCEPT_DIR) / "include";
  const std::vector<std::string> forbidden = {
      "<hsa/", "rocprofiler", "llvm/", "RuntimeConfig", "ProfilingRuntime",
      "DescriptorUpdater", "aegisbit/"};

  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(includeDir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string contents = readFile(entry.path());
    for (const std::string& token : forbidden) {
      EXPECT_EQ(contents.find(token), std::string::npos)
          << entry.path() << " contains forbidden token " << token;
    }
  }
}

TEST(HsaInterceptBoundaryTest, ImplementationHasNoAegisDependencies) {
  const std::filesystem::path srcDir =
      std::filesystem::path(NEW_AEGIS_HSA_INTERCEPT_DIR) / "src";
  const std::vector<std::string> forbidden = {
      "RuntimeConfig", "ProfilingRuntime", "DispatchAction", "DescriptorUpdater",
      "aegisbit/", "rewrite/", "output/"};

  for (const auto& entry : std::filesystem::recursive_directory_iterator(srcDir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string contents = readFile(entry.path());
    for (const std::string& token : forbidden) {
      EXPECT_EQ(contents.find(token), std::string::npos)
          << entry.path() << " contains forbidden token " << token;
    }
  }
}
