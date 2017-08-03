// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/mozart/examples/shadertoy/service/imagepipe_shadertoy.h"

#include "apps/mozart/examples/shadertoy/service/escher_utils.h"
#include "apps/mozart/examples/shadertoy/service/renderer.h"
#include "escher/renderer/framebuffer.h"
#include "escher/renderer/image.h"
#include "escher/renderer/simple_image_factory.h"

namespace {
// TODO: Copied this constant from mozart/src/scene_manager/fence.h.
// Put it in a shared header file somewhere.
constexpr mx_status_t kFenceSignalled = MX_EVENT_SIGNALED;
}  // namespace

namespace shadertoy {

ShadertoyStateForImagePipe::ShadertoyStateForImagePipe(
    App* app,
    ::fidl::InterfaceHandle<mozart2::ImagePipe> image_pipe)
    : ShadertoyState(app),
      image_pipe_(mozart2::ImagePipePtr::Create(std::move(image_pipe))) {
  image_pipe_.set_connection_error_handler([this] { this->Close(); });
}

ShadertoyStateForImagePipe::~ShadertoyStateForImagePipe() = default;

void ShadertoyStateForImagePipe::ClearFramebuffers() {
  for (size_t i = 0; i < kNumFramebuffers; ++i) {
    auto& fb = framebuffers_[i];
    fb.framebuffer = nullptr;
    fb.acquire_semaphore = nullptr;
    fb.release_semaphore = nullptr;
    fb.acquire_fence.reset();
    fb.release_fence.reset();
    if (fb.image_pipe_id) {
      // TODO(MZ-242): The docs in image_pipe.fidl says that all release fences
      // must "be signaled before freeing or modifying the underlying memory
      // object".  However, it seems convenient to allow clients to free the
      // object immediately; this shouldn't be a problem because the
      // presentation queue also has a reference to the memory.
      image_pipe_->RemoveImage(fb.image_pipe_id);
      fb.image_pipe_id = 0;
    }
  }
}

void ShadertoyStateForImagePipe::OnSetResolution() {
  ClearFramebuffers();

  escher::ImageInfo escher_image_info;
  escher_image_info.format = renderer()->framebuffer_format();
  escher_image_info.width = width();
  escher_image_info.height = height();
  escher_image_info.sample_count = 1;
  escher_image_info.usage = vk::ImageUsageFlagBits::eColorAttachment;

  escher::SimpleImageFactory factory(escher()->resource_recycler(),
                                     escher()->gpu_allocator());
  for (size_t i = 0; i < kNumFramebuffers; ++i) {
    auto& fb = framebuffers_[i];

    auto acquire_semaphore_pair = NewSemaphoreEventPair(escher());
    auto release_semaphore_pair = NewSemaphoreEventPair(escher());
    if (!acquire_semaphore_pair.first || !release_semaphore_pair.first) {
      FTL_LOG(ERROR) << "OnSetResolution() failed.";
      ClearFramebuffers();
      Close();
      return;
    }

    // The release fences should be immediately ready to render, since they are
    // passed to DrawFrame() as the 'framebuffer_ready' semaphore.
    release_semaphore_pair.second.signal(0u, kFenceSignalled);

    auto image = factory.NewImage(escher_image_info);
    mx::vmo vmo = ExportMemoryAsVMO(escher(), image->memory());
    if (!vmo) {
      FTL_LOG(ERROR) << "OnSetResolution() failed.";
      ClearFramebuffers();
      Close();
      return;
    }

    fb.framebuffer = ftl::MakeRefCounted<escher::Framebuffer>(
        escher(), width(), height(), std::vector<escher::ImagePtr>{image},
        renderer()->render_pass());
    fb.acquire_semaphore = std::move(acquire_semaphore_pair.first);
    fb.release_semaphore = std::move(release_semaphore_pair.first);
    fb.acquire_fence = std::move(acquire_semaphore_pair.second);
    fb.release_fence = std::move(release_semaphore_pair.second);
    fb.image_pipe_id = next_image_pipe_id_++;

    auto image_info = mozart2::ImageInfo::New();
    image_info->width = width();
    image_info->height = height();
    image_info->stride = 0;  // inapplicable to GPU_OPTIMAL tiling.
    image_info->tiling = mozart2::ImageInfo::Tiling::GPU_OPTIMAL;

    image_pipe_->AddImage(fb.image_pipe_id, std::move(image_info),
                          std::move(vmo), mozart2::MemoryType::VK_DEVICE_MEMORY,
                          image->memory_offset());
  }
}

static mx::event DuplicateEvent(const mx::event& evt) {
  mx::event dup;
  auto result = evt.duplicate(MX_RIGHT_SAME_RIGHTS, &dup);
  if (result != MX_OK) {
    FTL_LOG(ERROR) << "Failed to duplicate event (status: " << result << ").";
  }
  return dup;
}

void ShadertoyStateForImagePipe::DrawFrame(uint64_t presentation_time,
                                           float animation_time) {
  // Prepare arguments.
  auto& fb = framebuffers_[next_framebuffer_index_];
  next_framebuffer_index_ = (next_framebuffer_index_ + 1) % kNumFramebuffers;
  mx::event acquire_fence(DuplicateEvent(fb.acquire_fence));
  mx::event release_fence(DuplicateEvent(fb.release_fence));
  if (!acquire_fence || !release_fence) {
    Close();
    return;
  }

  // Render.
  Renderer::Params params;
  params.iResolution = glm::vec3(width(), height(), 1);
  params.iTime = animation_time;
  // TODO(MZ-241):  params.iTimeDelta = ??;
  // TODO(MZ-241): params.iFrame = 0;
  // TODO(MZ-241): params.iChannelTime = ??;
  // TODO(MZ-241): params.iChannelResolution = ??;
  params.iMouse = i_mouse();
  // TODO(MZ-241): params.iDate = ??;
  // TODO(MZ-241): params.iSampleRate = ??;

  renderer()->DrawFrame(
      fb.framebuffer, pipeline(), params, channel0(), channel1(), channel2(),
      channel3(),
      // TODO(MZ-240): waiting on the release_semaphore causes the app to crash
      // during Vulkan command-buffer submit.
      // fb.release_semaphore,
      escher::SemaphorePtr(),
      // TODO(MZ-240): signaling the acquire_semaphore causes the app to crash
      // during Vulkan command-buffer submit.
      // fb.acquire_semaphore,
      escher::SemaphorePtr());
  // TODO(MZ-240): this shouldn't even be necessary once we have Vulkan
  // semaphore import working properly.
  fb.acquire_fence.signal(0u, kFenceSignalled);
  mx::event bogus_event;
  mx::event::create(0u, &bogus_event);

  // Present the image and request another frame.
  auto present_image_callback = [weak = weak_ptr_factory()->GetWeakPtr()](
      mozart2::PresentationInfoPtr info) {
    // Need this cast in order to call protected member of superclass.
    if (auto self = static_cast<ShadertoyStateForImagePipe*>(weak.get())) {
      self->OnFramePresented(info);
    }
  };
  image_pipe_->PresentImage(fb.image_pipe_id, presentation_time,
                            std::move(acquire_fence),
                            // TODO(MZ-240): use release_fence once we have
                            // Vulkan semaphore import working properly.
                            // std::move(release_fence),
                            std::move(bogus_event), present_image_callback);
}

}  // namespace shadertoy
