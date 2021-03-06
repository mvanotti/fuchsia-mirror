// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_LIB_ESCHER_RENDERER_BATCH_GPU_UPLOADER_H_
#define SRC_UI_LIB_ESCHER_RENDERER_BATCH_GPU_UPLOADER_H_

#include <lib/fit/function.h>

#include "src/ui/lib/escher/escher.h"
#include "src/ui/lib/escher/renderer/buffer_cache.h"
#include "src/ui/lib/escher/renderer/frame.h"
#include "src/ui/lib/escher/vk/buffer.h"
#include "src/ui/lib/escher/vk/command_buffer.h"

#include <vulkan/vulkan.hpp>

namespace escher {

// Provides host-accessible GPU memory for clients to upload Images and Buffers
// to the GPU. Offers the ability to batch uploads into consolidated submissions
// to the GPU driver.
//
// TODO(SCN-1197) Add memory barriers so the BatchGpuUploader and
// BatchGpuDownloader can handle synchronization of reads and writes on the same
// Resource.
//
// Currently users of BatchGpuUploader should manually enforce that the
// BatchGpuUploader waits on other BatchGpuUploader or gfx::Engine if they write
// to the images / buffers the BatchGpuUploader reads from, by using
// AddWaitSemaphore() function. Also, Submit() function will return a semaphore
// being signaled when command buffer finishes execution, which can be used for
// synchronization.
class BatchGpuUploader {
 public:
  static std::unique_ptr<BatchGpuUploader> New(EscherWeakPtr weak_escher,
                                               uint64_t frame_trace_number = 0);

  BatchGpuUploader(EscherWeakPtr weak_escher, uint64_t frame_trace_number = 0);
  ~BatchGpuUploader();

  // Returns true if the BatchGPUUploader has acquired a reader or writer and
  // has work to do on the GPU.
  bool HasContentToUpload() const { return is_initialized_; }

  // Provides a pointer in host-accessible GPU memory, and methods to copy this
  // memory into optimally-formatted Images and Buffers.
  class Writer {
   public:
    Writer(CommandBufferPtr command_buffer, BufferPtr buffer);
    ~Writer();

    // Schedule a buffer-to-buffer copy that will be submitted when Submit()
    // is called.  Retains a reference to the target until the submission's
    // CommandBuffer is retired. Places a wait semaphore on the target,
    // which is signaled when the batched commands are done.
    void WriteBuffer(const BufferPtr& target, vk::BufferCopy region);

    // Schedule a buffer-to-image copy that will be submitted when Submit()
    // is called.  Retains a reference to the target until the submission's
    // CommandBuffer is retired. Places a wait semaphore on the target,
    // which is signaled when the batched commands are done.
    void WriteImage(const ImagePtr& target, vk::BufferImageCopy region,
                    vk::ImageLayout final_layout = vk::ImageLayout::eShaderReadOnlyOptimal);

    uint8_t* host_ptr() const { return buffer_->host_ptr(); }
    vk::DeviceSize size() const { return buffer_->size(); }

   private:
    friend class BatchGpuUploader;
    // Gets the CommandBuffer to batch commands with all other posted writers.
    // This writer cannot be used after the command buffer has been retrieved.
    CommandBufferPtr TakeCommandsAndShutdown();

    CommandBufferPtr command_buffer_;
    BufferPtr buffer_;

    FXL_DISALLOW_COPY_AND_ASSIGN(Writer);
  };  // class BatchGpuUploader::Writer

  // Obtain a Writer that has the specified amount of write space.
  //
  // TODO(SCN-846) Only one writer can be acquired at a time. When we move to
  // backing writers with secondary CommandBuffers, multiple writes can be
  // acquired at once and their writes can be parallelized across threads.
  std::unique_ptr<Writer> AcquireWriter(vk::DeviceSize size);

  // Post a Writer to the batch uploader. The Writer's work will be posted to
  // the GPU on Submit();
  void PostWriter(std::unique_ptr<Writer> writer);

  // Submits all Writers' and Reader's work to the GPU. No Writers or Readers
  // can be posted once Submit is called. Callback function will be called after
  // all work is done.
  void Submit(fit::function<void()> callback = nullptr);

  // Submit() will wait on all semaphores added by AddWaitSemaphore().
  void AddWaitSemaphore(SemaphorePtr sema, vk::PipelineStageFlags flags);

  // Submit() will signal all semaphores added by AddSignalSemaphore().
  void AddSignalSemaphore(SemaphorePtr sema);

 private:
  void Initialize();

  int32_t writer_count_ = 0;

  EscherWeakPtr escher_;
  bool is_initialized_ = false;
  // The trace number for the frame. Cached to support lazy frame creation.
  const uint64_t frame_trace_number_;
  // Lazily created when the first Reader or Writer is acquired.
  BufferCacheWeakPtr buffer_cache_;
  FramePtr frame_;

  std::vector<std::pair<SemaphorePtr, vk::PipelineStageFlags>> wait_semaphores_;
  std::vector<SemaphorePtr> signal_semaphores_;

  FXL_DISALLOW_COPY_AND_ASSIGN(BatchGpuUploader);
};

}  // namespace escher

#endif  // SRC_UI_LIB_ESCHER_RENDERER_BATCH_GPU_UPLOADER_H_
