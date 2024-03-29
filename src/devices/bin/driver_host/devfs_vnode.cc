// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "devfs_vnode.h"

#include <lib/ddk/device.h>

#include <string_view>

#include <fbl/string_buffer.h>

#include "driver_host.h"
#include "src/lib/storage/vfs/cpp/vfs_types.h"

namespace {

namespace statecontrol_fidl = fuchsia_hardware_power_statecontrol;

// The path might get truncated if too long, which isn't a problem because it should just result in
// the driver not being found.
fbl::StringBuffer<fuchsia_device::wire::kMaxDriverPathLen> GetFullDriverPath(
    const fidl::StringView& driver_path) {
  fbl::StringBuffer<fuchsia_device::wire::kMaxDriverPathLen> buffer;
  if (!driver_path.empty() && driver_path[0] != '/') {
    buffer.Append(std::string_view(internal::ContextForApi()->root_driver_path()));
  }
  return buffer.Append(driver_path.data(), driver_path.size());
}

}  // namespace

zx_status_t DevfsVnode::OpenNode(fs::Vnode::ValidatedOptions options,
                                 fbl::RefPtr<Vnode>* out_redirect) {
  if (dev_->Unbound()) {
    return ZX_ERR_IO_NOT_PRESENT;
  }
  fbl::RefPtr<zx_device_t> new_dev;
  zx_status_t status = device_open(dev_, &new_dev, options->ToIoV1Flags());
  if (status != ZX_OK) {
    return status;
  }
  if (new_dev != dev_) {
    *out_redirect = new_dev->vnode;
  }
  return ZX_OK;
}

zx_status_t DevfsVnode::CloseNode() {
  zx_status_t status = device_close(dev_, 0);
  // If this vnode is for an instance device, drop its reference on close to break
  // the reference cycle.  This is handled for non-instance devices during the device
  // remove path.
  if (dev_->flags() & DEV_FLAG_INSTANCE) {
    dev_->vnode.reset();
  }
  return status;
}

zx_status_t DevfsVnode::GetAttributes(fs::VnodeAttributes* a) {
  a->mode = V_TYPE_CDEV | V_IRUSR | V_IWUSR;
  a->content_size = dev_->GetSizeOp();
  a->link_count = 1;
  return ZX_OK;
}

fs::VnodeProtocolSet DevfsVnode::GetProtocols() const { return fs::VnodeProtocol::kDevice; }

zx_status_t DevfsVnode::GetNodeInfoForProtocol(fs::VnodeProtocol protocol, fs::Rights rights,
                                               fs::VnodeRepresentation* info) {
  if (protocol == fs::VnodeProtocol::kDevice) {
    zx::eventpair event;
    if (dev_->event.is_valid()) {
      zx_status_t status = dev_->event.duplicate(ZX_RIGHTS_BASIC, &event);
      if (status != ZX_OK) {
        return status;
      }
    }
    *info = fs::VnodeRepresentation::Device{std::move(event)};
    return ZX_OK;
  }
  return ZX_ERR_NOT_SUPPORTED;
}

void DevfsVnode::HandleFsSpecificMessage(fidl::IncomingMessage& msg, fidl::Transaction* txn) {
  if (dev_->Unbound()) {
    txn->Close(ZX_ERR_IO_NOT_PRESENT);
    return;
  }
  ::fidl::DispatchResult dispatch_result =
      fidl::WireTryDispatch<fuchsia_device::Controller>(this, msg, txn);
  if (dispatch_result == ::fidl::DispatchResult::kFound) {
    return;
  }

  fidl_incoming_msg_t c_msg = std::move(msg).ReleaseToEncodedCMessage();
  auto ddk_txn = MakeDdkInternalTransaction(txn);
  zx_status_t status = dev_->MessageOp(&c_msg, ddk_txn.Txn());
  if (status != ZX_OK && status != ZX_ERR_ASYNC) {
    // Close the connection on any error
    txn->Close(status);
  }
}

zx_status_t DevfsVnode::Read(void* data, size_t len, size_t off, size_t* out_actual) {
  if (dev_->Unbound()) {
    return ZX_ERR_IO_NOT_PRESENT;
  }
  return dev_->ReadOp(data, len, off, out_actual);
}
zx_status_t DevfsVnode::Write(const void* data, size_t len, size_t off, size_t* out_actual) {
  if (dev_->Unbound()) {
    return ZX_ERR_IO_NOT_PRESENT;
  }
  return dev_->WriteOp(data, len, off, out_actual);
}

