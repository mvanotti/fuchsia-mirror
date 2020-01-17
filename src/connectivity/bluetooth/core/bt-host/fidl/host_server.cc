// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "host_server.h"

#include <zircon/assert.h>

#include "fuchsia/bluetooth/control/cpp/fidl.h"
#include "helpers.h"
#include "low_energy_central_server.h"
#include "low_energy_peripheral_server.h"
#include "profile_server.h"
#include "src/connectivity/bluetooth/core/bt-host/common/identifier.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/adapter.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/bonding_data.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/bredr_connection_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/bredr_discovery_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/gap.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/low_energy_address_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/low_energy_discovery_manager.h"
#include "src/connectivity/bluetooth/core/bt-host/gatt_host.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/types.h"
#include "src/connectivity/bluetooth/core/bt-host/sm/util.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/strings/join_strings.h"
#include "src/lib/fxl/strings/string_number_conversions.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace bthost {

namespace fbt = fuchsia::bluetooth;

using bt::PeerId;
using bt::sm::IOCapability;
using fidl_helpers::AddressBytesFromString;
using fidl_helpers::NewFidlError;
using fidl_helpers::PeerIdFromString;
using fidl_helpers::SecurityLevelFromFidl;
using fidl_helpers::StatusToFidl;
using fuchsia::bluetooth::Bool;
using fuchsia::bluetooth::ErrorCode;
using fuchsia::bluetooth::Status;
using fuchsia::bluetooth::control::AdapterState;
using fuchsia::bluetooth::control::BondingData;
using fuchsia::bluetooth::control::PairingOptions;
using fuchsia::bluetooth::control::RemoteDevice;
using fuchsia::bluetooth::control::TechnologyType;

std::pair<PeerTracker::Updated, PeerTracker::Removed> PeerTracker::ToFidl(
    const bt::gap::PeerCache* peer_cache) {
  PeerTracker::Updated updated_fidl;
  for (auto& id : updated_) {
    auto* peer = peer_cache->FindById(id);

    // All ids in |updated_| are assumed to be valid as they would otherwise be in |removed_|.
    ZX_ASSERT(peer);

    updated_fidl.push_back(fidl_helpers::PeerToFidl(*peer));
  }

  PeerTracker::Removed removed_fidl;
  for (auto& id : removed_) {
    removed_fidl.push_back(fbt::PeerId{id.value()});
  }

  return std::make_pair(std::move(updated_fidl), std::move(removed_fidl));
}

void PeerTracker::Update(bt::PeerId id) {
  updated_.insert(id);
  removed_.erase(id);
}

void PeerTracker::Remove(bt::PeerId id) {
  updated_.erase(id);
  removed_.insert(id);
}

WatchPeersGetter::WatchPeersGetter(bt::gap::PeerCache* peer_cache) : peer_cache_(peer_cache) {
  ZX_DEBUG_ASSERT(peer_cache_);
}

void WatchPeersGetter::Notify(std::queue<Callback> callbacks, PeerTracker peers) {
  auto [updated, removed] = peers.ToFidl(peer_cache_);
  while (!callbacks.empty()) {
    auto f = std::move(callbacks.front());
    callbacks.pop();
    f(fidl::Clone(updated), fidl::Clone(removed));
  }
}

HostServer::HostServer(zx::channel channel, fxl::WeakPtr<bt::gap::Adapter> adapter,
                       fbl::RefPtr<GattHost> gatt_host)
    : AdapterServerBase(adapter, this, std::move(channel)),
      pairing_delegate_(nullptr),
      gatt_host_(gatt_host),
      requesting_discovery_(false),
      requesting_discoverable_(false),
      io_capability_(IOCapability::kNoInputNoOutput),
      watch_peers_getter_(adapter->peer_cache()),
      weak_ptr_factory_(this) {
  ZX_DEBUG_ASSERT(gatt_host_);

  auto self = weak_ptr_factory_.GetWeakPtr();
  adapter->peer_cache()->set_peer_updated_callback([self](const auto& peer) {
    if (self) {
      self->OnPeerUpdated(peer);
    }
  });
  adapter->peer_cache()->set_peer_removed_callback([self](const auto& identifier) {
    if (self) {
      self->OnPeerRemoved(identifier);
    }
  });
  adapter->peer_cache()->set_peer_bonded_callback([self](const auto& peer) {
    if (self) {
      self->OnPeerBonded(peer);
    }
  });
  adapter->set_auto_connect_callback([self](auto conn_ref) {
    if (self) {
      self->RegisterLowEnergyConnection(std::move(conn_ref), true);
    }
  });

  // Initialize the HostInfo getter with the initial state.
  NotifyInfoChange();

  // Initialize the peer watcher with all known connectable peers that are in the cache.
  adapter->peer_cache()->ForEach([this](const bt::gap::Peer& peer) { OnPeerUpdated(peer); });
}

