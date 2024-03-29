// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_F2FS_MKFS_H_
#define SRC_STORAGE_F2FS_MKFS_H_

namespace f2fs {

constexpr uint32_t kChecksumOffset = 4092;

static const char* kMediaExtList[] = {"jpg", "gif", "png",  "avi", "divx", "mp4", "mp3", "3gp",
                                      "wmv", "wma", "mpeg", "mkv", "mov",  "asx", "asf", "wmx",
                                      "svi", "wvx", "wm",   "mpg", "mpe",  "rm",  "ogg"};

struct MkfsOptions {
  char* label = nullptr;
  bool heap_based_allocation = true;
  uint32_t overprovision_ratio = 0;
  uint32_t segs_per_sec = 1;
  uint32_t secs_per_zone = 1;
  char* extension_list = nullptr;
};

class MkfsWorker {
 public:
  explicit MkfsWorker(std::unique_ptr<Bcache> bc, const MkfsOptions& options)
      : bc_(std::move(bc)), mkfs_options_(options) {}

  // Not copyable or moveable
  MkfsWorker(const MkfsWorker&) = delete;
  MkfsWorker& operator=(const MkfsWorker&) = delete;
  MkfsWorker(const MkfsWorker&&) = delete;
  MkfsWorker& operator=(const MkfsWorker&&) = delete;

  zx::status<std::unique_ptr<Bcache>> DoMkfs();

  std::unique_ptr<Bcache> Destroy() { return std::move(bc_); }

 private:
  std::unique_ptr<Bcache> bc_;
  MkfsOptions mkfs_options_{};

  // F2FS Parameter
  GlobalParameters params_;
  SuperBlock super_block_;

  void InitGlobalParameters();
  zx_status_t GetDeviceInfo();
  zx_status_t FormatDevice();

  void ConfigureExtensionList();

  zx_status_t WriteToDisk(FsBlock& buf, block_t bno);

  zx::status<uint32_t> GetCalculatedOp(uint32_t op);

  zx_status_t PrepareSuperBlock();
  zx_status_t InitSitArea();
  zx_status_t InitNatArea();

  zx_status_t WriteCheckPointPack();
  zx_status_t WriteSuperBlock();
  zx_status_t WriteRootInode();
  zx_status_t UpdateNatRoot();
  zx_status_t AddDefaultDentryRoot();
  zx_status_t CreateRootDir();

  void PrintCurrentOption();

  zx_status_t TrimDevice();
};

void PrintUsage();
zx_status_t ParseOptions(int argc, char** argv, MkfsOptions& options);

zx::status<std::unique_ptr<Bcache>> Mkfs(const MkfsOptions& options, std::unique_ptr<Bcache> bc);

void AsciiToUnicode(const std::string& in_string, std::u16string* out_string);

}  // namespace f2fs

#endif  // SRC_STORAGE_F2FS_MKFS_H_
