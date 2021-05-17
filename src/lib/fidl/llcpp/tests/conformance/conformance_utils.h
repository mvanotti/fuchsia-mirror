// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_FIDL_LLCPP_TESTS_CONFORMANCE_CONFORMANCE_UTILS_H_
#define SRC_LIB_FIDL_LLCPP_TESTS_CONFORMANCE_CONFORMANCE_UTILS_H_

#include <lib/fidl/llcpp/coding.h>
#include <lib/fidl/llcpp/message.h>
#include <lib/fidl/llcpp/traits.h>
#include <zircon/fidl.h>
#include <zircon/status.h>
#include <zircon/types.h>

#ifdef __Fuchsia__
#include <lib/zx/channel.h>
#endif

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#ifndef __Fuchsia__
// The current build rules for zircon/system/ulib/zircon don't allow linking
// zx_status_get_string on host. Consider changing in the future.
#define zx_status_get_string(status) ((status))
#endif

// Testing utilities indended for GIDL-generated conformance tests.
namespace llcpp_conformance_utils {

inline bool operator==(zx_handle_disposition_t a, zx_handle_disposition_t b) {
  return a.operation == b.operation && a.handle == b.handle && a.type == b.type &&
         a.rights == b.rights && a.result == b.result;
}
inline bool operator!=(zx_handle_disposition_t a, zx_handle_disposition_t b) { return !(a == b); }
inline std::ostream& operator<<(std::ostream& os, const zx_handle_disposition_t& hd) {
  return os << "zx_handle_disposition_t{\n"
            << "  .operation = " << hd.operation << "\n"
            << "  .handle = " << hd.handle << "\n"
            << "  .type = " << hd.type << "\n"
            << "  .rights = " << hd.rights << "\n"
            << "  .result = " << hd.result << "\n"
            << "}\n";
}

template <typename T>
bool ComparePayload(const T* actual, size_t actual_size, const T* expected, size_t expected_size) {
  bool pass = true;
  for (size_t i = 0; i < actual_size && i < expected_size; i++) {
    if (actual[i] != expected[i]) {
      pass = false;
      if constexpr (std::is_same_v<T, zx_handle_disposition_t>) {
        std::cout << std::dec << "element[" << i << "]: actual=" << actual[i]
                  << " expected=" << expected[i];
      } else {
        std::cout << std::dec << "element[" << i << "]: " << std::hex << "actual=0x" << +actual[i]
                  << " "
                  << "expected=0x" << +expected[i] << "\n";
      }
    }
  }
  if (actual_size != expected_size) {
    pass = false;
    std::cout << std::dec << "element[...]: "
              << "actual.size=" << +actual_size << " "
              << "expected.size=" << +expected_size << "\n";
  }
  return pass;
}

// Verifies that |value| encodes to |bytes|.
// Note: This is destructive to |value| - a new value must be created with each call.
template <typename FidlType>
bool EncodeSuccess(FidlType* value, const std::vector<uint8_t>& bytes,
                   const std::vector<zx_handle_disposition_t>& handle_dispositions,
                   bool check_handle_rights) {
  static_assert(fidl::IsFidlType<FidlType>::value, "FIDL type required");

  ::fidl::OwnedEncodedMessage<FidlType> encoded(value);
  if (!encoded.ok()) {
    std::cout << "Encoding failed: " << encoded.error() << std::endl;
    return false;
  }
  ::fidl::OutgoingMessage& outgoing = encoded.GetOutgoingMessage();
  for (uint32_t i = 0; i < outgoing.iovec_actual(); i++) {
    if (outgoing.iovecs()[i].buffer == nullptr) {
      std::cout << "Iovec " << i << " unexpectedly had a null buffer" << std::endl;
      return false;
    }
    if (outgoing.iovecs()[i].capacity == 0) {
      std::cout << "Iovec " << i << " had zero capacity" << std::endl;
      return false;
    }
    if (outgoing.iovecs()[i].reserved != 0) {
      std::cout << "Iovec " << i << " had a non-zero reserved field" << std::endl;
      return false;
    }
  }
  auto encoded_bytes = outgoing.CopyBytes();
  bool bytes_match =
      ComparePayload(encoded_bytes.data(), encoded_bytes.size(), bytes.data(), bytes.size());
  bool handles_match = false;
  if (check_handle_rights) {
    handles_match = ComparePayload(outgoing.handles(), outgoing.handle_actual(),
                                   handle_dispositions.data(), handle_dispositions.size());
  } else {
    std::vector<zx_handle_t> outgoing_msg_handles;
    std::vector<zx_handle_t> expected_handles;
    for (size_t i = 0; i < outgoing.handle_actual(); i++) {
      outgoing_msg_handles.push_back(outgoing.handles()[i].handle);
    }
    for (const auto& handle_disposition : handle_dispositions) {
      expected_handles.push_back(handle_disposition.handle);
    }
    handles_match = ComparePayload(outgoing_msg_handles.data(), outgoing_msg_handles.size(),
                                   expected_handles.data(), expected_handles.size());
  }
  return bytes_match && handles_match;
}

// Verifies that |value| fails to encode, with the expected error code.
// Note: This is destructive to |value| - a new value must be created with each call.
template <typename FidlType>
bool EncodeFailure(FidlType* value, zx_status_t expected_error_code) {
  static_assert(fidl::IsFidlType<FidlType>::value, "FIDL type required");

  ::fidl::OwnedEncodedMessage<FidlType> encoded(value);
  if (encoded.ok()) {
    std::cout << "Encoding unexpectedly succeeded" << std::endl;
    return false;
  }
  if (encoded.status() != expected_error_code) {
    std::cout << "Encoding failed with error code " << zx_status_get_string(encoded.status())
              << " (" << encoded.error() << "), but expected error code "
              << zx_status_get_string(expected_error_code) << std::endl;
    return false;
  }
  return true;
}

// Verifies that |bytes| decodes to an object that is the same as |value|.
// EqualityCheck is a callable with the signature |bool EqualityCheck(FidlType& actual)|
// that performs deep equality and compares handles based on koid, type and rights.
template <typename FidlType, typename EqualityCheck>
bool DecodeSuccess(FidlType* value, std::vector<uint8_t> bytes,
                   std::vector<zx_handle_info_t> handle_infos, EqualityCheck equality_check) {
  static_assert(fidl::IsFidlType<FidlType>::value, "FIDL type required");
  fidl::DecodedMessage<FidlType> decoded(bytes.data(), static_cast<uint32_t>(bytes.size()),
                                         handle_infos.data(),
                                         static_cast<uint32_t>(handle_infos.size()));
  if (!decoded.ok()) {
    std::cout << "Decoding failed: " << decoded.error() << std::endl;
    return false;
  }
  return equality_check(*decoded.PrimaryObject());
}

// Verifies that |bytes| fails to decode as |FidlType|, with the expected error
// code.
template <typename FidlType>
bool DecodeFailure(std::vector<uint8_t> bytes, std::vector<zx_handle_info_t> handle_infos,
                   zx_status_t expected_error_code) {
  static_assert(fidl::IsFidlType<FidlType>::value, "FIDL type required");
  fidl::DecodedMessage<FidlType> decoded(bytes.data(), static_cast<uint32_t>(bytes.size()),
                                         handle_infos.data(),
                                         static_cast<uint32_t>(handle_infos.size()));
  if (decoded.ok()) {
    std::cout << "Decoding unexpectedly succeeded" << std::endl;
    return false;
  }
  if (decoded.status() != expected_error_code) {
    std::cout << "Decoding failed with error code " << zx_status_get_string(decoded.status())
              << " (" << decoded.error() << "), but expected error code "
              << zx_status_get_string(expected_error_code) << std::endl;
    return false;
  }
  return true;
}

constexpr inline uint64_t FidlAlign(uint32_t offset) {
  constexpr uint64_t alignment_mask = FIDL_ALIGNMENT - 1;
  return (offset + alignment_mask) & ~alignment_mask;
}

}  // namespace llcpp_conformance_utils

#endif  // SRC_LIB_FIDL_LLCPP_TESTS_CONFORMANCE_CONFORMANCE_UTILS_H_