HostServer::~HostServer() { Close(); }

void HostServer::WatchState(WatchStateCallback callback) {
  info_getter_.Watch(std::move(callback));
}

void HostServer::SetLocalData(::fuchsia::bluetooth::control::HostData host_data) {
  if (host_data.irk) {
    bt_log(TRACE, "bt-host", "assign IRK");
    auto addr_mgr = adapter()->le_address_manager();
    if (addr_mgr) {
      addr_mgr->set_irk(fidl_helpers::LocalKeyFromFidl(*host_data.irk));
    }
  }
}

void HostServer::WatchPeers(WatchPeersCallback callback) {
  watch_peers_getter_.Watch(std::move(callback));
}

// TODO(35008): Add a unit test for this method.
void HostServer::SetLocalName(::std::string local_name, SetLocalNameCallback callback) {
  ZX_DEBUG_ASSERT(!local_name.empty());
  // Make a copy of |local_name| to move separately into the lambda.
  std::string name_copy(local_name);
  adapter()->SetLocalName(std::move(local_name),
                          [self = weak_ptr_factory_.GetWeakPtr(), local_name = std::move(name_copy),
                           callback = std::move(callback)](auto status) {
                            // Send adapter state update on success and if the connection is still
                            // open.
                            if (status && self) {
                              self->NotifyInfoChange();
                            }
                            callback(StatusToFidl(status, "Can't Set Local Name"));
                          });
}

// TODO(35008): Add a unit test for this method.
void HostServer::SetDeviceClass(fbt::DeviceClass device_class, SetDeviceClassCallback callback) {
  // Device Class values must only contain data in the lower 3 bytes.
  if (device_class.value >= 1 << 24) {
    callback(NewFidlError(ErrorCode::INVALID_ARGUMENTS, "Can't Set Device Class"));
    return;
  }
  bt::DeviceClass dev_class(device_class.value);
  adapter()->SetDeviceClass(dev_class, [callback = std::move(callback)](auto status) {
    callback(fidl_helpers::StatusToFidl(status, "Can't Set Device Class"));
  });
}

void HostServer::StartLEDiscovery(StartDiscoveryCallback callback) {
  auto le_manager = adapter()->le_discovery_manager();
  if (!le_manager) {
    callback(NewFidlError(ErrorCode::BAD_STATE, "Adapter is not initialized yet."));
    return;
  }
  le_manager->StartDiscovery(
      [self = weak_ptr_factory_.GetWeakPtr(), callback = std::move(callback)](auto session) {
        // End the new session if this AdapterServer got destroyed in the
        // mean time (e.g. because the client disconnected).
        if (!self) {
          callback(NewFidlError(ErrorCode::FAILED, "Adapter Shutdown"));
          return;
        }

        if (!self->requesting_discovery_) {
          callback(NewFidlError(ErrorCode::CANCELED, "Request canceled"));
          return;
        }

        if (!session) {
          bt_log(TRACE, "bt-host", "failed to start LE discovery session");
          callback(NewFidlError(ErrorCode::FAILED, "Failed to start LE discovery session"));
          self->bredr_discovery_session_ = nullptr;
          self->requesting_discovery_ = false;
          return;
        }

        // Set up a general-discovery filter for connectable devices.
        // NOTE(armansito): This currently has no effect since peer updates
        // are driven by PeerCache events. |session|'s "result callback" is unused.
        session->filter()->set_connectable(true);
        session->filter()->SetGeneralDiscoveryFlags();

        self->le_discovery_session_ = std::move(session);
        self->requesting_discovery_ = false;

        // Send the adapter state update.
        self->NotifyInfoChange();

        callback(Status());
      });
}