void DevfsVnode::Bind(BindRequestView request, BindCompleter::Sync& completer) {
  zx_status_t status = device_bind(dev_, GetFullDriverPath(request->driver).c_str());
  if (status != ZX_OK) {
    completer.ReplyError(status);
  } else {
    dev_->set_bind_conn([completer = completer.ToAsync()](zx_status_t status) mutable {
      if (status != ZX_OK) {
        completer.ReplyError(status);
      } else {
        completer.ReplySuccess();
      }
    });
  }
};

void DevfsVnode::GetDevicePerformanceStates(GetDevicePerformanceStatesRequestView request,
                                            GetDevicePerformanceStatesCompleter::Sync& completer) {
  auto& perf_states = dev_->GetPerformanceStates();
  ZX_DEBUG_ASSERT(perf_states.size() == fuchsia_device::wire::kMaxDevicePerformanceStates);

  ::fidl::Array<fuchsia_device::wire::DevicePerformanceStateInfo, 20> states{};
  for (size_t i = 0; i < fuchsia_device::wire::kMaxDevicePerformanceStates; i++) {
    states[i] = perf_states[i];
  }
  completer.Reply(states, ZX_OK);
}

void DevfsVnode::GetCurrentPerformanceState(GetCurrentPerformanceStateRequestView request,
                                            GetCurrentPerformanceStateCompleter::Sync& completer) {
  completer.Reply(dev_->current_performance_state());
}

void DevfsVnode::Rebind(RebindRequestView request, RebindCompleter::Sync& completer) {
  dev_->set_rebind_drv_name(GetFullDriverPath(request->driver).c_str());
  zx_status_t status = device_rebind(dev_.get());

  if (status != ZX_OK) {
    completer.ReplyError(status);
  } else {
    // These will be set, until device is unbound and then bound again.
    dev_->set_rebind_conn([completer = completer.ToAsync()](zx_status_t status) mutable {
      if (status != ZX_OK) {
        completer.ReplyError(status);
      } else {
        completer.ReplySuccess();
      }
    });
  }
}

void DevfsVnode::UnbindChildren(UnbindChildrenRequestView request,
                                UnbindChildrenCompleter::Sync& completer) {
  zx_status_t status = device_schedule_unbind_children(dev_);

  if (status != ZX_OK) {
    completer.ReplyError(status);
  } else {
    // The unbind conn will be set until all the children of this device are unbound.
    dev_->set_unbind_children_conn([completer = completer.ToAsync()](zx_status_t status) mutable {
      if (status != ZX_OK) {
        completer.ReplyError(status);
      } else {
        completer.ReplySuccess();
      }
    });
  }
}

void DevfsVnode::ScheduleUnbind(ScheduleUnbindRequestView request,
                                ScheduleUnbindCompleter::Sync& completer) {
  zx_status_t status = device_schedule_remove(dev_, true /* unbind_self */);
  if (status != ZX_OK) {
    completer.ReplyError(status);
  } else {
    completer.ReplySuccess();
  }
}

void DevfsVnode::GetDriverName(GetDriverNameRequestView request,
                               GetDriverNameCompleter::Sync& completer) {
  if (!dev_->driver) {
    completer.Reply(ZX_ERR_NOT_SUPPORTED, {});
    return;
  }
  const char* name = dev_->driver->name();
  if (name == nullptr) {
    name = "unknown";
  }
  completer.Reply(ZX_OK, fidl::StringView::FromExternal(name));
}

void DevfsVnode::GetDeviceName(GetDeviceNameRequestView request,
                               GetDeviceNameCompleter::Sync& completer) {
  completer.Reply(fidl::StringView::FromExternal(dev_->name()));
}

void DevfsVnode::GetTopologicalPath(GetTopologicalPathRequestView request,
                                    GetTopologicalPathCompleter::Sync& completer) {
  char buf[fuchsia_device::wire::kMaxDevicePathLen + 1];
  size_t actual;
  zx_status_t status = dev_->driver_host_context()->GetTopoPath(dev_, buf, sizeof(buf), &actual);
  if (status != ZX_OK) {
    completer.ReplyError(status);
    return;
  }
  if (actual > 0) {
    // Remove the accounting for the null byte
    actual--;
  }
  auto path = ::fidl::StringView(buf, actual);
  completer.ReplySuccess(std::move(path));
}

