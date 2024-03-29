// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "connection.h"

#include <endian.h>

#include "lib/async/default.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/defaults.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/protocol.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/command_channel.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/status.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/transport.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace bt::hci {

// Production implementation of the Connection class against a HCI transport.
class ConnectionImpl final : public Connection {
 public:
  ConnectionImpl(hci_spec::ConnectionHandle handle, bt::LinkType ll_type, Role role,
                 const DeviceAddress& local_address, const DeviceAddress& peer_address,
                 fxl::WeakPtr<Transport> hci);
  ~ConnectionImpl() override;

  // Connection overrides:
  fxl::WeakPtr<Connection> WeakPtr() override;
  bool StartEncryption() override;
  State state() const { return conn_state_; }

  // Sends HCI Disconnect command with |reason|. Takes over handling of HCI
  // Disconnection Complete event with a lambda so that connection instance can be safely destroyed
  // immediately.
  void Disconnect(hci_spec::StatusCode reason) override;

  void set_state(State state) { conn_state_ = state; };

 private:
  // Start the BR/EDR link layer encryption. |ltk_| and |ltk_type_| must have already been set and
  // may be used for bonding and detecting the security properties following the encryption
  // enable.
  bool BrEdrStartEncryption();

  // Start the LE link layer authentication procedure using the given |ltk|.
  bool LEStartEncryption(const hci_spec::LinkKey& ltk);

  // Called when encryption is enabled or disabled as a result of the link layer
  // encryption "start" or "pause" procedure. If |status| indicates failure,
  // then this method will disconnect the link before notifying the encryption
  // change handler.
  void HandleEncryptionStatus(Status status, bool enabled);

  // Request the current encryption key size and call |key_size_validity_cb|
  // when the controller responds. |key_size_validity_cb| will be called with a
  // success only if the link is encrypted with a key of size at least
  // |hci_spec::kMinEncryptionKeySize|. Only valid for ACL-U connections.
  void ValidateAclEncryptionKeySize(hci::StatusCallback key_size_validity_cb);

  // HCI event handlers.
  CommandChannel::EventCallbackResult OnEncryptionChangeEvent(const EventPacket& event);
  CommandChannel::EventCallbackResult OnEncryptionKeyRefreshCompleteEvent(const EventPacket& event);
  CommandChannel::EventCallbackResult OnLELongTermKeyRequestEvent(const EventPacket& event);
  CommandChannel::EventCallbackResult OnDisconnectionCompleteEvent(const EventPacket& event);

  // Checks |event|, unregisters link, and clears pending packets count.
  // If the disconnection was initiated by the peer, call |peer_disconnect_callback|.
  // Returns true if event was valid and for this connection.
  // This method is static so that it can be called in an event handler
  // after this object has been destroyed.
  static CommandChannel::EventCallbackResult OnDisconnectionComplete(
      fxl::WeakPtr<ConnectionImpl> self, hci_spec::ConnectionHandle handle,
      fxl::WeakPtr<Transport> hci, const EventPacket& event);

  fit::thread_checker thread_checker_;

  // IDs for encryption related HCI event handlers.
  CommandChannel::EventHandlerId enc_change_id_;
  CommandChannel::EventHandlerId enc_key_refresh_cmpl_id_;
  CommandChannel::EventHandlerId le_ltk_request_id_;

  // The underlying HCI transport.
  fxl::WeakPtr<Transport> hci_;

  State conn_state_;

  // Keep this as the last member to make sure that all weak pointers are
  // invalidated before other members get destroyed.
  fxl::WeakPtrFactory<ConnectionImpl> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(ConnectionImpl);
};

namespace {

template <
    CommandChannel::EventCallbackResult (ConnectionImpl::*EventHandlerMethod)(const EventPacket&)>
CommandChannel::EventCallback BindEventHandler(fxl::WeakPtr<ConnectionImpl> conn) {
  return [conn](const auto& event) {
    if (conn) {
      return ((conn.get())->*EventHandlerMethod)(event);
    }
    return CommandChannel::EventCallbackResult::kRemove;
  };
}

}  // namespace

