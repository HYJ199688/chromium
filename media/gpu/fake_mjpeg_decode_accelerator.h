// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_GPU_FAKE_MJPEG_DECODE_ACCELERATOR_H_
#define MEDIA_GPU_FAKE_MJPEG_DECODE_ACCELERATOR_H_

#include <stdint.h>

#include <memory>

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/thread.h"
#include "media/base/bitstream_buffer.h"
#include "media/gpu/media_gpu_export.h"
#include "media/video/mjpeg_decode_accelerator.h"

namespace base {
class SingleThreadTaskRunner;
}

namespace media {

// Uses software-based decoding. The purpose of this class is to enable testing
// of communication to the MjpegDecodeAccelerator without requiring an actual
// hardware decoder.
class MEDIA_GPU_EXPORT FakeMjpegDecodeAccelerator
    : public MjpegDecodeAccelerator {
 public:
  FakeMjpegDecodeAccelerator(
      const scoped_refptr<base::SingleThreadTaskRunner>& io_task_runner);
  ~FakeMjpegDecodeAccelerator() override;

  // MjpegDecodeAccelerator implementation.
  bool Initialize(MjpegDecodeAccelerator::Client* client) override;
  void Decode(const BitstreamBuffer& bitstream_buffer,
              const scoped_refptr<VideoFrame>& video_frame) override;
  bool IsSupported() override;

 private:
  void DecodeOnDecoderThread(const BitstreamBuffer& bitstream_buffer,
                             const scoped_refptr<VideoFrame>& video_frame,
                             std::unique_ptr<WritableUnalignedMapping> src_shm);
  void NotifyError(int32_t bitstream_buffer_id, Error error);
  void NotifyErrorOnClientThread(int32_t bitstream_buffer_id, Error error);
  void OnDecodeDoneOnClientThread(int32_t input_buffer_id);

  // Task runner for calls to |client_|.
  const scoped_refptr<base::SingleThreadTaskRunner> client_task_runner_;

  // GPU IO task runner.
  const scoped_refptr<base::SingleThreadTaskRunner> io_task_runner_;

  Client* client_ = nullptr;

  base::Thread decoder_thread_;
  scoped_refptr<base::SingleThreadTaskRunner> decoder_task_runner_;

  base::WeakPtrFactory<FakeMjpegDecodeAccelerator> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(FakeMjpegDecodeAccelerator);
};

}  // namespace media

#endif  // MEDIA_GPU_FAKE_MJPEG_DECODE_ACCELERATOR_H_