void HostServer::StartDiscovery(StartDiscoveryCallback callback) {
  bt_log(TRACE, "bt-host", "StartDiscovery()");
  ZX_DEBUG_ASSERT(adapter());

  if (le_discovery_session_ || requesting_discovery_) {
    bt_log(TRACE, "bt-host", "discovery already in progress");
    callback(NewFidlError(ErrorCode::IN_PROGRESS, "Discovery already in progress"));
    return;
  }

  requesting_discovery_ = true;
  auto bredr_manager = adapter()->bredr_discovery_manager();
  if (!bredr_manager) {
    StartLEDiscovery(std::move(callback));
    return;
  }
  // TODO(jamuraa): start these in parallel instead of sequence
  bredr_manager->RequestDiscovery(
      [self = weak_ptr_factory_.GetWeakPtr(), callback = std::move(callback)](
          bt::hci::Status status, auto session) mutable {
        if (!self) {
          callback(NewFidlError(ErrorCode::FAILED, "Adapter Shutdown"));
          return;
        }

        if (!self->requesting_discovery_) {
          callback(NewFidlError(ErrorCode::CANCELED, "Request Canceled"));
          return;
        }

        if (!status || !session) {
          bt_log(TRACE, "bt-host", "failed to start BR/EDR discovery session");
          callback(StatusToFidl(status, "Failed to start BR/EDR discovery session"));
          self->requesting_discovery_ = false;
          return;
        }

        self->bredr_discovery_session_ = std::move(session);
        self->StartLEDiscovery(std::move(callback));
      });
}

void HostServer::StopDiscovery(StopDiscoveryCallback callback) {
  bt_log(TRACE, "bt-host", "StopDiscovery()");
  if (!le_discovery_session_) {
    bt_log(TRACE, "bt-host", "no active discovery session");
    callback(NewFidlError(ErrorCode::BAD_STATE, "No discovery session in progress"));
    return;
  }

  bredr_discovery_session_ = nullptr;
  le_discovery_session_ = nullptr;
  NotifyInfoChange();

  callback(Status());
}

void HostServer::SetConnectable(bool connectable, SetConnectableCallback callback) {
  bt_log(TRACE, "bt-host", "SetConnectable(%s)", connectable ? "true" : "false");

  auto bredr_conn_manager = adapter()->bredr_connection_manager();
  if (!bredr_conn_manager) {
    callback(NewFidlError(ErrorCode::NOT_SUPPORTED, "Connectable mode not available"));
    return;
  }
  bredr_conn_manager->SetConnectable(
      connectable,
      [callback = std::move(callback)](const auto& status) { callback(StatusToFidl(status)); });
}

void HostServer::AddBondedDevices(::std::vector<BondingData> bonds,
                                  AddBondedDevicesCallback callback) {
  bt_log(TRACE, "bt-host", "AddBondedDevices");
  if (bonds.empty()) {
    callback(NewFidlError(ErrorCode::NOT_SUPPORTED, "No bonds were added"));
    return;
  }

  std::vector<std::string> failed_ids;

  for (auto& bond : bonds) {
    auto peer_id = PeerIdFromString(bond.identifier);
    if (!peer_id) {
      failed_ids.push_back(bond.identifier);
      continue;
    }

    std::optional<std::string> peer_name;
    if (bond.name) {
      peer_name = std::move(bond.name);
    }

    bt::DeviceAddress address;
    bt::sm::PairingData le_bond_data;
    if (bond.le) {
      if (bond.bredr && bond.le->address != bond.bredr->address) {
        bt_log(ERROR, "bt-host", "Dual-mode bonding data mismatched (id: %s)",
               bond.identifier.c_str());
        failed_ids.push_back(bond.identifier);
        continue;
      }
      le_bond_data = fidl_helpers::PairingDataFromFidl(*bond.le);

      // The |identity_address| field in bt::sm::PairingData is optional
      // however it is not nullable in the FIDL struct. Hence it must be
      // present.
      ZX_DEBUG_ASSERT(le_bond_data.identity_address);
      address = *le_bond_data.identity_address;
    }

    std::optional<bt::sm::LTK> bredr_link_key;
    if (bond.bredr) {
      // Dual-mode peers will have a BR/EDR-typed address.
      auto addr = AddressBytesFromString(bond.bredr->address);
      ZX_DEBUG_ASSERT(addr);
      address = bt::DeviceAddress(bt::DeviceAddress::Type::kBREDR, *addr);
      bredr_link_key = fidl_helpers::BrEdrKeyFromFidl(*bond.bredr);
    }

    if (!bond.le && !bond.bredr) {
      bt_log(ERROR, "bt-host", "Required bonding data missing (id: %s)", bond.identifier.c_str());
      failed_ids.push_back(bond.identifier);
      continue;
    }

    // TODO(armansito): BondingData should contain the identity address for both
    // transports instead of storing them separately. For now use the one we
    // obtained from |bond.le|.
    if (!adapter()->AddBondedPeer(
            bt::gap::BondingData{*peer_id, address, peer_name, le_bond_data, bredr_link_key})) {
      failed_ids.push_back(bond.identifier);
      continue;
    }
  }

  if (!failed_ids.empty()) {
    callback(fidl_helpers::NewFidlError(
        ErrorCode::FAILED, fxl::StringPrintf("Some peers failed to load (ids: %s)",
                                             fxl::JoinStrings(failed_ids, ", ").c_str())));
  } else {
    callback(Status());
  }
}