// ====== Connection member methods  =====

// static
std::unique_ptr<Connection> Connection::CreateLE(hci_spec::ConnectionHandle handle, Role role,
                                                 const DeviceAddress& local_address,
                                                 const DeviceAddress& peer_address,
                                                 const hci_spec::LEConnectionParameters& params,
                                                 fxl::WeakPtr<Transport> hci) {
  ZX_DEBUG_ASSERT(local_address.type() != DeviceAddress::Type::kBREDR);
  ZX_DEBUG_ASSERT(peer_address.type() != DeviceAddress::Type::kBREDR);
  auto conn = std::make_unique<ConnectionImpl>(handle, bt::LinkType::kLE, role, local_address,
                                               peer_address, std::move(hci));
  conn->set_low_energy_parameters(params);
  return conn;
}

// static
std::unique_ptr<Connection> Connection::CreateACL(hci_spec::ConnectionHandle handle, Role role,
                                                  const DeviceAddress& local_address,
                                                  const DeviceAddress& peer_address,
                                                  fxl::WeakPtr<Transport> hci) {
  ZX_DEBUG_ASSERT(local_address.type() == DeviceAddress::Type::kBREDR);
  ZX_DEBUG_ASSERT(peer_address.type() == DeviceAddress::Type::kBREDR);
  auto conn = std::make_unique<ConnectionImpl>(handle, bt::LinkType::kACL, role, local_address,
                                               peer_address, std::move(hci));
  return conn;
}

std::unique_ptr<Connection> Connection::CreateSCO(hci_spec::LinkType link_type,
                                                  hci_spec::ConnectionHandle handle,
                                                  const DeviceAddress& local_address,
                                                  const DeviceAddress& peer_address,
                                                  fxl::WeakPtr<Transport> hci) {
  ZX_ASSERT(local_address.type() == DeviceAddress::Type::kBREDR);
  ZX_ASSERT(peer_address.type() == DeviceAddress::Type::kBREDR);
  ZX_ASSERT(link_type == hci_spec::LinkType::kSCO || link_type == hci_spec::LinkType::kExtendedSCO);

  bt::LinkType conn_type =
      link_type == hci_spec::LinkType::kSCO ? bt::LinkType::kSCO : bt::LinkType::kESCO;

  // TODO(fxb/61070): remove role for SCO connections, as it has no meaning
  auto conn = std::make_unique<ConnectionImpl>(handle, conn_type, Role::kMaster, local_address,
                                               peer_address, std::move(hci));
  return conn;
}

Connection::Connection(hci_spec::ConnectionHandle handle, bt::LinkType ll_type, Role role,
                       const DeviceAddress& local_address, const DeviceAddress& peer_address)
    : ll_type_(ll_type),
      handle_(handle),
      role_(role),
      local_address_(local_address),
      peer_address_(peer_address) {
  ZX_DEBUG_ASSERT(handle_);
}

std::string Connection::ToString() const {
  std::string params = "";
  if (ll_type() == bt::LinkType::kLE) {
    params = ", " + le_params_.ToString();
  }
  return fxl::StringPrintf("(%s link - handle: %#.4x, role: %s, address: %s%s)",
                           LinkTypeToString(ll_type_).c_str(), handle_,
                           role_ == Role::kMaster ? "master" : "slave",
                           peer_address_.ToString().c_str(), params.c_str());
}

// ====== ConnectionImpl member methods ======

