// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "system_instance.h"

#include <fidl/fuchsia.boot/cpp/wire.h>
#include <fidl/fuchsia.kernel/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/directory.h>
#include <lib/fidl-async/cpp/bind.h>
#include <lib/zx/channel.h>
#include <lib/zx/process.h>
#include <lib/zx/time.h>

#include <map>
#include <string>

#include <zxtest/zxtest.h>

namespace fboot = fuchsia_boot;
namespace fkernel = fuchsia_kernel;

namespace {

// Get the root job from the root job service.
zx_status_t get_root_job(zx::job* root_job) {
  auto client_end = service::Connect<fkernel::RootJob>();
  if (client_end.is_error()) {
    return client_end.error_value();
  }

  auto client = fidl::BindSyncClient(std::move(*client_end));
  auto result = client.Get();
  if (!result.ok()) {
    return result.status();
  }
  *root_job = std::move(result.value().job);
  return ZX_OK;
}

class FakeBootArgsServer final : public fidl::WireServer<fboot::Arguments> {
 public:
  FakeBootArgsServer() : values_() {}

  void SetBool(std::string key, bool value) { values_.insert_or_assign(key, value); }

  // fidl::WireServer<fuchsia_boot::Arguments> methods:
  void GetString(GetStringRequestView request, GetStringCompleter::Sync& completer) override {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void GetStrings(GetStringsRequestView request, GetStringsCompleter::Sync& completer) override {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void GetBool(GetBoolRequestView request, GetBoolCompleter::Sync& completer) override {
    bool result = request->defaultval;
    auto value = values_.find(std::string(request->key.data()));
    if (value != values_.end()) {
      result = value->second;
    }
    completer.Reply(result);
  }

  void GetBools(GetBoolsRequestView request, GetBoolsCompleter::Sync& completer) override {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void Collect(CollectRequestView request, CollectCompleter::Sync& completer) override {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

 private:
  std::map<std::string, bool> values_;
};

class SystemInstanceForTest : public SystemInstance {
 public:
  using SystemInstance::launcher;
};

class SystemInstanceTest : public zxtest::Test {
 public:
  SystemInstanceTest() : loop_(&kAsyncLoopConfigNoAttachToCurrentThread) {
    ASSERT_OK(loop_.StartThread());

    auto endpoints = fidl::CreateEndpoints<fboot::Arguments>();
    ASSERT_OK(endpoints.status_value());
    boot_args_server_.reset(new FakeBootArgsServer());
    fidl::BindSingleInFlightOnly(loop_.dispatcher(), std::move(endpoints->server),
                                 boot_args_server_.get());
    boot_args_client_ = fidl::BindSyncClient(std::move(endpoints->client));

    under_test_.reset(new SystemInstanceForTest());
  }

  std::unique_ptr<FakeBootArgsServer> boot_args_server_;
  fidl::WireSyncClient<fboot::Arguments> boot_args_client_;
  std::unique_ptr<SystemInstanceForTest> under_test_;

 private:
  async::Loop loop_;
};

// Verify the job that driver_hosts are launched under also lacks ZX_POL_AMBIENT_MARK_VMO_EXEC.
TEST_F(SystemInstanceTest, DriverHostJobLacksAmbientVmex) {
  zx::job root_job;
  ASSERT_OK(get_root_job(&root_job));

  zx::job driver_job;
  ASSERT_OK(under_test_->CreateDriverHostJob(root_job, &driver_job));

  zx::process proc;
  const char* args[] = {"/pkg/bin/ambient_vmex_test_util", nullptr};
  ASSERT_OK(under_test_->launcher().Launch(driver_job, args[0], args, nullptr, -1, zx::resource(),
                                           nullptr, nullptr, 0, &proc, 0));

  ASSERT_OK(proc.wait_one(ZX_TASK_TERMINATED, zx::time::infinite(), nullptr));
  zx_info_process_t proc_info;
  ASSERT_OK(proc.get_info(ZX_INFO_PROCESS, &proc_info, sizeof(proc_info), nullptr, nullptr));
  ASSERT_TRUE(proc_info.flags & ZX_INFO_PROCESS_FLAG_EXITED);
  // A return code of 1 from the util process indicates the replace_as_executable call failed with
  // ACCESS_DENIED.
  ASSERT_EQ(proc_info.return_code, 1);
}

TEST_F(SystemInstanceTest, DriverHostJobLacksNewProcess) {
  zx::job root_job;
  ASSERT_OK(get_root_job(&root_job));

  zx::job driver_job;
  ASSERT_OK(under_test_->CreateDriverHostJob(root_job, &driver_job));

  zx::process proc;
  const char* args[] = {"/pkg/bin/new_process_test_util", nullptr};
  ASSERT_OK(under_test_->launcher().Launch(driver_job, args[0], args, nullptr, -1, zx::resource(),
                                           nullptr, nullptr, 0, &proc, 0));

  ASSERT_OK(proc.wait_one(ZX_TASK_TERMINATED, zx::time::infinite(), nullptr));
  zx_info_process_t proc_info;
  ASSERT_OK(proc.get_info(ZX_INFO_PROCESS, &proc_info, sizeof(proc_info), nullptr, nullptr));
  ASSERT_TRUE(proc_info.flags & ZX_INFO_PROCESS_FLAG_EXITED);
  // A return code of 1 from the util process indicates the process_create call failed with
  // ACCESS_DENIED.
  ASSERT_EQ(proc_info.return_code, 1);
}

}  // namespace
