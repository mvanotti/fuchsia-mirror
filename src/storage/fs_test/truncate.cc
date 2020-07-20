// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zircon/syscalls.h>

#include <iterator>
#include <memory>

#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/unique_fd.h>

#include "src/storage/fs_test/truncate_fixture.h"

namespace fs_test {
namespace {

using TruncateTest = FilesystemTest;

void CheckFileContains(const char* filename, const void* data, ssize_t len) {
  char buf[4096];
  struct stat st;

  ASSERT_EQ(stat(filename, &st), 0);
  ASSERT_EQ(st.st_size, len);
  fbl::unique_fd fd(open(filename, O_RDWR, 0644));
  ASSERT_TRUE(fd);
  ASSERT_EQ(read(fd.get(), buf, len), len);
  ASSERT_EQ(memcmp(buf, data, len), 0);
}

void CheckFileEmpty(const char* filename) {
  struct stat st;
  ASSERT_EQ(stat(filename, &st), 0);
  ASSERT_EQ(st.st_size, 0);
}

// Test that the really simple cases of truncate are operational
TEST_P(TruncateTest, TruncateSmall) {
  const char* str = "Hello, World!\n";
  const std::string filename = GetPath("alpha");

  // Try writing a string to a file
  fbl::unique_fd fd(open(filename.c_str(), O_RDWR | O_CREAT, 0644));
  ASSERT_TRUE(fd);
  ASSERT_EQ(write(fd.get(), str, strlen(str)), static_cast<ssize_t>(strlen(str)));
  ASSERT_NO_FATAL_FAILURE(CheckFileContains(filename.c_str(), str, strlen(str)));

  // Check that opening a file with O_TRUNC makes it empty
  fbl::unique_fd fd2(open(filename.c_str(), O_RDWR | O_TRUNC, 0644));
  ASSERT_TRUE(fd2);
  ASSERT_NO_FATAL_FAILURE(CheckFileEmpty(filename.c_str()));

  // Check that we can still write to a file that has been truncated
  ASSERT_EQ(lseek(fd.get(), 0, SEEK_SET), 0);
  ASSERT_EQ(write(fd.get(), str, strlen(str)), static_cast<ssize_t>(strlen(str)));
  ASSERT_NO_FATAL_FAILURE(CheckFileContains(filename.c_str(), str, strlen(str)));

  // Check that we can truncate the file using the "truncate" function
  ASSERT_EQ(truncate(filename.c_str(), 5), 0);
  ASSERT_NO_FATAL_FAILURE(CheckFileContains(filename.c_str(), str, 5));
  ASSERT_EQ(truncate(filename.c_str(), 0), 0);
  ASSERT_NO_FATAL_FAILURE(CheckFileEmpty(filename.c_str()));

  // Check that truncating an already empty file does not cause problems
  ASSERT_EQ(truncate(filename.c_str(), 0), 0);
  ASSERT_NO_FATAL_FAILURE(CheckFileEmpty(filename.c_str()));

  // Check that we can use truncate to extend a file
  char empty[5] = {0, 0, 0, 0, 0};
  ASSERT_EQ(truncate(filename.c_str(), 5), 0);
  ASSERT_NO_FATAL_FAILURE(CheckFileContains(filename.c_str(), empty, 5));

  ASSERT_EQ(close(fd.release()), 0);
  ASSERT_EQ(close(fd2.release()), 0);
  ASSERT_EQ(unlink(filename.c_str()), 0);
}

enum class SparseTestType {
  UnlinkThenClose,
  CloseThenUnlink,
};

using ParamType = std::tuple<TestFilesystemOptions, SparseTestType>;

class SparseTruncateTest : public BaseFilesystemTest,
                           public testing::WithParamInterface<ParamType> {
 public:
  SparseTruncateTest() : BaseFilesystemTest(std::get<0>(GetParam())) {}

  SparseTestType test_type() const { return std::get<1>(GetParam()); }
};

// This test catches a particular regression in MinFS truncation, where, if a block is cut in half
// for truncation, it is read, filled with zeroes, and written back out to disk.
//
// This test tries to proke at a variety of offsets of interest.
TEST_P(SparseTruncateTest, PartialBlockSparse) {
  // TODO(smklein): Acquire these constants directly from MinFS's header
  constexpr size_t kBlockSize = 8192;
  constexpr size_t kDirectBlocks = 16;
  constexpr size_t kIndirectBlocks = 31;
  constexpr size_t kDirectPerIndirect = kBlockSize / 4;

  uint8_t buf[kBlockSize];
  memset(buf, 0xAB, sizeof(buf));

  off_t write_offsets[] = {
      kBlockSize * 5,
      kBlockSize * kDirectBlocks,
      kBlockSize * kDirectBlocks + kBlockSize * kDirectPerIndirect * 1,
      kBlockSize * kDirectBlocks + kBlockSize * kDirectPerIndirect * 2,
      kBlockSize * kDirectBlocks + kBlockSize * kDirectPerIndirect * kIndirectBlocks -
          2 * kBlockSize,
      kBlockSize * kDirectBlocks + kBlockSize * kDirectPerIndirect * kIndirectBlocks - kBlockSize,
      kBlockSize * kDirectBlocks + kBlockSize * kDirectPerIndirect * kIndirectBlocks,
      kBlockSize * kDirectBlocks + kBlockSize * kDirectPerIndirect * kIndirectBlocks + kBlockSize,
  };

  for (size_t i = 0; i < std::size(write_offsets); i++) {
    off_t write_off = write_offsets[i];
    fbl::unique_fd fd(open(GetPath("truncate-sparse").c_str(), O_CREAT | O_RDWR));
    ASSERT_TRUE(fd);
    ASSERT_EQ(lseek(fd.get(), write_off, SEEK_SET), write_off);
    ASSERT_EQ(write(fd.get(), buf, sizeof(buf)), static_cast<ssize_t>(sizeof(buf)));
    ASSERT_EQ(ftruncate(fd.get(), write_off + 2 * kBlockSize), 0);
    ASSERT_EQ(ftruncate(fd.get(), write_off + kBlockSize + kBlockSize / 2), 0);
    ASSERT_EQ(ftruncate(fd.get(), write_off + kBlockSize / 2), 0);
    ASSERT_EQ(ftruncate(fd.get(), write_off - kBlockSize / 2), 0);
    if (test_type() == SparseTestType::UnlinkThenClose) {
      ASSERT_EQ(unlink(GetPath("truncate-sparse").c_str()), 0);
      ASSERT_EQ(close(fd.release()), 0);
    } else {
      ASSERT_EQ(close(fd.release()), 0);
      ASSERT_EQ(unlink(GetPath("truncate-sparse").c_str()), 0);
    }
  }
}

TEST_P(TruncateTest, Errno) {
  fbl::unique_fd fd(open(GetPath("truncate_errno").c_str(), O_RDWR | O_CREAT | O_EXCL));
  ASSERT_TRUE(fd);

  ASSERT_EQ(ftruncate(fd.get(), -1), -1);
  ASSERT_EQ(errno, EINVAL);
  errno = 0;
  ASSERT_EQ(ftruncate(fd.get(), 1UL << 60), -1);
  ASSERT_EQ(errno, EINVAL);

  ASSERT_EQ(unlink(GetPath("truncate_errno").c_str()), 0);
  ASSERT_EQ(close(fd.release()), 0);
}

std::string GetParamDescription(const testing::TestParamInfo<ParamType> param) {
  std::stringstream s;
  s << std::get<0>(param.param);
  switch (std::get<1>(param.param)) {
    case SparseTestType::UnlinkThenClose:
      s << "UnlinkThenClose";
      break;
    case SparseTestType::CloseThenUnlink:
      s << "CloseThenUnlink";
      break;
  }
  return s.str();
}

INSTANTIATE_TEST_SUITE_P(/*no prefix*/, TruncateTest, testing::ValuesIn(AllTestFilesystems()),
                         testing::PrintToStringParamName());

INSTANTIATE_TEST_SUITE_P(/*no prefix*/, SparseTruncateTest,
                         testing::Combine(testing::ValuesIn(AllTestFilesystems()),
                                          testing::Values(SparseTestType::UnlinkThenClose,
                                                          SparseTestType::CloseThenUnlink)),
                         GetParamDescription);

INSTANTIATE_TEST_SUITE_P(
    /*no prefix*/, LargeTruncateTest,
    testing::Combine(testing::ValuesIn(AllTestFilesystems()),
                     testing::Values(std::make_tuple(1 << 10, 100, LargeTruncateTestType::KeepOpen),
                                     std::make_tuple(1 << 10, 100, LargeTruncateTestType::Reopen),
                                     std::make_tuple(1 << 15, 50, LargeTruncateTestType::KeepOpen),
                                     std::make_tuple(1 << 15, 50, LargeTruncateTestType::Reopen))),
    GetDescriptionForLargeTruncateTestParamType);

}  // namespace
}  // namespace fs_test
