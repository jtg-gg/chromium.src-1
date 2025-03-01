// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_TEST_PIPELINE_INTEGRATION_TEST_BASE_H_
#define MEDIA_TEST_PIPELINE_INTEGRATION_TEST_BASE_H_

#include <stdint.h>

#include "base/md5.h"
#include "base/message_loop/message_loop.h"
#include "media/audio/clockless_audio_sink.h"
#include "media/audio/null_audio_sink.h"
#include "media/base/audio_hardware_config.h"
#include "media/base/demuxer.h"
#include "media/base/media_keys.h"
#include "media/base/null_video_sink.h"
#include "media/base/pipeline_impl.h"
#include "media/base/pipeline_status.h"
#include "media/base/text_track.h"
#include "media/base/text_track_config.h"
#include "media/base/video_frame.h"
#include "media/renderers/video_renderer_impl.h"
#include "testing/gmock/include/gmock/gmock.h"

namespace base {
class FilePath;
}

namespace media {

class CdmContext;

// Empty MD5 hash string.  Used to verify empty video tracks.
extern const char kNullVideoHash[];

// Empty hash string.  Used to verify empty audio tracks.
extern const char kNullAudioHash[];

// Dummy tick clock which advances extremely quickly (1 minute every time
// NowTicks() is called).
class DummyTickClock : public base::TickClock {
 public:
  DummyTickClock() : now_() {}
  ~DummyTickClock() override {}
  base::TimeTicks NowTicks() override;

 private:
  base::TimeTicks now_;
};

// Integration tests for Pipeline. Real demuxers, real decoders, and
// base renderer implementations are used to verify pipeline functionality. The
// renderers used in these tests rely heavily on the AudioRendererBase &
// VideoRendererImpl implementations which contain a majority of the code used
// in the real AudioRendererImpl & SkCanvasVideoRenderer implementations used in
// the browser. The renderers in this test don't actually write data to a
// display or audio device. Both of these devices are simulated since they have
// little effect on verifying pipeline behavior and allow tests to run faster
// than real-time.
class PipelineIntegrationTestBase {
 public:
  PipelineIntegrationTestBase();
  virtual ~PipelineIntegrationTestBase();

  bool WaitUntilOnEnded();
  PipelineStatus WaitUntilEndedOrError();

  // Starts the pipeline (optionally with a CdmContext), returning the final
  // status code after it has started. |filename| points at a test file located
  // under media/test/data/.
  PipelineStatus Start(const std::string& filename);
  PipelineStatus Start(const std::string& filename, CdmContext* cdm_context);

  // Starts the pipeline in a particular mode for advanced testing and
  // benchmarking purposes (e.g., underflow is disabled to ensure consistent
  // hashes).  May be combined using the bitwise or operator (and as such must
  // have values that are powers of two).
  enum TestTypeFlags { kNormal = 0, kHashed = 1, kClockless = 2 };
  PipelineStatus Start(const std::string& filename, uint8_t test_type);

  void Play();
  void Pause();
  bool Seek(base::TimeDelta seek_time);
  bool Suspend();
  bool Resume(base::TimeDelta seek_time);
  void Stop();
  bool WaitUntilCurrentTimeIsAfter(const base::TimeDelta& wait_time);

  // Returns the MD5 hash of all video frames seen.  Should only be called once
  // after playback completes.  First time hashes should be generated with
  // --video-threads=1 to ensure correctness.  Pipeline must have been started
  // with hashing enabled.
  std::string GetVideoHash();

  // Returns the hash of all audio frames seen.  Should only be called once
  // after playback completes.  Pipeline must have been started with hashing
  // enabled.
  std::string GetAudioHash();

  // Returns the time taken to render the complete audio file.
  // Pipeline must have been started with clockless playback enabled.
  base::TimeDelta GetAudioTime();

 protected:
  base::MessageLoop message_loop_;
  base::MD5Context md5_context_;
  bool hashing_enabled_;
  bool clockless_playback_;
  scoped_ptr<Demuxer> demuxer_;
  scoped_ptr<DataSource> data_source_;
  scoped_ptr<PipelineImpl> pipeline_;
  scoped_refptr<NullAudioSink> audio_sink_;
  scoped_refptr<ClocklessAudioSink> clockless_audio_sink_;
  scoped_ptr<NullVideoSink> video_sink_;
  bool ended_;
  PipelineStatus pipeline_status_;
  Demuxer::EncryptedMediaInitDataCB encrypted_media_init_data_cb_;
  VideoPixelFormat last_video_frame_format_;
  ColorSpace last_video_frame_color_space_;
  DummyTickClock dummy_clock_;
  AudioHardwareConfig hardware_config_;
  PipelineMetadata metadata_;

  void OnSeeked(base::TimeDelta seek_time, PipelineStatus status);
  void OnStatusCallback(PipelineStatus status);
  void DemuxerEncryptedMediaInitDataCB(EmeInitDataType type,
                                       const std::vector<uint8_t>& init_data);
  void set_encrypted_media_init_data_cb(
      const Demuxer::EncryptedMediaInitDataCB& encrypted_media_init_data_cb) {
    encrypted_media_init_data_cb_ = encrypted_media_init_data_cb;
  }

  void OnEnded();
  void OnError(PipelineStatus status);
  void QuitAfterCurrentTimeTask(const base::TimeDelta& quit_time);

  // Creates Demuxer and sets |demuxer_|.
  void CreateDemuxer(const std::string& filename);

  // Creates and returns a Renderer.
  virtual scoped_ptr<Renderer> CreateRenderer();

  void OnVideoFramePaint(const scoped_refptr<VideoFrame>& frame);

  MOCK_METHOD1(OnMetadata, void(PipelineMetadata));
  MOCK_METHOD1(OnBufferingStateChanged, void(BufferingState));
  MOCK_METHOD1(DecryptorAttached, void(bool));
  MOCK_METHOD2(OnAddTextTrack,
               void(const TextTrackConfig& config,
                    const AddTextTrackDoneCB& done_cb));
  MOCK_METHOD0(OnWaitingForDecryptionKey, void(void));
};

}  // namespace media

#endif  // MEDIA_TEST_PIPELINE_INTEGRATION_TEST_BASE_H_