void HostServer::OnPeerBonded(const bt::gap::Peer& peer) {
  bt_log(TRACE, "bt-host", "OnPeerBonded()");
  binding()->events().OnNewBondingData(fidl_helpers::NewBondingData(*adapter(), peer));
}

void HostServer::RegisterLowEnergyConnection(bt::gap::LowEnergyConnectionRefPtr conn_ref,
                                             bool auto_connect) {
  ZX_DEBUG_ASSERT(conn_ref);

  bt::PeerId id = conn_ref->peer_identifier();
  auto iter = le_connections_.find(id);
  if (iter != le_connections_.end()) {
    bt_log(WARN, "bt-host", "peer already connected; reference dropped");
    return;
  }

  bt_log(TRACE, "bt-host", "LE peer connected (%s): %s ", (auto_connect ? "auto" : "direct"),
         bt_str(id));
  conn_ref->set_closed_callback([self = weak_ptr_factory_.GetWeakPtr(), id] {
    if (self)
      self->le_connections_.erase(id);
  });
  le_connections_[id] = std::move(conn_ref);
}

void HostServer::SetDiscoverable(bool discoverable, SetDiscoverableCallback callback) {
  bt_log(TRACE, "bt-host", "SetDiscoverable(%s)", discoverable ? "true" : "false");
  // TODO(NET-830): advertise LE here
  if (!discoverable) {
    bredr_discoverable_session_ = nullptr;
    NotifyInfoChange();
    callback(Status());
    return;
  }
  if (discoverable && requesting_discoverable_) {
    bt_log(TRACE, "bt-host", "SetDiscoverable already in progress");
    callback(NewFidlError(ErrorCode::IN_PROGRESS, "SetDiscoverable already in progress"));
    return;
  }
  requesting_discoverable_ = true;
  auto bredr_manager = adapter()->bredr_discovery_manager();
  if (!bredr_manager) {
    callback(NewFidlError(ErrorCode::FAILED, "Discoverable mode not available"));
    return;
  }
  bredr_manager->RequestDiscoverable(
      [self = weak_ptr_factory_.GetWeakPtr(), callback = std::move(callback)](
          bt::hci::Status status, auto session) {
        if (!self) {
          callback(NewFidlError(ErrorCode::FAILED, "Adapter Shutdown"));
          return;
        }

        if (!self->requesting_discoverable_) {
          callback(NewFidlError(ErrorCode::CANCELED, "Request canceled"));
          return;
        }

        if (!status || !session) {
          bt_log(TRACE, "bt-host", "failed to set discoverable");
          callback(StatusToFidl(status, "Failed to set discoverable"));
          self->requesting_discoverable_ = false;
          return;
        }

        self->bredr_discoverable_session_ = std::move(session);
        self->requesting_discoverable_ = false;
        self->NotifyInfoChange();
        callback(Status());
      });
}

void HostServer::EnableBackgroundScan(bool enabled) {
  bt_log(TRACE, "bt-host", "%s background scan", (enabled ? "enable" : "disable"));
  auto le_manager = adapter()->le_discovery_manager();
  if (le_manager) {
    le_manager->EnableBackgroundScan(enabled);
  }
}