void DevfsVnode::GetEventHandle(GetEventHandleRequestView request,
                                GetEventHandleCompleter::Sync& completer) {
  zx::eventpair event;
  zx_status_t status = dev_->event.duplicate(ZX_RIGHTS_BASIC, &event);
  static_assert(fuchsia_device::wire::kDeviceSignalReadable == DEV_STATE_READABLE);
  static_assert(fuchsia_device::wire::kDeviceSignalWritable == DEV_STATE_WRITABLE);
  static_assert(fuchsia_device::wire::kDeviceSignalError == DEV_STATE_ERROR);
  static_assert(fuchsia_device::wire::kDeviceSignalHangup == DEV_STATE_HANGUP);
  static_assert(fuchsia_device::wire::kDeviceSignalOob == DEV_STATE_OOB);
  // TODO(teisenbe): The FIDL definition erroneously describes this as an event rather than
  // eventpair.
  completer.Reply(status, zx::event(event.release()));
}

void DevfsVnode::GetMinDriverLogSeverity(GetMinDriverLogSeverityRequestView request,
                                         GetMinDriverLogSeverityCompleter::Sync& completer) {
  if (!dev_->driver) {
    completer.Reply(ZX_ERR_UNAVAILABLE, 0);
    return;
  }
  uint8_t severity = fx_logger_get_min_severity(dev_->driver->logger());
  completer.Reply(ZX_OK, severity);
}

void DevfsVnode::SetMinDriverLogSeverity(SetMinDriverLogSeverityRequestView request,
                                         SetMinDriverLogSeverityCompleter::Sync& completer) {
  if (!dev_->driver) {
    completer.Reply(ZX_ERR_UNAVAILABLE);
    return;
  }
  auto status = dev_->driver->set_driver_min_log_severity(request->severity);
  completer.Reply(status);
}

void DevfsVnode::RunCompatibilityTests(RunCompatibilityTestsRequestView request,
                                       RunCompatibilityTestsCompleter::Sync& completer) {
  auto shared_completer =
      std::make_shared<RunCompatibilityTestsCompleter::Async>(completer.ToAsync());
  zx_status_t status = device_run_compatibility_tests(
      dev_, request->hook_wait_time,
      [shared_completer](zx_status_t status) mutable { shared_completer->Reply(status); });
  if (status != ZX_OK) {
    shared_completer->Reply(status);
  }
};

void DevfsVnode::GetDevicePowerCaps(GetDevicePowerCapsRequestView request,
                                    GetDevicePowerCapsCompleter::Sync& completer) {
  // For now, the result is always a successful response because the device itself is not added
  // without power states validation. In future, we may add more checks for validation, and the
  // error result will be put to use.
  fuchsia_device::wire::ControllerGetDevicePowerCapsResponse response{};
  auto& states = dev_->GetPowerStates();

  ZX_DEBUG_ASSERT(states.size() == fuchsia_device::wire::kMaxDevicePowerStates);
  for (size_t i = 0; i < fuchsia_device::wire::kMaxDevicePowerStates; i++) {
    response.dpstates[i] = states[i];
  }
  completer.Reply(fuchsia_device::wire::ControllerGetDevicePowerCapsResult::WithResponse(
      fidl::ObjectView<fuchsia_device::wire::ControllerGetDevicePowerCapsResponse>::FromExternal(
          &response)));
};

void DevfsVnode::SetPerformanceState(SetPerformanceStateRequestView request,
                                     SetPerformanceStateCompleter::Sync& completer) {
  uint32_t out_state;
  zx_status_t status = dev_->driver_host_context()->DeviceSetPerformanceState(
      dev_, request->requested_state, &out_state);
  completer.Reply(status, out_state);
}

void DevfsVnode::ConfigureAutoSuspend(ConfigureAutoSuspendRequestView request,
                                      ConfigureAutoSuspendCompleter::Sync& completer) {
  zx_status_t status = dev_->driver_host_context()->DeviceConfigureAutoSuspend(
      dev_, request->enable, request->requested_deepest_sleep_state);
  completer.Reply(status);
}

void DevfsVnode::UpdatePowerStateMapping(UpdatePowerStateMappingRequestView request,
                                         UpdatePowerStateMappingCompleter::Sync& completer) {
  std::array<fuchsia_device::wire::SystemPowerStateInfo,
             statecontrol_fidl::wire::kMaxSystemPowerStates>
      states_mapping;

  for (size_t i = 0; i < statecontrol_fidl::wire::kMaxSystemPowerStates; i++) {
    states_mapping[i] = request->mapping[i];
  }
  zx_status_t status = dev_->SetSystemPowerStateMapping(states_mapping);
  if (status != ZX_OK) {
    completer.ReplyError(status);
    return;
  }

  fuchsia_device::wire::ControllerUpdatePowerStateMappingResponse response;
  completer.Reply(
      fuchsia_device::wire::ControllerUpdatePowerStateMappingResult::WithResponse(response));
}

