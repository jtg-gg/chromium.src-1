// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/renderers/default_renderer_factory.h"

#include <utility>

#include "base/bind.h"
#include "base/single_thread_task_runner.h"
#include "build/build_config.h"
#include "media/base/decoder_factory.h"
#include "media/base/media_log.h"
#include "media/filters/gpu_video_decoder.h"
#include "media/filters/opus_audio_decoder.h"
#include "media/renderers/audio_renderer_impl.h"
#include "media/renderers/gpu_video_accelerator_factories.h"
#include "media/renderers/renderer_impl.h"
#include "media/renderers/video_renderer_impl.h"

#if !defined(MEDIA_DISABLE_FFMPEG)
#include "media/filters/ffmpeg_audio_decoder.h"
#if !defined(DISABLE_FFMPEG_VIDEO_DECODERS)
#include "media/filters/ffmpeg_video_decoder.h"
#endif
#endif

#if !defined(MEDIA_DISABLE_LIBVPX)
#include "media/filters/vpx_video_decoder.h"
#endif

namespace media {

DefaultRendererFactory::DefaultRendererFactory(
    const scoped_refptr<MediaLog>& media_log,
    DecoderFactory* decoder_factory,
    const GetGpuFactoriesCB& get_gpu_factories_cb,
    const AudioHardwareConfig& audio_hardware_config)
    : media_log_(media_log),
      decoder_factory_(decoder_factory),
      get_gpu_factories_cb_(get_gpu_factories_cb),
      audio_hardware_config_(audio_hardware_config) {}

DefaultRendererFactory::~DefaultRendererFactory() {
}

ScopedVector<AudioDecoder> DefaultRendererFactory::CreateAudioDecoders(
    const scoped_refptr<base::SingleThreadTaskRunner>& media_task_runner) {
  // Create our audio decoders and renderer.
  ScopedVector<AudioDecoder> audio_decoders;

#if !defined(MEDIA_DISABLE_FFMPEG)
  audio_decoders.push_back(
      new FFmpegAudioDecoder(media_task_runner, media_log_));
#endif

  audio_decoders.push_back(new OpusAudioDecoder(media_task_runner));

  if (decoder_factory_)
    decoder_factory_->CreateAudioDecoders(&audio_decoders);

  return audio_decoders;
}

ScopedVector<VideoDecoder> DefaultRendererFactory::CreateVideoDecoders(
    const scoped_refptr<base::SingleThreadTaskRunner>& media_task_runner,
    const RequestSurfaceCB& request_surface_cb,
    GpuVideoAcceleratorFactories* gpu_factories) {
  // Create our video decoders and renderer.
  ScopedVector<VideoDecoder> video_decoders;

  // |gpu_factories_| requires that its entry points be called on its
  // |GetTaskRunner()|.  Since |pipeline_| will own decoders created from the
  // factories, require that their message loops are identical.
  DCHECK(!gpu_factories ||
         (gpu_factories->GetTaskRunner() == media_task_runner.get()));

  if (gpu_factories)
    video_decoders.push_back(
        new GpuVideoDecoder(gpu_factories, request_surface_cb));

#if !defined(MEDIA_DISABLE_LIBVPX)
  video_decoders.push_back(new VpxVideoDecoder());
#endif

#if !defined(MEDIA_DISABLE_FFMPEG) && !defined(DISABLE_FFMPEG_VIDEO_DECODERS)
  video_decoders.push_back(new FFmpegVideoDecoder());
#endif

  if (decoder_factory_)
    decoder_factory_->CreateVideoDecoders(&video_decoders);

  return video_decoders;
}

scoped_ptr<Renderer> DefaultRendererFactory::CreateRenderer(
    const scoped_refptr<base::SingleThreadTaskRunner>& media_task_runner,
    const scoped_refptr<base::TaskRunner>& worker_task_runner,
    AudioRendererSink* audio_renderer_sink,
    VideoRendererSink* video_renderer_sink,
    const RequestSurfaceCB& request_surface_cb) {
  DCHECK(audio_renderer_sink);

  scoped_ptr<AudioRenderer> audio_renderer(
      new AudioRendererImpl(media_task_runner, audio_renderer_sink,
                            CreateAudioDecoders(media_task_runner),
                            audio_hardware_config_, media_log_));

  GpuVideoAcceleratorFactories* gpu_factories = nullptr;
  if (!get_gpu_factories_cb_.is_null())
    gpu_factories = get_gpu_factories_cb_.Run();

  scoped_ptr<VideoRenderer> video_renderer(new VideoRendererImpl(
      media_task_runner, worker_task_runner, video_renderer_sink,
      CreateVideoDecoders(media_task_runner, request_surface_cb, gpu_factories),
      true, gpu_factories, media_log_));

  return scoped_ptr<Renderer>(new RendererImpl(
      media_task_runner, std::move(audio_renderer), std::move(video_renderer)));
}

}  // namespace media