void HostServer::EnablePrivacy(bool enabled) {
  bt_log(TRACE, "bt-host", "%s LE privacy", (enabled ? "enable" : "disable"));
  auto addr_mgr = adapter()->le_address_manager();
  if (addr_mgr) {
    addr_mgr->EnablePrivacy(enabled);
  }
}

void HostServer::SetPairingDelegate(
    ::fuchsia::bluetooth::control::InputCapabilityType input,
    ::fuchsia::bluetooth::control::OutputCapabilityType output,
    ::fidl::InterfaceHandle<::fuchsia::bluetooth::control::PairingDelegate> delegate) {
  bool cleared = !delegate;
  pairing_delegate_.Bind(std::move(delegate));

  if (cleared) {
    bt_log(TRACE, "bt-host", "PairingDelegate cleared");
    ResetPairingDelegate();
    return;
  }

  io_capability_ = fidl_helpers::IoCapabilityFromFidl(input, output);
  bt_log(TRACE, "bt-host", "PairingDelegate assigned (I/O capability: %s)",
         bt::sm::util::IOCapabilityToString(io_capability_).c_str());

  auto self = weak_ptr_factory_.GetWeakPtr();
  adapter()->SetPairingDelegate(self);
  pairing_delegate_.set_error_handler([self](zx_status_t status) {
    bt_log(TRACE, "bt-host", "PairingDelegate disconnected");
    if (self) {
      self->ResetPairingDelegate();
    }
  });
}

// Attempt to connect to peer identified by |peer_id|. The peer must be
// in our peer cache. We will attempt to connect technologies (LowEnergy,
// Classic or Dual-Mode) as the peer claims to support when discovered
void HostServer::Connect(::std::string peer_id, ConnectCallback callback) {
  auto id = PeerIdFromString(peer_id);
  if (!id.has_value()) {
    callback(NewFidlError(ErrorCode::INVALID_ARGUMENTS, "invalid peer ID"));
    return;
  }
  auto peer = adapter()->peer_cache()->FindById(*id);
  if (!peer) {
    // We don't support connecting to peers that are not in our cache
    callback(NewFidlError(ErrorCode::NOT_FOUND, "Cannot find peer with the given ID"));
    return;
  }

  // TODO(BT-649): Dual-mode currently not supported; if the peer supports
  // LowEnergy we assume LE. If a dual-mode peer, we should attempt to connect
  // both protocols.
  if (!peer->le()) {
    ConnectBrEdr(*id, std::move(callback));
    return;
  }

  ConnectLowEnergy(*id, std::move(callback));
}

// Attempt to disconnect the peer identified by |peer_id| from all transports.
// If the peer is already not connected, return success. If the peer is
// disconnected succesfully, return success.
void HostServer::Disconnect(::std::string peer_id, DisconnectCallback callback) {
  auto id = PeerIdFromString(peer_id);
  if (!id.has_value()) {
    callback(NewFidlError(ErrorCode::INVALID_ARGUMENTS, "invalid peer ID"));
    return;
  }

  auto le_disc = adapter()->le_connection_manager()->Disconnect(*id);
  auto bredr_disc = adapter()->bredr_connection_manager()->Disconnect(*id);

  if (le_disc && bredr_disc) {
    callback(Status());
  } else {
    if (le_disc)
      callback(NewFidlError(ErrorCode::UNKNOWN, "Failed to disconnect from Br/Edr"));
    else if (bredr_disc)
      callback(NewFidlError(ErrorCode::UNKNOWN, "Failed to disconnect from LE"));
    else
      callback(NewFidlError(ErrorCode::UNKNOWN, "Failed to disconnect from both LE and Br/Edr"));
  }
}