ConnectionImpl::ConnectionImpl(hci_spec::ConnectionHandle handle, bt::LinkType ll_type, Role role,
                               const DeviceAddress& local_address,
                               const DeviceAddress& peer_address, fxl::WeakPtr<Transport> hci)
    : Connection(handle, ll_type, role, local_address, peer_address),
      hci_(std::move(hci)),
      conn_state_(State::kConnected),
      weak_ptr_factory_(this) {
  ZX_DEBUG_ASSERT(hci_);

  auto self = weak_ptr_factory_.GetWeakPtr();

  enc_change_id_ = hci_->command_channel()->AddEventHandler(
      hci_spec::kEncryptionChangeEventCode,
      BindEventHandler<&ConnectionImpl::OnEncryptionChangeEvent>(self));

  enc_key_refresh_cmpl_id_ = hci_->command_channel()->AddEventHandler(
      hci_spec::kEncryptionKeyRefreshCompleteEventCode,
      BindEventHandler<&ConnectionImpl::OnEncryptionKeyRefreshCompleteEvent>(self));

  le_ltk_request_id_ = hci_->command_channel()->AddLEMetaEventHandler(
      hci_spec::kLELongTermKeyRequestSubeventCode,
      BindEventHandler<&ConnectionImpl::OnLELongTermKeyRequestEvent>(self));

  auto disconn_complete_handler = [self, handle, hci = hci_](auto& event) {
    return ConnectionImpl::OnDisconnectionComplete(self, handle, hci, event);
  };

  hci_->command_channel()->AddEventHandler(hci_spec::kDisconnectionCompleteEventCode,
                                           disconn_complete_handler);

  // Allow packets to be sent on this link immediately.
  hci_->acl_data_channel()->RegisterLink(handle, ll_type);
}

ConnectionImpl::~ConnectionImpl() {
  if (conn_state_ == Connection::State::kConnected) {
    Disconnect(hci_spec::StatusCode::kRemoteUserTerminatedConnection);
  }

  // Unregister HCI event handlers.
  hci_->command_channel()->RemoveEventHandler(enc_change_id_);
  hci_->command_channel()->RemoveEventHandler(enc_key_refresh_cmpl_id_);
  hci_->command_channel()->RemoveEventHandler(le_ltk_request_id_);
}

fxl::WeakPtr<Connection> ConnectionImpl::WeakPtr() { return weak_ptr_factory_.GetWeakPtr(); }

