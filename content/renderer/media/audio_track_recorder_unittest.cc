// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/media/audio_track_recorder.h"

#include <stdint.h>

#include "base/macros.h"
#include "base/run_loop.h"
#include "base/stl_util.h"
#include "base/strings/utf_string_conversions.h"
#include "content/renderer/media/media_stream_audio_source.h"
#include "content/renderer/media/mock_media_constraint_factory.h"
#include "content/renderer/media/webrtc/webrtc_local_audio_track_adapter.h"
#include "content/renderer/media/webrtc_local_audio_track.h"
#include "media/audio/simple_sources.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/WebKit/public/web/WebHeap.h"
#include "third_party/opus/src/include/opus.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::InSequence;
using ::testing::Mock;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::TestWithParam;
using ::testing::ValuesIn;

namespace {

const int kDefaultBitsPerSample = 16;
const int kDefaultSampleRate = 48000;
// The |frames_per_buffer| field of AudioParameters is not used by ATR.
const int kIgnoreFramesPerBuffer = 1;

// The following parameters replicate those in audio_track_recorder.cc, see this
// file for explanations.
const int kMediaStreamAudioTrackBufferDurationMs = 10;
const int kOpusBufferDurationMs = 60;
const int kRatioInputToOutputFrames =
    kOpusBufferDurationMs / kMediaStreamAudioTrackBufferDurationMs;

const int kFramesPerBuffer = kOpusBufferDurationMs * kDefaultSampleRate / 1000;

}  // namespace

namespace content {

ACTION_P(RunClosure, closure) {
  closure.Run();
}

struct ATRTestParams {
  const media::AudioParameters::Format input_format;
  const media::ChannelLayout channel_layout;
  const int sample_rate;
  const int bits_per_sample;
};

const ATRTestParams kATRTestParams[] = {
    // Equivalent to default settings:
    {media::AudioParameters::AUDIO_PCM_LOW_LATENCY, /* input format */
     media::CHANNEL_LAYOUT_STEREO,                  /* channel layout */
     kDefaultSampleRate,                            /* sample rate */
     kDefaultBitsPerSample},                        /* bits per sample */
    // Change to mono:
    {media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
     media::CHANNEL_LAYOUT_MONO,
     kDefaultSampleRate,
     kDefaultBitsPerSample},
    // Different sampling rate as well:
    {media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
      media::CHANNEL_LAYOUT_MONO,
     24000,
     kDefaultBitsPerSample},
    {media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
     media::CHANNEL_LAYOUT_STEREO,
     8000,
     kDefaultBitsPerSample},
    // Using a non-default Opus sampling rate (48, 24, 16, 12, or 8 kHz).
    {media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
     media::CHANNEL_LAYOUT_MONO,
     22050,
     kDefaultBitsPerSample},
    {media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
     media::CHANNEL_LAYOUT_STEREO,
     44100,
     kDefaultBitsPerSample},
    {media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
     media::CHANNEL_LAYOUT_STEREO,
     96000,
     kDefaultBitsPerSample},
};

class AudioTrackRecorderTest : public TestWithParam<ATRTestParams> {
 public:
  // Initialize |first_params_| based on test parameters, and |second_params_|
  // to always be the same thing.
  AudioTrackRecorderTest()
      : first_params_(GetParam().input_format,
                      GetParam().channel_layout,
                      GetParam().sample_rate,
                      GetParam().bits_per_sample,
                      kIgnoreFramesPerBuffer),
        second_params_(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
                       media::CHANNEL_LAYOUT_STEREO,
                       kDefaultSampleRate,
                       kDefaultBitsPerSample,
                       kIgnoreFramesPerBuffer),
        first_source_(first_params_.channels(),     /* # channels */
                      440,                          /* frequency */
                      first_params_.sample_rate()), /* sample rate */
        second_source_(second_params_.channels(),
                       440,
                       second_params_.sample_rate()),
        opus_decoder_(nullptr) {
    ResetDecoder(first_params_);
    PrepareBlinkTrack();
    audio_track_recorder_.reset(new AudioTrackRecorder(
        blink_track_, base::Bind(&AudioTrackRecorderTest::OnEncodedAudio,
                                 base::Unretained(this)),
        0 /* bits_per_second */));
  }

  ~AudioTrackRecorderTest() {
    opus_decoder_destroy(opus_decoder_);
    opus_decoder_ = nullptr;
    blink_track_.reset();
    blink::WebHeap::collectAllGarbageForTesting();
    audio_track_recorder_.reset();
    // Let the message loop run to finish destroying the recorder properly.
    base::RunLoop().RunUntilIdle();
  }

  void ResetDecoder(const media::AudioParameters& params) {
    if (opus_decoder_) {
      opus_decoder_destroy(opus_decoder_);
      opus_decoder_ = nullptr;
    }

    int error;
    opus_decoder_ =
        opus_decoder_create(kDefaultSampleRate, params.channels(), &error);
    EXPECT_TRUE(error == OPUS_OK && opus_decoder_);

    buffer_.reset(new float[kFramesPerBuffer * params.channels()]);
  }

  scoped_ptr<media::AudioBus> GetFirstSourceAudioBus() {
    scoped_ptr<media::AudioBus> bus(media::AudioBus::Create(
        first_params_.channels(), first_params_.sample_rate() *
                                      kMediaStreamAudioTrackBufferDurationMs /
                                      base::Time::kMillisecondsPerSecond));
    first_source_.OnMoreData(bus.get(), 0, 0);
    return bus;
  }
  scoped_ptr<media::AudioBus> GetSecondSourceAudioBus() {
    scoped_ptr<media::AudioBus> bus(media::AudioBus::Create(
        second_params_.channels(), second_params_.sample_rate() *
                                       kMediaStreamAudioTrackBufferDurationMs /
                                       base::Time::kMillisecondsPerSecond));
    second_source_.OnMoreData(bus.get(), 0, 0);
    return bus;
  }

