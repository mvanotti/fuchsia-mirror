// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_LIB_USERABI_USERBOOT_LOADER_SERVICE_H_
#define ZIRCON_KERNEL_LIB_USERABI_USERBOOT_LOADER_SERVICE_H_

#include <lib/zx/channel.h>
#include <lib/zx/debuglog.h>
#include <lib/zx/vmo.h>
#include <zircon/types.h>

#include <cstddef>

class Bootfs;

class LoaderService {
 public:
  LoaderService(zx::debuglog log, Bootfs* fs, const char* root)
      : log_(std::move(log)), fs_(fs), root_(root) {}

  // Handle loader-service RPCs on channel until there are no more.
  // Consumes the channel.
  void Serve(zx::channel);

 private:
  static constexpr const char kLoadObjectFilePrefix[] = "lib/";
  zx::debuglog log_;
  Bootfs* fs_;
  const char* root_;
  char prefix_[32];
  size_t prefix_len_ = 0;
  bool exclusive_ = false;

  bool HandleRequest(const zx::channel&);
  void Config(const char* string, size_t len);
  zx::vmo LoadObject(const char* name, size_t len);
  zx::vmo TryLoadObject(const char* name, size_t len, bool use_prefix);
};

#endif  // ZIRCON_KERNEL_LIB_USERABI_USERBOOT_LOADER_SERVICE_H_