CommandChannel::EventCallbackResult ConnectionImpl::OnDisconnectionComplete(
    fxl::WeakPtr<ConnectionImpl> self, hci_spec::ConnectionHandle handle,
    fxl::WeakPtr<Transport> hci, const EventPacket& event) {
  ZX_DEBUG_ASSERT(event.event_code() == hci_spec::kDisconnectionCompleteEventCode);

  if (event.view().payload_size() != sizeof(hci_spec::DisconnectionCompleteEventParams)) {
    bt_log(WARN, "hci", "malformed disconnection complete event");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  const auto& params = event.params<hci_spec::DisconnectionCompleteEventParams>();
  const auto event_handle = le16toh(params.connection_handle);

  // Silently ignore this event as it isn't meant for this connection.
  if (event_handle != handle) {
    return CommandChannel::EventCallbackResult::kContinue;
  }

  bt_log(INFO, "hci", "disconnection complete - %s, handle: %#.4x, reason: %#.2x",
         bt_str(event.ToStatus()), handle, params.reason);

  // Stop data flow and revoke queued packets for this connection.
  hci->acl_data_channel()->UnregisterLink(handle);

  // Notify ACL data channel that packets have been flushed from controller buffer.
  hci->acl_data_channel()->ClearControllerPacketCount(handle);

  if (!self) {
    return CommandChannel::EventCallbackResult::kRemove;
  }

  self->set_state(State::kDisconnected);

  // Peer disconnect. Callback may destroy connection.
  if (self->peer_disconnect_callback()) {
    self->peer_disconnect_callback()(self.get(), params.reason);
  }

  return CommandChannel::EventCallbackResult::kRemove;
}

void ConnectionImpl::Disconnect(hci_spec::StatusCode reason) {
  ZX_ASSERT(conn_state_ == Connection::State::kConnected);

  conn_state_ = Connection::State::kWaitingForDisconnectionComplete;

  // Here we send a HCI_Disconnect command without waiting for it to complete.
  auto status_cb = [](auto id, const EventPacket& event) {
    ZX_DEBUG_ASSERT(event.event_code() == hci_spec::kCommandStatusEventCode);
    hci_is_error(event, TRACE, "hci", "ignoring disconnection failure");
  };

  auto disconn =
      CommandPacket::New(hci_spec::kDisconnect, sizeof(hci_spec::DisconnectCommandParams));
  auto params = disconn->mutable_payload<hci_spec::DisconnectCommandParams>();
  params->connection_handle = htole16(handle());
  params->reason = reason;

  bt_log(DEBUG, "hci", "disconnecting connection (handle: %#.4x, reason: %#.2x)", handle(), reason);

  // Send HCI Disconnect.
  hci_->command_channel()->SendCommand(std::move(disconn), std::move(status_cb),
                                       hci_spec::kCommandStatusEventCode);
}

bool ConnectionImpl::StartEncryption() {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  if (conn_state_ != Connection::State::kConnected) {
    bt_log(DEBUG, "hci", "connection closed; cannot start encryption");
    return false;
  }

  if (ll_type() != bt::LinkType::kLE) {
    return BrEdrStartEncryption();
  }

  if (role() != Role::kMaster) {
    bt_log(DEBUG, "hci", "only the master can start encryption");
    return false;
  }

  if (!ltk()) {
    bt_log(DEBUG, "hci", "connection has no LTK; cannot start encryption");
    return false;
  }

  return LEStartEncryption(*ltk());
}

bool ConnectionImpl::BrEdrStartEncryption() {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());

  ZX_ASSERT(ltk().has_value() == ltk_type().has_value());
  if (!ltk().has_value()) {
    bt_log(DEBUG, "hci", "connection link key type has not been set; not starting encryption");
    return false;
  }

  auto cmd = CommandPacket::New(hci_spec::kSetConnectionEncryption,
                                sizeof(hci_spec::SetConnectionEncryptionCommandParams));
  auto* params = cmd->mutable_payload<hci_spec::SetConnectionEncryptionCommandParams>();
  params->connection_handle = htole16(handle());
  params->encryption_enable = hci_spec::GenericEnableParam::kEnable;

  auto self = weak_ptr_factory_.GetWeakPtr();
  auto status_cb = [self, handle = handle()](auto id, const EventPacket& event) {
    if (!self) {
      return;
    }

    const Status status = event.ToStatus();
    if (!bt_is_error(status, ERROR, "hci-bredr", "could not set encryption on link %#.04x",
                     handle)) {
      bt_log(DEBUG, "hci-bredr", "requested encryption start on %#.04x", handle);
      return;
    }

    if (self->encryption_change_callback()) {
      self->encryption_change_callback()(status, false);
    }
  };

  return hci_->command_channel()->SendCommand(std::move(cmd), std::move(status_cb),
                                              hci_spec::kCommandStatusEventCode) != 0u;
}

bool ConnectionImpl::LEStartEncryption(const hci_spec::LinkKey& ltk) {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  ZX_ASSERT(!ltk_type().has_value());

  // TODO(fxbug.dev/801): Tell the data channel to stop data flow.

  auto cmd = CommandPacket::New(hci_spec::kLEStartEncryption,
                                sizeof(hci_spec::LEStartEncryptionCommandParams));
  auto* params = cmd->mutable_payload<hci_spec::LEStartEncryptionCommandParams>();
  params->connection_handle = htole16(handle());
  params->random_number = htole64(ltk.rand());
  params->encrypted_diversifier = htole16(ltk.ediv());
  params->long_term_key = ltk.value();

  auto self = weak_ptr_factory_.GetWeakPtr();
  auto status_cb = [self, handle = handle()](auto id, const EventPacket& event) {
    if (!self) {
      return;
    }

    const Status status = event.ToStatus();
    if (!bt_is_error(status, ERROR, "hci-le", "could not set encryption on link %#.04x", handle)) {
      bt_log(DEBUG, "hci-le", "requested encryption start on %#.04x", handle);
      return;
    }

    if (self->encryption_change_callback()) {
      self->encryption_change_callback()(status, false);
    }
  };

  return hci_->command_channel()->SendCommand(std::move(cmd), std::move(status_cb),
                                              hci_spec::kCommandStatusEventCode) != 0u;
}

