// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_MLME_RUST_C_BINDING_BINDINGS_H_
#define SRC_CONNECTIVITY_WLAN_LIB_MLME_RUST_C_BINDING_BINDINGS_H_

// Warning:
// This file was autogenerated by cbindgen.
// Do not modify this file manually.

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <ddk/protocol/wlan/info.h>
#include <ddk/protocol/wlan/mac.h>
#include <garnet/lib/wlan/protocol/include/wlan/protocol/mac.h>

typedef struct wlan_ap_sta_t wlan_ap_sta_t;

/**
 * A STA running in Client mode.
 * The Client STA is in its early development process and does not yet manage its internal state
 * machine or track negotiated capabilities.
 */
typedef struct wlan_client_sta_t wlan_client_sta_t;

typedef struct wlan_client_mlme_t wlan_client_mlme_t;

/**
 * Manages all SNS for a STA.
 */
typedef struct mlme_sequence_manager_t mlme_sequence_manager_t;

/**
 * A convenient C-wrapper for read-only memory that is neither owned or managed by Rust
 */
typedef struct {
  const uint8_t *data;
  uintptr_t size;
} wlan_span_t;

/**
 * An output buffer requires its owner to manage the underlying buffer's memory themselves.
 * An output buffer is used for every buffer handed from Rust to C++.
 */
typedef struct {
  /**
   * Pointer to the buffer's underlying data structure.
   */
  void *raw;
  /**
   * Pointer to the start of the buffer's data portion and the amount of bytes written.
   */
  uint8_t *data;
  uintptr_t written_bytes;
} mlme_out_buf_t;

/**
 * A `Device` allows transmitting frames and MLME messages.
 */
typedef struct {
  void *device;
  /**
   * Request to deliver an Ethernet II frame to Fuchsia's Netstack.
   */
  int32_t (*deliver_eth_frame)(void *device, const uint8_t *data, uintptr_t len);
  /**
   * Request to deliver a WLAN frame over the air.
   */
  int32_t (*send_wlan_frame)(void *device, mlme_out_buf_t buf, uint32_t flags);
  /**
   * Returns an unowned channel handle to MLME's SME peer, or ZX_HANDLE_INVALID
   * if no SME channel is available.
   */
  uint32_t (*get_sme_channel)(void *device);
  /**
   * Returns the currently set WLAN channel.
   */
  wlan_channel_t (*get_wlan_channel)(void *device);
  /**
   * Request the PHY to change its channel. If successful, get_wlan_channel will return the
   * chosen channel.
   */
  int32_t (*set_wlan_channel)(void *device, wlan_channel_t channel);
  /**
   * Set a key on the device.
   * |key| is mutable because the underlying API does not take a const wlan_key_config_t.
   */
  int32_t (*set_key)(void *device, wlan_key_config_t *key);
  /**
   * Make scan request to the driver
   */
  int32_t (*start_hw_scan)(void *device, const wlan_hw_scan_config_t *config);
  /**
   * Get information and capabilities of this WLAN interface
   */
  wlanmac_info_t (*get_wlan_info)(void *device);
  /**
   * Configure the device's BSS.
   * |cfg| is mutable because the underlying API does not take a const wlan_bss_config_t.
   */
  int32_t (*configure_bss)(void *device, wlan_bss_config_t *cfg);
  /**
   * Enable hardware offload of beaconing on the device.
   */
  int32_t (*enable_beaconing)(void *device, const uint8_t *beacon_tmpl_data,
                              uintptr_t beacon_tmpl_len, uintptr_t tim_ele_offset,
                              uint16_t beacon_interval);
  /**
   * Disable beaconing on the device.
   */
  int32_t (*disable_beaconing)(void *device);
  /**
   * Sets the link status to be UP or DOWN.
   */
  int32_t (*set_link_status)(void *device, uint8_t status);
  /**
   * Configure the association context.
   * |assoc_ctx| is mutable because the underlying API does not take a const wlan_assoc_ctx_t.
   */
  int32_t (*configure_assoc)(void *device, wlan_assoc_ctx_t *assoc_ctx);
  /**
   * Clear the association context.
   */
  int32_t (*clear_assoc)(void *device, const uint8_t (*addr)[6]);
} mlme_device_ops_t;

/**
 * An input buffer will always be returned to its original owner when no longer being used.
 * An input buffer is used for every buffer handed from C++ to Rust.
 */
typedef struct {
  /**
   * Returns the buffer's ownership and free it.
   */
  void (*free_buffer)(void *raw);
  /**
   * Pointer to the buffer's underlying data structure.
   */
  void *raw;
  /**
   * Pointer to the start of the buffer's data portion and its length.
   */
  uint8_t *data;
  uintptr_t len;
} mlme_in_buf_t;

typedef struct {
  /**
   * Acquire a `InBuf` with a given minimum length from the provider.
   * The provider must release the underlying buffer's ownership and transfer it to this crate.
   * The buffer will be returned via the `free_buffer` callback when it's no longer used.
   */
  mlme_in_buf_t (*get_buffer)(uintptr_t min_len);
} mlme_buffer_provider_ops_t;

typedef struct {
  uint64_t _0;
} wlan_scheduler_event_id_t;

/**
 * A scheduler to schedule and cancel timeouts.
 */
typedef struct {
  void *cookie;
  /**
   * Returns the current system time in nano seconds.
   */
  int64_t (*now)(void *cookie);
  /**
   * Requests to schedule an event. Returns a a unique ID used to cancel the scheduled event.
   */
  wlan_scheduler_event_id_t (*schedule)(void *cookie, int64_t deadline);
  /**
   * Cancels a previously scheduled event.
   */
  void (*cancel)(void *cookie, wlan_scheduler_event_id_t id);
} wlan_scheduler_ops_t;

