// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.sysmem2/cpp/wire.h>
#include <lib/ddk/platform-defs.h>

#include <zxtest/zxtest.h>

#include "usage_pixel_format_cost.h"

namespace sysmem_driver {
namespace {

TEST(PixelFormatCost, Afbc) {
  fidl::Arena allocator;

  fuchsia_sysmem2::wire::BufferCollectionConstraints constraints(allocator);
  constraints.set_image_format_constraints(allocator, allocator, 2);
  {
    fuchsia_sysmem2::wire::ImageFormatConstraints image_format_constraints(allocator);
    {
      fuchsia_sysmem2::wire::PixelFormat pixel_format(allocator);
      pixel_format.set_type(allocator, fuchsia_sysmem2::wire::PixelFormatType::kBgra32);
      image_format_constraints.set_pixel_format(allocator, std::move(pixel_format));
    }
    constraints.image_format_constraints()[0] = std::move(image_format_constraints);
  }
  {
    fuchsia_sysmem2::wire::ImageFormatConstraints image_format_constraints(allocator);
    {
      fuchsia_sysmem2::wire::PixelFormat pixel_format(allocator);
      pixel_format.set_type(allocator, fuchsia_sysmem2::wire::PixelFormatType::kBgra32);
      pixel_format.set_format_modifier_value(allocator,
                                             fuchsia_sysmem2::wire::kFormatModifierArmAfbc32X8);
      image_format_constraints.set_pixel_format(allocator, std::move(pixel_format));
    }
    constraints.image_format_constraints()[1] = std::move(image_format_constraints);
  }

  uint32_t amlogic_pids[] = {
      PDEV_PID_AMLOGIC_S912,
      PDEV_PID_AMLOGIC_S905D2,
      PDEV_PID_AMLOGIC_T931,
      PDEV_PID_AMLOGIC_A311D,
  };
  for (uint32_t pid : amlogic_pids) {
    EXPECT_LT(0, UsagePixelFormatCost::Compare(PDEV_VID_AMLOGIC, pid, constraints, 0, 1));
    EXPECT_GT(0, UsagePixelFormatCost::Compare(PDEV_VID_AMLOGIC, pid, constraints, 1, 0));
  }
  EXPECT_EQ(0, UsagePixelFormatCost::Compare(0u, PDEV_PID_AMLOGIC_S912, constraints, 0, 1));
  EXPECT_EQ(0, UsagePixelFormatCost::Compare(0u, PDEV_PID_AMLOGIC_S912, constraints, 1, 0));
}

TEST(PixelFormatCost, IntelTiling) {
  fidl::Arena allocator;

  constexpr uint32_t kUnknownPid = 0;
  constexpr uint32_t kUnknownVid = 0;

  fuchsia_sysmem2::wire::BufferCollectionConstraints constraints(allocator);

  constraints.set_image_format_constraints(allocator, allocator, 2);
  uint64_t tiling_types[] = {fuchsia_sysmem2::wire::kFormatModifierIntelI915XTiled,
                             fuchsia_sysmem2::wire::kFormatModifierIntelI915YfTiled,
                             fuchsia_sysmem2::wire::kFormatModifierIntelI915YTiled};
  for (auto modifier : tiling_types) {
    {
      fuchsia_sysmem2::wire::ImageFormatConstraints image_format_constraints(allocator);
      {
        fuchsia_sysmem2::wire::PixelFormat pixel_format(allocator);
        pixel_format.set_type(allocator, fuchsia_sysmem2::wire::PixelFormatType::kBgra32);
        pixel_format.set_format_modifier_value(allocator,
                                               fuchsia_sysmem2::wire::kFormatModifierLinear);
        image_format_constraints.set_pixel_format(allocator, std::move(pixel_format));
      }
      constraints.image_format_constraints()[0] = std::move(image_format_constraints);
    }
    {
      fuchsia_sysmem2::wire::ImageFormatConstraints image_format_constraints(allocator);
      {
        fuchsia_sysmem2::wire::PixelFormat pixel_format(allocator);
        pixel_format.set_type(allocator, fuchsia_sysmem2::wire::PixelFormatType::kBgra32);
        pixel_format.set_format_modifier_value(allocator, modifier);
        image_format_constraints.set_pixel_format(allocator, std::move(pixel_format));
      }
      constraints.image_format_constraints()[1] = std::move(image_format_constraints);
    }
    EXPECT_LT(0, UsagePixelFormatCost::Compare(kUnknownVid, kUnknownPid, constraints, 0, 1));
    EXPECT_GT(0, UsagePixelFormatCost::Compare(kUnknownVid, kUnknownPid, constraints, 1, 0));
    // Intel tiled formats aren't necessarily useful on AMLOGIC, but if some hardware supported them
    // they should probably be used anyway.
    EXPECT_LT(0, UsagePixelFormatCost::Compare(PDEV_VID_AMLOGIC, PDEV_PID_AMLOGIC_S912, constraints,
                                               0, 1));
    EXPECT_GT(0, UsagePixelFormatCost::Compare(PDEV_VID_AMLOGIC, PDEV_PID_AMLOGIC_S912, constraints,
                                               1, 0));

    // Explicit linear should be treated the same as no format modifier value.
    constraints.image_format_constraints()[0].pixel_format().format_modifier_value() =
        fuchsia_sysmem2::wire::kFormatModifierNone;

    EXPECT_LT(0, UsagePixelFormatCost::Compare(kUnknownVid, kUnknownPid, constraints, 0, 1));
    EXPECT_GT(0, UsagePixelFormatCost::Compare(kUnknownVid, kUnknownPid, constraints, 1, 0));

    // Explicit linear should be treated the same as no format modifier value.
    {
      fuchsia_sysmem2::wire::PixelFormat pixel_format(allocator);
      pixel_format.set_type(allocator, fuchsia_sysmem2::wire::PixelFormatType::kBgra32);
      constraints.image_format_constraints()[0].set_pixel_format(allocator,
                                                                 std::move(pixel_format));
    }
    EXPECT_LT(0, UsagePixelFormatCost::Compare(kUnknownVid, kUnknownPid, constraints, 0, 1));
    EXPECT_GT(0, UsagePixelFormatCost::Compare(kUnknownVid, kUnknownPid, constraints, 1, 0));
  }

  // Formats are in ascending preference order (descending cost order).
  std::array modifier_list = {
      fuchsia_sysmem2::wire::kFormatModifierLinear,
      fuchsia_sysmem2::wire::kFormatModifierIntelI915XTiled,
      fuchsia_sysmem2::wire::kFormatModifierIntelI915YTiled,
      fuchsia_sysmem2::wire::kFormatModifierIntelI915YfTiled,
      fuchsia_sysmem2::wire::kFormatModifierIntelI915YTiledCcs,
      fuchsia_sysmem2::wire::kFormatModifierIntelI915YfTiledCcs,
  };
  constraints.set_image_format_constraints(allocator, allocator, modifier_list.size());

  for (uint32_t i = 0; i < modifier_list.size(); ++i) {
    {
      fuchsia_sysmem2::wire::ImageFormatConstraints image_format_constraints(allocator);
      {
        fuchsia_sysmem2::wire::PixelFormat pixel_format(allocator);
        pixel_format.set_type(allocator, fuchsia_sysmem2::wire::PixelFormatType::kBgra32);
        pixel_format.set_format_modifier_value(allocator, modifier_list[i]);
        image_format_constraints.set_pixel_format(allocator, std::move(pixel_format));
      }
      constraints.image_format_constraints()[i] = std::move(image_format_constraints);
    }
  }

  for (uint32_t i = 1; i < modifier_list.size(); ++i) {
    EXPECT_LT(0, UsagePixelFormatCost::Compare(kUnknownVid, kUnknownPid, constraints, i - 1, i),
              "i=%d", i);
    EXPECT_GT(0, UsagePixelFormatCost::Compare(kUnknownVid, kUnknownPid, constraints, i, i - 1),
              "i=%d", i);
  }
}

TEST(PixelFormatCost, ArmTransactionElimination) {
  fidl::Arena allocator;

  fuchsia_sysmem2::wire::BufferCollectionConstraints constraints(allocator);
  constraints.set_image_format_constraints(allocator, allocator, 2);
  {
    fuchsia_sysmem2::wire::ImageFormatConstraints image_format_constraints(allocator);
    {
      fuchsia_sysmem2::wire::PixelFormat pixel_format(allocator);
      pixel_format.set_type(allocator, fuchsia_sysmem2::wire::PixelFormatType::kBgra32);
      pixel_format.set_format_modifier_value(allocator,
                                             fuchsia_sysmem2::wire::kFormatModifierArmAfbc32X8);
      image_format_constraints.set_pixel_format(allocator, std::move(pixel_format));
    }
    constraints.image_format_constraints()[0] = std::move(image_format_constraints);
  }
  {
    fuchsia_sysmem2::wire::ImageFormatConstraints image_format_constraints(allocator);
    {
      fuchsia_sysmem2::wire::PixelFormat pixel_format(allocator);
      pixel_format.set_type(allocator, fuchsia_sysmem2::wire::PixelFormatType::kBgra32);
      pixel_format.set_format_modifier_value(allocator,
                                             fuchsia_sysmem2::wire::kFormatModifierArmAfbc32X8Te);
      image_format_constraints.set_pixel_format(allocator, std::move(pixel_format));
    }
    constraints.image_format_constraints()[1] = std::move(image_format_constraints);
  }

  EXPECT_LT(
      0, UsagePixelFormatCost::Compare(PDEV_VID_AMLOGIC, PDEV_PID_AMLOGIC_S912, constraints, 0, 1));
  EXPECT_GT(
      0, UsagePixelFormatCost::Compare(PDEV_VID_AMLOGIC, PDEV_PID_AMLOGIC_S912, constraints, 1, 0));
  EXPECT_EQ(0, UsagePixelFormatCost::Compare(0u, PDEV_PID_AMLOGIC_S912, constraints, 0, 1));
  EXPECT_EQ(0, UsagePixelFormatCost::Compare(0u, PDEV_PID_AMLOGIC_S912, constraints, 1, 0));
}

TEST(PixelFormatCost, AfbcWithFlags) {
  fidl::Arena allocator;

  // Formats are in ascending preference order (descending cost order).
  std::array modifier_list = {
      fuchsia_sysmem2::wire::kFormatModifierLinear,
      fuchsia_sysmem2::wire::kFormatModifierArmAfbc16X16,
      fuchsia_sysmem2::wire::kFormatModifierArmAfbc16X16SplitBlockSparseYuv,
      fuchsia_sysmem2::wire::kFormatModifierArmAfbc16X16SplitBlockSparseYuvTiledHeader,
      fuchsia_sysmem2::wire::kFormatModifierArmAfbc16X16Te,
      fuchsia_sysmem2::wire::kFormatModifierArmAfbc16X16SplitBlockSparseYuvTe,
      fuchsia_sysmem2::wire::kFormatModifierArmAfbc16X16SplitBlockSparseYuvTeTiledHeader,
  };
  fuchsia_sysmem2::wire::BufferCollectionConstraints constraints(allocator);
  constraints.set_image_format_constraints(allocator, allocator, modifier_list.size());

  for (uint32_t i = 0; i < modifier_list.size(); ++i) {
    {
      fuchsia_sysmem2::wire::ImageFormatConstraints image_format_constraints(allocator);
      {
        fuchsia_sysmem2::wire::PixelFormat pixel_format(allocator);
        pixel_format.set_type(allocator, fuchsia_sysmem2::wire::PixelFormatType::kBgra32);
        pixel_format.set_format_modifier_value(allocator, modifier_list[i]);
        image_format_constraints.set_pixel_format(allocator, std::move(pixel_format));
      }
      constraints.image_format_constraints()[i] = std::move(image_format_constraints);
    }
  }

  for (uint32_t i = 1; i < modifier_list.size(); ++i) {
    EXPECT_LT(0,
              UsagePixelFormatCost::Compare(PDEV_VID_AMLOGIC, PDEV_PID_AMLOGIC_S912, constraints,
                                            i - 1, i),
              "i=%d", i);
    EXPECT_GT(0,
              UsagePixelFormatCost::Compare(PDEV_VID_AMLOGIC, PDEV_PID_AMLOGIC_S912, constraints, i,
                                            i - 1),
              "i=%d", i);
    EXPECT_EQ(0, UsagePixelFormatCost::Compare(0u, PDEV_PID_AMLOGIC_S912, constraints, i - 1, i));
    EXPECT_EQ(0, UsagePixelFormatCost::Compare(0u, PDEV_PID_AMLOGIC_S912, constraints, i, i - 1));
  }
}

}  // namespace
}  // namespace sysmem_driver