void DevfsVnode::GetPowerStateMapping(GetPowerStateMappingRequestView request,
                                      GetPowerStateMappingCompleter::Sync& completer) {
  fuchsia_device::wire::ControllerGetPowerStateMappingResponse response;

  auto& mapping = dev_->GetSystemPowerStateMapping();
  ZX_DEBUG_ASSERT(mapping.size() == statecontrol_fidl::wire::kMaxSystemPowerStates);

  for (size_t i = 0; i < statecontrol_fidl::wire::kMaxSystemPowerStates; i++) {
    response.mapping[i] = mapping[i];
  }
  completer.Reply(fuchsia_device::wire::ControllerGetPowerStateMappingResult::WithResponse(
      fidl::ObjectView<fuchsia_device::wire::ControllerGetPowerStateMappingResponse>::FromExternal(
          &response)));
};

void DevfsVnode::Suspend(SuspendRequestView request, SuspendCompleter::Sync& completer) {
  dev_->suspend_cb = [completer = completer.ToAsync()](zx_status_t status,
                                                       uint8_t out_state) mutable {
    completer.Reply(status, static_cast<fuchsia_device::wire::DevicePowerState>(out_state));
  };
  dev_->driver_host_context()->DeviceSuspendNew(dev_, request->requested_state);
}

void DevfsVnode::Resume(ResumeRequestView request, ResumeCompleter::Sync& completer) {
  dev_->resume_cb = [completer = completer.ToAsync()](zx_status_t status, uint8_t out_power_state,
                                                      uint32_t out_perf_state) mutable {
    completer.Reply(status, static_cast<fuchsia_device::wire::DevicePowerState>(out_power_state),
                    out_perf_state);
  };
  dev_->driver_host_context()->DeviceResumeNew(dev_);
}

namespace {

// Reply originating from driver.
zx_status_t DdkReply(fidl_txn_t* txn, const fidl_outgoing_msg_t* msg) {
  auto message = fidl::OutgoingMessage::FromEncodedCMessage(msg);
  // If FromDdkInternalTransaction returns a unique_ptr variant, it will be destroyed when exiting
  // this scope.
  auto fidl_txn = FromDdkInternalTransaction(ddk::internal::Transaction::FromTxn(txn));
  std::visit([&](auto&& arg) { arg->Reply(&message); }, fidl_txn);
  return ZX_OK;
}

// Bitmask for checking if a pointer stashed in a ddk::internal::Transaction is from the heap or
// not. This is safe to use on our pointers, because fidl::Transactions are always aligned to more
// than one byte.
static_assert(alignof(fidl::Transaction) > 1);
constexpr uintptr_t kTransactionIsBoxed = 0x1;

}  // namespace

ddk::internal::Transaction MakeDdkInternalTransaction(fidl::Transaction* txn) {
  device_fidl_txn_t fidl_txn = {};
  fidl_txn.txn = {
      .reply = DdkReply,
  };
  fidl_txn.driver_host_context = reinterpret_cast<uintptr_t>(txn);
  return ddk::internal::Transaction(fidl_txn);
}

ddk::internal::Transaction MakeDdkInternalTransaction(std::unique_ptr<fidl::Transaction> txn) {
  device_fidl_txn_t fidl_txn = {};
  fidl_txn.txn = {
      .reply = DdkReply,
  };
  fidl_txn.driver_host_context = reinterpret_cast<uintptr_t>(txn.release()) | kTransactionIsBoxed;
  return ddk::internal::Transaction(fidl_txn);
}

std::variant<fidl::Transaction*, std::unique_ptr<fidl::Transaction>> FromDdkInternalTransaction(
    ddk::internal::Transaction* txn) {
  uintptr_t raw = txn->DriverHostCtx();
  ZX_ASSERT_MSG(raw != 0, "Reused a fidl_txn_t!\n");

  // Invalidate the source transaction
  txn->DeviceFidlTxn()->driver_host_context = 0;

  auto ptr = reinterpret_cast<fidl::Transaction*>(raw & ~kTransactionIsBoxed);
  if (raw & kTransactionIsBoxed) {
    return std::unique_ptr<fidl::Transaction>(ptr);
  }
  return ptr;
}