void ConnectionImpl::HandleEncryptionStatus(Status status, bool enabled) {
  // "On an authentication failure, the connection shall be automatically
  // disconnected by the Link Layer." (HCI_LE_Start_Encryption, Vol 2, Part E,
  // 7.8.24). We make sure of this by telling the controller to disconnect.
  //
  // For ACL-U, Vol 3, Part C, 5.2.2.1.1 and 5.2.2.2.1 mention disconnecting the
  // link after pairing failures (supported by TS GAP/SEC/SEM/BV-10-C), but do
  // not specify actions to take after encryption failures. We'll choose to
  // disconnect ACL links after encryption failure.
  if (!status) {
    Disconnect(hci_spec::StatusCode::kAuthenticationFailure);
  } else {
    // TODO(fxbug.dev/801): Tell the data channel to resume data flow.
  }

  if (!encryption_change_callback()) {
    bt_log(DEBUG, "hci", "%#.4x: no encryption status callback assigned", handle());
    return;
  }

  encryption_change_callback()(status, enabled);
}

void ConnectionImpl::ValidateAclEncryptionKeySize(hci::StatusCallback key_size_validity_cb) {
  ZX_ASSERT(ll_type() == bt::LinkType::kACL);
  ZX_ASSERT(conn_state_ == Connection::State::kConnected);

  auto cmd = CommandPacket::New(hci_spec::kReadEncryptionKeySize,
                                sizeof(hci_spec::ReadEncryptionKeySizeParams));
  auto* params = cmd->mutable_payload<hci_spec::ReadEncryptionKeySizeParams>();
  params->connection_handle = htole16(handle());

  auto status_cb = [self = weak_ptr_factory_.GetWeakPtr(),
                    valid_cb = std::move(key_size_validity_cb)](auto, const EventPacket& event) {
    if (!self) {
      return;
    }

    Status status = event.ToStatus();
    if (!bt_is_error(status, ERROR, "hci", "Could not read ACL encryption key size on %#.4x",
                     self->handle())) {
      const auto& return_params =
          *event.return_params<hci_spec::ReadEncryptionKeySizeReturnParams>();
      const auto key_size = return_params.key_size;
      bt_log(TRACE, "hci", "%#.4x: encryption key size %hhu", self->handle(), key_size);

      if (key_size < hci_spec::kMinEncryptionKeySize) {
        bt_log(WARN, "hci", "%#.4x: encryption key size %hhu insufficient", self->handle(),
               key_size);
        status = Status(HostError::kInsufficientSecurity);
      }
    }
    valid_cb(status);
  };

  hci_->command_channel()->SendCommand(std::move(cmd), std::move(status_cb));
}

