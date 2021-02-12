// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:fidl_fidl_test_bitsmemberremove/fidl_async.dart' as fidllib;

// [START contents]
void useBits(fidllib.Flags bits) {
  if ((bits & fidllib.Flags.optionA).$value != 0) {
    print('option A is set');
  }
  if ((bits & fidllib.Flags.optionB).$value != 0) {
    print('option B is set');
  }
  if (bits.hasUnknownBits()) {
    print('unknown options: ${bits.getUnknownBits()}');
  }
}
// [END contents]