/**
 * ClientConfig affects time duration used for different timeouts.
 * Originally added to more easily control behavior in tests.
 */
typedef struct {
  uintptr_t signal_report_beacon_timeout;
  zx_duration_t ensure_on_channel_time;
} wlan_client_mlme_config_t;

/**
 * Proxy to C++ channel scheduler
 */
typedef struct {
  void *chan_sched;
  /**
   * Make sure that MLME remains on the main channel until deadline
   */
  void (*ensure_on_channel)(void *chan_sched, zx_time_t end);
} wlan_cpp_chan_sched_t;

/**
 * The power management state of a station.
 *
 * Represents the possible power states from IEEE-802.11-2016, 11.2.7.
 */
typedef struct {
  bool _0;
} wlan_power_state_t;

extern "C" void ap_sta_delete(wlan_ap_sta_t *sta);

extern "C" int32_t ap_sta_handle_eth_frame(wlan_ap_sta_t *sta, const uint8_t (*dst_addr)[6],
                                           const uint8_t (*src_addr)[6], uint16_t ether_type,
                                           wlan_span_t body);

extern "C" int32_t ap_sta_handle_mac_frame(wlan_ap_sta_t *sta, wlan_span_t frame,
                                           bool body_aligned);

extern "C" int32_t ap_sta_handle_mlme_msg(wlan_ap_sta_t *sta, wlan_span_t bytes);

extern "C" wlan_ap_sta_t *ap_sta_new(mlme_device_ops_t device,
                                     mlme_buffer_provider_ops_t buf_provider,
                                     wlan_scheduler_ops_t scheduler, const uint8_t (*bssid)[6]);

extern "C" void ap_sta_timeout_fired(wlan_ap_sta_t *sta, wlan_scheduler_event_id_t event_id);

extern "C" void client_mlme_delete(wlan_client_mlme_t *mlme);

extern "C" int32_t client_mlme_handle_mlme_msg(wlan_client_mlme_t *mlme, wlan_span_t bytes);

extern "C" wlan_client_mlme_t *client_mlme_new(wlan_client_mlme_config_t config,
                                               mlme_device_ops_t device,
                                               mlme_buffer_provider_ops_t buf_provider,
                                               wlan_scheduler_ops_t scheduler,
                                               wlan_cpp_chan_sched_t cpp_chan_sched);

extern "C" mlme_sequence_manager_t *client_mlme_seq_mgr(wlan_client_mlme_t *mlme);

extern "C" void client_mlme_timeout_fired(wlan_client_mlme_t *mlme, wlan_client_sta_t *sta,
                                          wlan_scheduler_event_id_t event_id);

extern "C" void client_sta_delete(wlan_client_sta_t *sta);

extern "C" int32_t client_sta_handle_data_frame(wlan_client_sta_t *sta, wlan_client_mlme_t *mlme,
                                                wlan_span_t data_frame, bool has_padding,
                                                bool controlled_port_open);

extern "C" wlan_client_sta_t *client_sta_new(const uint8_t (*bssid)[6],
                                             const uint8_t (*iface_mac)[6], bool is_rsn);

extern "C" int32_t client_sta_send_assoc_req_frame(wlan_client_sta_t *sta, wlan_client_mlme_t *mlme,
                                                   uint16_t cap_info, wlan_span_t ssid,
                                                   wlan_span_t rates, wlan_span_t rsne,
                                                   wlan_span_t ht_cap, wlan_span_t vht_cap);

extern "C" int32_t client_sta_send_data_frame(wlan_client_sta_t *sta, wlan_client_mlme_t *mlme,
                                              const uint8_t (*src)[6], const uint8_t (*dest)[6],
                                              bool is_protected, bool is_qos, uint16_t ether_type,
                                              wlan_span_t payload);

extern "C" int32_t client_sta_send_deauth_frame(wlan_client_sta_t *sta, wlan_client_mlme_t *mlme,
                                                uint16_t reason_code);

extern "C" void client_sta_send_eapol_frame(wlan_client_sta_t *sta, wlan_client_mlme_t *mlme,
                                            const uint8_t (*src)[6], const uint8_t (*dest)[6],
                                            bool is_protected, wlan_span_t payload);

extern "C" int32_t client_sta_send_open_auth_frame(wlan_client_sta_t *sta,
                                                   wlan_client_mlme_t *mlme);

extern "C" int32_t client_sta_send_power_state_frame(wlan_client_sta_t *sta,
                                                     wlan_client_mlme_t *mlme,
                                                     wlan_power_state_t state);

extern "C" int32_t client_sta_send_ps_poll_frame(wlan_client_sta_t *sta, wlan_client_mlme_t *mlme,
                                                 uint16_t aid);

extern "C" int32_t mlme_is_valid_open_auth_resp(wlan_span_t auth_resp);

extern "C" void mlme_sequence_manager_delete(mlme_sequence_manager_t *mgr);

extern "C" mlme_sequence_manager_t *mlme_sequence_manager_new(void);

extern "C" uint32_t mlme_sequence_manager_next_sns1(mlme_sequence_manager_t *mgr,
                                                    const uint8_t (*sta_addr)[6]);

extern "C" uint32_t mlme_sequence_manager_next_sns2(mlme_sequence_manager_t *mgr,
                                                    const uint8_t (*sta_addr)[6], uint16_t tid);

#endif /* SRC_CONNECTIVITY_WLAN_LIB_MLME_RUST_C_BINDING_BINDINGS_H_ */