void HostServer::ConnectLowEnergy(PeerId peer_id, ConnectCallback callback) {
  auto self = weak_ptr_factory_.GetWeakPtr();
  auto on_complete = [self, callback = std::move(callback), peer_id](auto status, auto connection) {
    if (!status) {
      ZX_ASSERT(!connection);
      bt_log(TRACE, "bt-host", "failed to connect to peer (id %s)", bt_str(peer_id));
      callback(StatusToFidl(status, "failed to connect"));
      return;
    }

    // We must be connected and to the right peer
    ZX_ASSERT(connection);
    ZX_ASSERT(peer_id == connection->peer_identifier());

    callback(Status());

    if (self)
      self->RegisterLowEnergyConnection(std::move(connection), false);
  };
  if (!adapter()->le_connection_manager()->Connect(peer_id, std::move(on_complete))) {
    callback(NewFidlError(ErrorCode::FAILED, "failed to connect"));
  }
}

// Initiate an outgoing Br/Edr connection, unless already connected
// Br/Edr connections are host-wide, and stored in BrEdrConnectionManager
void HostServer::ConnectBrEdr(PeerId peer_id, ConnectCallback callback) {
  auto on_complete = [callback = std::move(callback), peer_id](auto status, auto connection) {
    if (!status) {
      ZX_ASSERT(!connection);
      bt_log(TRACE, "bt-host", "failed to connect to peer (id %s)", bt_str(peer_id));
      callback(StatusToFidl(status, "failed to connect"));
      return;
    }

    // We must be connected and to the right peer
    ZX_ASSERT(connection);
    ZX_ASSERT(peer_id == connection->peer_id());

    callback(Status());
  };

  if (!adapter()->bredr_connection_manager()->Connect(peer_id, std::move(on_complete))) {
    callback(NewFidlError(ErrorCode::FAILED, "failed to connect"));
  }
}

void HostServer::Forget(::std::string peer_id, ForgetCallback callback) {
  auto id = PeerIdFromString(peer_id);
  if (!id.has_value()) {
    callback(NewFidlError(ErrorCode::INVALID_ARGUMENTS, "Invalid peer ID"));
    return;
  }
  auto peer = adapter()->peer_cache()->FindById(*id);
  if (!peer) {
    bt_log(TRACE, "bt-host", "peer %s to forget wasn't found", peer_id.c_str());
    callback(Status());
    return;
  }

  const bool le_disconnected = adapter()->le_connection_manager()->Disconnect(*id);
  const bool bredr_disconnected = adapter()->bredr_connection_manager()->Disconnect(*id);
  const bool peer_removed = adapter()->peer_cache()->RemoveDisconnectedPeer(*id);

  if (!le_disconnected || !bredr_disconnected) {
    const auto message =
        fxl::StringPrintf("Link(s) failed to close:%s%s", le_disconnected ? "" : " LE",
                          bredr_disconnected ? "" : " BR/EDR");
    callback(NewFidlError(ErrorCode::FAILED, message));
  } else {
    ZX_ASSERT(peer_removed);
    callback(Status());
  }
}

void HostServer::Pair(fuchsia::bluetooth::PeerId id,
                      fuchsia::bluetooth::control::PairingOptions options, PairCallback callback) {
  auto peer_id = bt::PeerId(id.value);
  auto peer = adapter()->peer_cache()->FindById(peer_id);
  if (!peer) {
    // We don't support pairing to peers that are not in our cache
    callback(NewFidlError(ErrorCode::NOT_FOUND, "Cannot find peer with the given ID"));
    return;
  }
  // If options specifies a transport preference for LE or BR/EDR, we use that. Otherwise, we use
  // whatever transport exists, defaulting to LE for dual-mode connections.
  bool pair_bredr = !peer->le();
  if (options.has_transport() && options.transport() != TechnologyType::DUAL_MODE) {
    pair_bredr = (options.transport() == TechnologyType::CLASSIC);
  }
  if (pair_bredr) {
    PairBrEdr(peer_id, std::move(callback));
    return;
  }
  PairLowEnergy(peer_id, std::move(options), std::move(callback));
}

void HostServer::PairLowEnergy(PeerId peer_id, PairingOptions options, PairCallback callback) {
  std::optional<bt::sm::SecurityLevel> security_level;
  if (options.has_le_security_level()) {
    security_level = SecurityLevelFromFidl(options.le_security_level());
    if (!security_level.has_value()) {
      callback(NewFidlError(ErrorCode::INVALID_ARGUMENTS, "invalid security level"));
      return;
    }
  } else {
    security_level = bt::sm::SecurityLevel::kAuthenticated;
  }
  auto on_complete = [peer_id, callback = std::move(callback)](bt::sm::Status status) {
    if (!status) {
      bt_log(WARN, "bt-host", "failed to pair to peer (id %s)", bt_str(peer_id));
      callback(NewFidlError(fidl_helpers::HostErrorToFidl(status.error()), "Failed to pair"));
      return;
    }
    callback(Status());
  };
  adapter()->le_connection_manager()->Pair(peer_id, *security_level, std::move(on_complete));
}