CommandChannel::EventCallbackResult ConnectionImpl::OnEncryptionChangeEvent(
    const EventPacket& event) {
  ZX_DEBUG_ASSERT(event.event_code() == hci_spec::kEncryptionChangeEventCode);
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());

  if (event.view().payload_size() != sizeof(hci_spec::EncryptionChangeEventParams)) {
    bt_log(WARN, "hci", "malformed encryption change event");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  const auto& params = event.params<hci_spec::EncryptionChangeEventParams>();
  hci_spec::ConnectionHandle handle = le16toh(params.connection_handle);

  // Silently ignore the event as it isn't meant for this connection.
  if (handle != this->handle()) {
    return CommandChannel::EventCallbackResult::kContinue;
  }

  if (conn_state_ != Connection::State::kConnected) {
    bt_log(DEBUG, "hci", "encryption change ignored: connection closed");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  Status status(params.status);
  bool enabled = params.encryption_enabled != hci_spec::EncryptionStatus::kOff;

  bt_log(DEBUG, "hci", "encryption change (%s) %s", enabled ? "enabled" : "disabled",
         status.ToString().c_str());

  if (ll_type() == bt::LinkType::kACL && status && enabled) {
    ValidateAclEncryptionKeySize([this](const Status& key_valid_status) {
      HandleEncryptionStatus(key_valid_status, true /* enabled */);
    });
    return CommandChannel::EventCallbackResult::kContinue;
  }

  HandleEncryptionStatus(status, enabled);

  return CommandChannel::EventCallbackResult::kContinue;
}

CommandChannel::EventCallbackResult ConnectionImpl::OnEncryptionKeyRefreshCompleteEvent(
    const EventPacket& event) {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  ZX_DEBUG_ASSERT(event.event_code() == hci_spec::kEncryptionKeyRefreshCompleteEventCode);

  if (event.view().payload_size() != sizeof(hci_spec::EncryptionKeyRefreshCompleteEventParams)) {
    bt_log(WARN, "hci", "malformed encryption key refresh complete event");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  const auto& params = event.params<hci_spec::EncryptionKeyRefreshCompleteEventParams>();
  hci_spec::ConnectionHandle handle = le16toh(params.connection_handle);

  // Silently ignore this event as it isn't meant for this connection.
  if (handle != this->handle()) {
    return CommandChannel::EventCallbackResult::kContinue;
  }

  if (conn_state_ != Connection::State::kConnected) {
    bt_log(DEBUG, "hci", "encryption key refresh ignored: connection closed");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  Status status(params.status);

  bt_log(DEBUG, "hci", "encryption key refresh %s", status.ToString().c_str());

  // Report that encryption got disabled on failure status. The accuracy of this
  // isn't that important since the link will be disconnected.
  HandleEncryptionStatus(status, static_cast<bool>(status));

  return CommandChannel::EventCallbackResult::kContinue;
}

CommandChannel::EventCallbackResult ConnectionImpl::OnLELongTermKeyRequestEvent(
    const EventPacket& event) {
  ZX_DEBUG_ASSERT(thread_checker_.is_thread_valid());
  ZX_DEBUG_ASSERT(event.event_code() == hci_spec::kLEMetaEventCode);
  ZX_DEBUG_ASSERT(event.params<hci_spec::LEMetaEventParams>().subevent_code ==
                  hci_spec::kLELongTermKeyRequestSubeventCode);

  auto* params = event.le_event_params<hci_spec::LELongTermKeyRequestSubeventParams>();
  if (!params) {
    bt_log(WARN, "hci", "malformed LE LTK request event");
    return CommandChannel::EventCallbackResult::kContinue;
  }

  hci_spec::ConnectionHandle handle = le16toh(params->connection_handle);

  // Silently ignore the event as it isn't meant for this connection.
  if (handle != this->handle()) {
    return CommandChannel::EventCallbackResult::kContinue;
  }

  // TODO(fxbug.dev/1360): Tell the data channel to stop data flow.

  std::unique_ptr<CommandPacket> cmd;

  uint64_t rand = le64toh(params->random_number);
  uint16_t ediv = le16toh(params->encrypted_diversifier);

  bt_log(DEBUG, "hci", "LE LTK request - ediv: %#.4x, rand: %#.16lx", ediv, rand);
  if (ltk() && ltk()->rand() == rand && ltk()->ediv() == ediv) {
    cmd = CommandPacket::New(hci_spec::kLELongTermKeyRequestReply,
                             sizeof(hci_spec::LELongTermKeyRequestReplyCommandParams));
    auto* params = cmd->mutable_payload<hci_spec::LELongTermKeyRequestReplyCommandParams>();

    params->connection_handle = htole16(handle);
    params->long_term_key = ltk()->value();
  } else {
    bt_log(DEBUG, "hci-le", "LTK request rejected");

    cmd = CommandPacket::New(hci_spec::kLELongTermKeyRequestNegativeReply,
                             sizeof(hci_spec::LELongTermKeyRequestNegativeReplyCommandParams));
    auto* params = cmd->mutable_payload<hci_spec::LELongTermKeyRequestNegativeReplyCommandParams>();
    params->connection_handle = htole16(handle);
  }

  auto status_cb = [](auto id, const EventPacket& event) {
    hci_is_error(event, TRACE, "hci-le", "failed to reply to LTK request");
  };
  hci_->command_channel()->SendCommand(std::move(cmd), std::move(status_cb));
  return CommandChannel::EventCallbackResult::kContinue;
}

}  // namespace bt::hci