  MOCK_METHOD3(DoOnEncodedAudio,
               void(const media::AudioParameters& params,
                    std::string encoded_data,
                    base::TimeTicks timestamp));

  void OnEncodedAudio(const media::AudioParameters& params,
                      scoped_ptr<std::string> encoded_data,
                      base::TimeTicks timestamp) {
    EXPECT_TRUE(!encoded_data->empty());
    // Decode |encoded_data| and check we get the expected number of frames
    // per buffer.
    EXPECT_EQ(kDefaultSampleRate * kOpusBufferDurationMs / 1000,
              opus_decode_float(
                  opus_decoder_, reinterpret_cast<uint8_t*>(
                                     string_as_array(encoded_data.get())),
                  encoded_data->size(), buffer_.get(), kFramesPerBuffer, 0));

    DoOnEncodedAudio(params, *encoded_data, timestamp);
  }

  const base::MessageLoop message_loop_;

  // ATR and WebMediaStreamTrack for fooling it.
  scoped_ptr<AudioTrackRecorder> audio_track_recorder_;
  blink::WebMediaStreamTrack blink_track_;

  // Two different sets of AudioParameters for testing re-init of ATR.
  const media::AudioParameters first_params_;
  const media::AudioParameters second_params_;

  // AudioSources for creating AudioBuses.
  media::SineWaveAudioSource first_source_;
  media::SineWaveAudioSource second_source_;

  // Decoder for verifying data was properly encoded.
  OpusDecoder* opus_decoder_;
  scoped_ptr<float[]> buffer_;

 private:
  // Prepares a blink track of a given MediaStreamType and attaches the native
  // track, which can be used to capture audio data and pass it to the producer.
  // Adapted from media::WebRTCLocalAudioSourceProviderTest.
  void PrepareBlinkTrack() {
    MockMediaConstraintFactory constraint_factory;
    scoped_refptr<WebRtcAudioCapturer> capturer(
        WebRtcAudioCapturer::CreateCapturer(
            -1, StreamDeviceInfo(),
            constraint_factory.CreateWebMediaConstraints(), NULL, NULL));
    scoped_refptr<WebRtcLocalAudioTrackAdapter> adapter(
        WebRtcLocalAudioTrackAdapter::Create(std::string(), NULL));
    scoped_ptr<WebRtcLocalAudioTrack> native_track(
        new WebRtcLocalAudioTrack(adapter.get(), capturer, NULL));
    blink::WebMediaStreamSource audio_source;
    audio_source.initialize(base::UTF8ToUTF16("dummy_source_id"),
                            blink::WebMediaStreamSource::TypeAudio,
                            base::UTF8ToUTF16("dummy_source_name"),
                            false /* remote */, true /* readonly */);
    blink_track_.initialize(blink::WebString::fromUTF8("audio_track"),
                            audio_source);
    blink_track_.setExtraData(native_track.release());
  }

  DISALLOW_COPY_AND_ASSIGN(AudioTrackRecorderTest);
};

TEST_P(AudioTrackRecorderTest, OnData) {
  InSequence s;
  base::RunLoop run_loop;
  base::Closure quit_closure = run_loop.QuitClosure();

  // Give ATR initial audio parameters.
  audio_track_recorder_->OnSetFormat(first_params_);

  // TODO(ajose): consider adding WillOnce(SaveArg...) and inspecting, as done
  // in VTR unittests. http://crbug.com/548856
  EXPECT_CALL(*this, DoOnEncodedAudio(_, _, _)).Times(1);
  audio_track_recorder_->OnData(*GetFirstSourceAudioBus(),
                                base::TimeTicks::Now());
  for (int i = 0; i < kRatioInputToOutputFrames - 1; ++i) {
    audio_track_recorder_->OnData(*GetFirstSourceAudioBus(),
                                  base::TimeTicks::Now());
  }

  EXPECT_CALL(*this, DoOnEncodedAudio(_, _, _))
      .Times(1)
      // Only reset the decoder once we've heard back:
      .WillOnce(RunClosure(base::Bind(&AudioTrackRecorderTest::ResetDecoder,
                                      base::Unretained(this), second_params_)));
  audio_track_recorder_->OnData(*GetFirstSourceAudioBus(),
                                base::TimeTicks::Now());
  for (int i = 0; i < kRatioInputToOutputFrames - 1; ++i) {
    audio_track_recorder_->OnData(*GetFirstSourceAudioBus(),
                                  base::TimeTicks::Now());
  }

  // If the amount of samples/10ms buffer is not an integer (e.g. 22050Hz) we
  // need an extra OnData() to account for the round-off error.
  if (GetParam().sample_rate % 100) {
    audio_track_recorder_->OnData(*GetFirstSourceAudioBus(),
                                  base::TimeTicks::Now());
  }

  // Give ATR new audio parameters.
  audio_track_recorder_->OnSetFormat(second_params_);

  // Send audio with different params.
  EXPECT_CALL(*this, DoOnEncodedAudio(_, _, _))
      .Times(1)
      .WillOnce(RunClosure(quit_closure));
  audio_track_recorder_->OnData(*GetSecondSourceAudioBus(),
                                base::TimeTicks::Now());
  for (int i = 0; i < kRatioInputToOutputFrames - 1; ++i) {
    audio_track_recorder_->OnData(*GetSecondSourceAudioBus(),
                                  base::TimeTicks::Now());
  }

  run_loop.Run();
  Mock::VerifyAndClearExpectations(this);
}

INSTANTIATE_TEST_CASE_P(, AudioTrackRecorderTest, ValuesIn(kATRTestParams));

}  // namespace content