void HostServer::PairBrEdr(PeerId peer_id, PairCallback callback) {
  auto on_complete = [peer_id, callback = std::move(callback)](bt::hci::Status status) {
    if (!status) {
      bt_log(WARN, "bt-host", "failed to pair to peer (id %s)", bt_str(peer_id));
      callback(NewFidlError(fidl_helpers::HostErrorToFidl(status.error()), "Failed to pair"));
      return;
    }
    callback(Status());
  };
  adapter()->bredr_connection_manager()->Pair(peer_id, std::move(on_complete));
}

void HostServer::RequestLowEnergyCentral(
    fidl::InterfaceRequest<fuchsia::bluetooth::le::Central> request) {
  BindServer<LowEnergyCentralServer>(std::move(request), gatt_host_);
}

void HostServer::RequestLowEnergyPeripheral(
    fidl::InterfaceRequest<fuchsia::bluetooth::le::Peripheral> request) {
  BindServer<LowEnergyPeripheralServer>(std::move(request));
}

void HostServer::RequestGattServer(
    fidl::InterfaceRequest<fuchsia::bluetooth::gatt::Server> request) {
  // GATT FIDL requests are handled by GattHost.
  gatt_host_->BindGattServer(std::move(request));
}

void HostServer::RequestProfile(
    fidl::InterfaceRequest<fuchsia::bluetooth::bredr::Profile> request) {
  BindServer<ProfileServer>(std::move(request));
}

void HostServer::Close() {
  bt_log(TRACE, "bt-host", "closing FIDL handles");

  // Invalidate all weak pointers. This will guarantee that all pending tasks
  // that reference this HostServer will return early if they run in the future.
  weak_ptr_factory_.InvalidateWeakPtrs();

  // Destroy all FIDL bindings.
  servers_.clear();
  gatt_host_->CloseServers();

  // Cancel pending requests.
  requesting_discovery_ = false;
  requesting_discoverable_ = false;

  le_discovery_session_ = nullptr;
  bredr_discovery_session_ = nullptr;
  bredr_discoverable_session_ = nullptr;

  // Drop all connections that are attached to this HostServer.
  le_connections_.clear();

  // Stop background scan if enabled.
  auto le_manager = adapter()->le_discovery_manager();
  if (le_manager) {
    le_manager->EnableBackgroundScan(false);
  }
  auto addr_mgr = adapter()->le_address_manager();
  if (addr_mgr) {
    addr_mgr->EnablePrivacy(false);
    addr_mgr->set_irk(std::nullopt);
  }

  // Disallow future pairing.
  pairing_delegate_ = nullptr;
  ResetPairingDelegate();

  // Send adapter state change.
  if (binding()->is_bound()) {
    NotifyInfoChange();
  }
}

bt::sm::IOCapability HostServer::io_capability() const {
  bt_log(TRACE, "bt-host", "I/O capability: %s",
         bt::sm::util::IOCapabilityToString(io_capability_).c_str());
  return io_capability_;
}

void HostServer::CompletePairing(PeerId id, bt::sm::Status status) {
  bt_log(TRACE, "bt-host", "pairing complete for peer: %s, status: %s", bt_str(id), bt_str(status));
  ZX_DEBUG_ASSERT(pairing_delegate_);
  pairing_delegate_->OnPairingComplete(id.ToString(), StatusToFidl(status));
}

void HostServer::ConfirmPairing(PeerId id, ConfirmCallback confirm) {
  bt_log(TRACE, "bt-host", "pairing confirmation request for peer: %s", bt_str(id));
  DisplayPairingRequest(id, std::nullopt, fuchsia::bluetooth::control::PairingMethod::CONSENT,
                        std::move(confirm));
}

