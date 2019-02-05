// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <unistd.h>
#include <zircon/compiler.h>
#include <ddk/protocol/platform/device.h>
#include <ddk/protocol/platform-device-lib.h>
#include <lib/mmio/mmio.h>
#include <lib/mipi-dsi/mipi-dsi.h>
#include "common.h"
#include "dw-mipi-dsi-reg.h"
#include <optional>

namespace astro_display {

using mipi_dsi::MipiDsiCmd;

class DwMipiDsi {
public:
    DwMipiDsi() {}
    zx_status_t Init(zx_device_t* parent);
    zx_status_t SendCmd(const MipiDsiCmd& cmd);

private:
    inline bool IsPldREmpty();
    inline bool IsPldRFull();
    inline bool IsPldWEmpty();
    inline bool IsPldWFull();
    inline bool IsCmdEmpty();
    inline bool IsCmdFull();
    zx_status_t WaitforFifo(uint32_t reg, uint32_t bit, uint32_t val);
    zx_status_t WaitforPldWNotFull();
    zx_status_t WaitforPldWEmpty();
    zx_status_t WaitforPldRFull();
    zx_status_t WaitforPldRNotEmpty();
    zx_status_t WaitforCmdNotFull();
    zx_status_t WaitforCmdEmpty();
    void DumpCmd(const MipiDsiCmd& cmd);
    zx_status_t GenericPayloadRead(uint32_t* data);
    zx_status_t GenericHdrWrite(uint32_t data);
    zx_status_t GenericPayloadWrite(uint32_t data);
    void EnableBta();
    void DisableBta();
    zx_status_t WaitforBtaAck();
    zx_status_t GenWriteShort(const MipiDsiCmd& cmd);
    zx_status_t DcsWriteShort(const MipiDsiCmd& cmd);
    zx_status_t GenWriteLong(const MipiDsiCmd& cmd);
    zx_status_t GenRead(const MipiDsiCmd& cmd);

    std::optional<ddk::MmioBuffer> mipi_dsi_mmio_;
    pdev_protocol_t pdev_ = {nullptr, nullptr};

    bool initialized_ = false;
};

} // namespace astro_display