void HostServer::DisplayPasskey(PeerId id, uint32_t passkey, DisplayMethod method,
                                ConfirmCallback confirm) {
  auto fidl_method = fuchsia::bluetooth::control::PairingMethod::PASSKEY_DISPLAY;
  if (method == DisplayMethod::kComparison) {
    bt_log(TRACE, "bt-host", "compare passkey %06u on peer: %s", passkey, bt_str(id));
    fidl_method = fuchsia::bluetooth::control::PairingMethod::PASSKEY_COMPARISON;
  } else {
    bt_log(TRACE, "bt-host", "enter passkey %06u on peer: %s", passkey, bt_str(id));
  }
  DisplayPairingRequest(id, passkey, fidl_method, std::move(confirm));
}

void HostServer::RequestPasskey(PeerId id, PasskeyResponseCallback respond) {
  bt_log(TRACE, "bt-host", "passkey request for peer: %s", bt_str(id));
  auto found_peer = adapter()->peer_cache()->FindById(id);
  ZX_ASSERT(found_peer);
  auto device = fidl_helpers::NewRemoteDevicePtr(*found_peer);
  ZX_ASSERT(device);

  ZX_ASSERT(pairing_delegate_);
  pairing_delegate_->OnPairingRequest(
      std::move(*device), fuchsia::bluetooth::control::PairingMethod::PASSKEY_ENTRY, nullptr,
      [respond = std::move(respond)](const bool success, fidl::StringPtr passkey) {
        bt_log(TRACE, "bt-host", "got peer response: %s, \"%s\"", success ? "success" : "failure",
               passkey.has_value() ? passkey.value().c_str() : "(null)");
        if (!success) {
          respond(-1);
          return;
        }
        if (!passkey.has_value()) {
          bt_log(WARN, "bt-host", "Passkey not supplied");
          respond(-1);
          return;
        }
        uint32_t response;
        if (!fxl::StringToNumberWithError<uint32_t>(passkey.value(), &response)) {
          bt_log(WARN, "bt-host", "Unrecognized integer in string: \"%s\"",
                 passkey.value().c_str());
          respond(-1);
          return;
        }
        bt_log(SPEW, "bt-host", "got peer passkey: \"%u\"", response);
        respond(response);
      });
}

void HostServer::DisplayPairingRequest(bt::PeerId id, std::optional<uint32_t> passkey,
                                       fuchsia::bluetooth::control::PairingMethod method,
                                       ConfirmCallback confirm) {
  auto found_peer = adapter()->peer_cache()->FindById(id);
  ZX_ASSERT(found_peer);
  auto device = fidl_helpers::NewRemoteDevicePtr(*found_peer);
  ZX_ASSERT(device);

  ZX_ASSERT(pairing_delegate_);
  fidl::StringPtr displayed_passkey = nullptr;
  if (passkey) {
    displayed_passkey = fxl::StringPrintf("%06u", *passkey);
  }
  pairing_delegate_->OnPairingRequest(
      std::move(*device), method, displayed_passkey,
      [confirm = std::move(confirm)](const bool success, fidl::StringPtr passkey) {
        bt_log(TRACE, "bt-host", "got peer response: %s, \"%s\"", success ? "success" : "failure",
               passkey.has_value() ? passkey.value().c_str() : "(null)");
        confirm(success);
      });
}

void HostServer::OnConnectionError(Server* server) {
  ZX_DEBUG_ASSERT(server);
  servers_.erase(server);
}

void HostServer::OnPeerUpdated(const bt::gap::Peer& peer) {
  if (!peer.connectable()) {
    return;
  }

  watch_peers_getter_.Transform([id = peer.identifier()](auto tracker) {
    tracker.Update(id);
    return tracker;
  });
}

void HostServer::OnPeerRemoved(bt::PeerId id) {
  // TODO(armansito): Notify only if the peer is connectable for symmetry with
  // OnPeerUpdated?
  watch_peers_getter_.Transform([id](auto tracker) {
    tracker.Remove(id);
    return tracker;
  });
}

void HostServer::ResetPairingDelegate() {
  io_capability_ = IOCapability::kNoInputNoOutput;
  adapter()->SetPairingDelegate(fxl::WeakPtr<HostServer>());
}

void HostServer::NotifyInfoChange() { info_getter_.Set(fidl_helpers::HostInfoToFidl(*adapter())); }

}  // namespace bthost
