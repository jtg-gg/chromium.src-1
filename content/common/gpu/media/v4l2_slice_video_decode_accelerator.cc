// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback.h"
#include "base/callback_helpers.h"
#include "base/command_line.h"
#include "base/macros.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/stringprintf.h"
#include "content/common/gpu/media/v4l2_slice_video_decode_accelerator.h"
#include "media/base/bind_to_current_loop.h"
#include "media/base/media_switches.h"
#include "ui/gl/scoped_binders.h"

#define LOGF(level) LOG(level) << __FUNCTION__ << "(): "
#define DVLOGF(level) DVLOG(level) << __FUNCTION__ << "(): "

#define NOTIFY_ERROR(x)                        \
  do {                                         \
    LOG(ERROR) << "Setting error state:" << x; \
    SetErrorState(x);                          \
  } while (0)

#define IOCTL_OR_ERROR_RETURN_VALUE(type, arg, value, type_str)           \
  do {                                                                    \
    if (device_->Ioctl(type, arg) != 0) {                                 \
      PLOG(ERROR) << __FUNCTION__ << "(): ioctl() failed: " << type_str;  \
      return value;                                                       \
    }                                                                     \
  } while (0)

#define IOCTL_OR_ERROR_RETURN(type, arg) \
  IOCTL_OR_ERROR_RETURN_VALUE(type, arg, ((void)0), #type)

#define IOCTL_OR_ERROR_RETURN_FALSE(type, arg) \
  IOCTL_OR_ERROR_RETURN_VALUE(type, arg, false, #type)

#define IOCTL_OR_LOG_ERROR(type, arg)                                 \
  do {                                                                \
    if (device_->Ioctl(type, arg) != 0)                               \
      PLOG(ERROR) << __FUNCTION__ << "(): ioctl() failed: " << #type; \
  } while (0)

namespace content {

// static
const uint32_t V4L2SliceVideoDecodeAccelerator::supported_input_fourccs_[] = {
    V4L2_PIX_FMT_H264_SLICE, V4L2_PIX_FMT_VP8_FRAME,
};

class V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface
    : public base::RefCounted<V4L2DecodeSurface> {
 public:
  using ReleaseCB = base::Callback<void(int)>;

  V4L2DecodeSurface(int32_t bitstream_id,
                    int input_record,
                    int output_record,
                    const ReleaseCB& release_cb);

  // Mark the surface as decoded. This will also release all references, as
  // they are not needed anymore.
  void SetDecoded();
  bool decoded() const { return decoded_; }

  int32_t bitstream_id() const { return bitstream_id_; }
  int input_record() const { return input_record_; }
  int output_record() const { return output_record_; }
  uint32_t config_store() const { return config_store_; }

  // Take references to each reference surface and keep them until the
  // target surface is decoded.
  void SetReferenceSurfaces(
      const std::vector<scoped_refptr<V4L2DecodeSurface>>& ref_surfaces);

  std::string ToString() const;

 private:
  friend class base::RefCounted<V4L2DecodeSurface>;
  ~V4L2DecodeSurface();

  int32_t bitstream_id_;
  int input_record_;
  int output_record_;
  uint32_t config_store_;

  bool decoded_;
  ReleaseCB release_cb_;

  std::vector<scoped_refptr<V4L2DecodeSurface>> reference_surfaces_;

  DISALLOW_COPY_AND_ASSIGN(V4L2DecodeSurface);
};

V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface::V4L2DecodeSurface(
    int32_t bitstream_id,
    int input_record,
    int output_record,
    const ReleaseCB& release_cb)
    : bitstream_id_(bitstream_id),
      input_record_(input_record),
      output_record_(output_record),
      config_store_(input_record + 1),
      decoded_(false),
      release_cb_(release_cb) {}

V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface::~V4L2DecodeSurface() {
  DVLOGF(5) << "Releasing output record id=" << output_record_;
  release_cb_.Run(output_record_);
}

void V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface::SetReferenceSurfaces(
    const std::vector<scoped_refptr<V4L2DecodeSurface>>& ref_surfaces) {
  DCHECK(reference_surfaces_.empty());
  reference_surfaces_ = ref_surfaces;
}

void V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface::SetDecoded() {
  DCHECK(!decoded_);
  decoded_ = true;

  // We can now drop references to all reference surfaces for this surface
  // as we are done with decoding.
  reference_surfaces_.clear();
}

std::string V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface::ToString()
    const {
  std::string out;
  base::StringAppendF(&out, "Buffer %d -> %d. ", input_record_, output_record_);
  base::StringAppendF(&out, "Reference surfaces:");
  for (const auto& ref : reference_surfaces_) {
    DCHECK_NE(ref->output_record(), output_record_);
    base::StringAppendF(&out, " %d", ref->output_record());
  }
  return out;
}

V4L2SliceVideoDecodeAccelerator::InputRecord::InputRecord()
    : input_id(-1),
      address(nullptr),
      length(0),
      bytes_used(0),
      at_device(false) {
}

V4L2SliceVideoDecodeAccelerator::OutputRecord::OutputRecord()
    : at_device(false),
      at_client(false),
      picture_id(-1),
      egl_image(EGL_NO_IMAGE_KHR),
      egl_sync(EGL_NO_SYNC_KHR),
      cleared(false) {
}

struct V4L2SliceVideoDecodeAccelerator::BitstreamBufferRef {
  BitstreamBufferRef(
      base::WeakPtr<VideoDecodeAccelerator::Client>& client,
      const scoped_refptr<base::SingleThreadTaskRunner>& client_task_runner,
      base::SharedMemory* shm,
      size_t size,
      int32_t input_id);
  ~BitstreamBufferRef();
  const base::WeakPtr<VideoDecodeAccelerator::Client> client;
  const scoped_refptr<base::SingleThreadTaskRunner> client_task_runner;
  const scoped_ptr<base::SharedMemory> shm;
  const size_t size;
  off_t bytes_used;
  const int32_t input_id;
};

V4L2SliceVideoDecodeAccelerator::BitstreamBufferRef::BitstreamBufferRef(
    base::WeakPtr<VideoDecodeAccelerator::Client>& client,
    const scoped_refptr<base::SingleThreadTaskRunner>& client_task_runner,
    base::SharedMemory* shm,
    size_t size,
    int32_t input_id)
    : client(client),
      client_task_runner(client_task_runner),
      shm(shm),
      size(size),
      bytes_used(0),
      input_id(input_id) {}

V4L2SliceVideoDecodeAccelerator::BitstreamBufferRef::~BitstreamBufferRef() {
  if (input_id >= 0) {
    DVLOGF(5) << "returning input_id: " << input_id;
    client_task_runner->PostTask(
        FROM_HERE,
        base::Bind(&VideoDecodeAccelerator::Client::NotifyEndOfBitstreamBuffer,
                   client, input_id));
  }
}

struct V4L2SliceVideoDecodeAccelerator::EGLSyncKHRRef {
  EGLSyncKHRRef(EGLDisplay egl_display, EGLSyncKHR egl_sync);
  ~EGLSyncKHRRef();
  EGLDisplay const egl_display;
  EGLSyncKHR egl_sync;
};

V4L2SliceVideoDecodeAccelerator::EGLSyncKHRRef::EGLSyncKHRRef(
    EGLDisplay egl_display,
    EGLSyncKHR egl_sync)
    : egl_display(egl_display), egl_sync(egl_sync) {
}

V4L2SliceVideoDecodeAccelerator::EGLSyncKHRRef::~EGLSyncKHRRef() {
  // We don't check for eglDestroySyncKHR failures, because if we get here
  // with a valid sync object, something went wrong and we are getting
  // destroyed anyway.
  if (egl_sync != EGL_NO_SYNC_KHR)
    eglDestroySyncKHR(egl_display, egl_sync);
}

struct V4L2SliceVideoDecodeAccelerator::PictureRecord {
  PictureRecord(bool cleared, const media::Picture& picture);
  ~PictureRecord();
  bool cleared;  // Whether the texture is cleared and safe to render from.
  media::Picture picture;  // The decoded picture.
};

V4L2SliceVideoDecodeAccelerator::PictureRecord::PictureRecord(
    bool cleared,
    const media::Picture& picture)
    : cleared(cleared), picture(picture) {
}

V4L2SliceVideoDecodeAccelerator::PictureRecord::~PictureRecord() {
}

class V4L2SliceVideoDecodeAccelerator::V4L2H264Accelerator
    : public H264Decoder::H264Accelerator {
 public:
  V4L2H264Accelerator(V4L2SliceVideoDecodeAccelerator* v4l2_dec);
  ~V4L2H264Accelerator() override;

  // H264Decoder::H264Accelerator implementation.
  scoped_refptr<H264Picture> CreateH264Picture() override;

  bool SubmitFrameMetadata(const media::H264SPS* sps,
                           const media::H264PPS* pps,
                           const H264DPB& dpb,
                           const H264Picture::Vector& ref_pic_listp0,
                           const H264Picture::Vector& ref_pic_listb0,
                           const H264Picture::Vector& ref_pic_listb1,
                           const scoped_refptr<H264Picture>& pic) override;

  bool SubmitSlice(const media::H264PPS* pps,
                   const media::H264SliceHeader* slice_hdr,
                   const H264Picture::Vector& ref_pic_list0,
                   const H264Picture::Vector& ref_pic_list1,
                   const scoped_refptr<H264Picture>& pic,
                   const uint8_t* data,
                   size_t size) override;

  bool SubmitDecode(const scoped_refptr<H264Picture>& pic) override;
  bool OutputPicture(const scoped_refptr<H264Picture>& pic) override;

  void Reset() override;

 private:
  // Max size of reference list.
  static const size_t kDPBIndicesListSize = 32;
  void H264PictureListToDPBIndicesList(const H264Picture::Vector& src_pic_list,
                                       uint8_t dst_list[kDPBIndicesListSize]);

  void H264DPBToV4L2DPB(
      const H264DPB& dpb,
      std::vector<scoped_refptr<V4L2DecodeSurface>>* ref_surfaces);

  scoped_refptr<V4L2DecodeSurface> H264PictureToV4L2DecodeSurface(
      const scoped_refptr<H264Picture>& pic);

  size_t num_slices_;
  V4L2SliceVideoDecodeAccelerator* v4l2_dec_;

  // TODO(posciak): This should be queried from hardware once supported.
  static const size_t kMaxSlices = 16;
  struct v4l2_ctrl_h264_slice_param v4l2_slice_params_[kMaxSlices];
  struct v4l2_ctrl_h264_decode_param v4l2_decode_param_;

  DISALLOW_COPY_AND_ASSIGN(V4L2H264Accelerator);
};

class V4L2SliceVideoDecodeAccelerator::V4L2VP8Accelerator
    : public VP8Decoder::VP8Accelerator {
 public:
  V4L2VP8Accelerator(V4L2SliceVideoDecodeAccelerator* v4l2_dec);
  ~V4L2VP8Accelerator() override;

  // VP8Decoder::VP8Accelerator implementation.
  scoped_refptr<VP8Picture> CreateVP8Picture() override;

  bool SubmitDecode(const scoped_refptr<VP8Picture>& pic,
                    const media::Vp8FrameHeader* frame_hdr,
                    const scoped_refptr<VP8Picture>& last_frame,
                    const scoped_refptr<VP8Picture>& golden_frame,
                    const scoped_refptr<VP8Picture>& alt_frame) override;

  bool OutputPicture(const scoped_refptr<VP8Picture>& pic) override;

 private:
  scoped_refptr<V4L2DecodeSurface> VP8PictureToV4L2DecodeSurface(
      const scoped_refptr<VP8Picture>& pic);

  V4L2SliceVideoDecodeAccelerator* v4l2_dec_;

  DISALLOW_COPY_AND_ASSIGN(V4L2VP8Accelerator);
};

// Codec-specific subclasses of software decoder picture classes.
// This allows us to keep decoders oblivious of our implementation details.
class V4L2H264Picture : public H264Picture {
 public:
  V4L2H264Picture(const scoped_refptr<
      V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface>& dec_surface);

  V4L2H264Picture* AsV4L2H264Picture() override { return this; }
  scoped_refptr<V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface>
  dec_surface() {
    return dec_surface_;
  }

 private:
  ~V4L2H264Picture() override;

  scoped_refptr<V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface>
      dec_surface_;

  DISALLOW_COPY_AND_ASSIGN(V4L2H264Picture);
};

V4L2H264Picture::V4L2H264Picture(const scoped_refptr<
    V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface>& dec_surface)
    : dec_surface_(dec_surface) {
}

V4L2H264Picture::~V4L2H264Picture() {
}

class V4L2VP8Picture : public VP8Picture {
 public:
  V4L2VP8Picture(const scoped_refptr<
      V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface>& dec_surface);

  V4L2VP8Picture* AsV4L2VP8Picture() override { return this; }
  scoped_refptr<V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface>
  dec_surface() {
    return dec_surface_;
  }

 private:
  ~V4L2VP8Picture() override;

  scoped_refptr<V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface>
      dec_surface_;

  DISALLOW_COPY_AND_ASSIGN(V4L2VP8Picture);
};

V4L2VP8Picture::V4L2VP8Picture(const scoped_refptr<
    V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface>& dec_surface)
    : dec_surface_(dec_surface) {
}

V4L2VP8Picture::~V4L2VP8Picture() {
}

V4L2SliceVideoDecodeAccelerator::V4L2SliceVideoDecodeAccelerator(
    const scoped_refptr<V4L2Device>& device,
    EGLDisplay egl_display,
    EGLContext egl_context,
    const base::WeakPtr<Client>& io_client,
    const base::Callback<bool(void)>& make_context_current,
    const scoped_refptr<base::SingleThreadTaskRunner>& io_task_runner)
    : input_planes_count_(0),
      output_planes_count_(0),
      child_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      io_task_runner_(io_task_runner),
      io_client_(io_client),
      device_(device),
      decoder_thread_("V4L2SliceVideoDecodeAcceleratorThread"),
      device_poll_thread_("V4L2SliceVideoDecodeAcceleratorDevicePollThread"),
      input_streamon_(false),
      input_buffer_queued_count_(0),
      output_streamon_(false),
      output_buffer_queued_count_(0),
      video_profile_(media::VIDEO_CODEC_PROFILE_UNKNOWN),
      output_format_fourcc_(0),
      state_(kUninitialized),
      decoder_flushing_(false),
      decoder_resetting_(false),
      surface_set_change_pending_(false),
      picture_clearing_count_(0),
      make_context_current_(make_context_current),
      pictures_assigned_(false, false),
      egl_display_(egl_display),
      egl_context_(egl_context),
      weak_this_factory_(this) {
  weak_this_ = weak_this_factory_.GetWeakPtr();
}

V4L2SliceVideoDecodeAccelerator::~V4L2SliceVideoDecodeAccelerator() {
  DVLOGF(2);

  DCHECK(child_task_runner_->BelongsToCurrentThread());
  DCHECK(!decoder_thread_.IsRunning());
  DCHECK(!device_poll_thread_.IsRunning());

  DCHECK(input_buffer_map_.empty());
  DCHECK(output_buffer_map_.empty());
}

void V4L2SliceVideoDecodeAccelerator::NotifyError(Error error) {
  if (!child_task_runner_->BelongsToCurrentThread()) {
    child_task_runner_->PostTask(
        FROM_HERE, base::Bind(&V4L2SliceVideoDecodeAccelerator::NotifyError,
                              weak_this_, error));
    return;
  }

  if (client_) {
    client_->NotifyError(error);
    client_ptr_factory_.reset();
  }
}

bool V4L2SliceVideoDecodeAccelerator::Initialize(const Config& config,
                                                 Client* client) {
  DVLOGF(3) << "profile: " << config.profile;
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(state_, kUninitialized);

  if (config.is_encrypted) {
    NOTREACHED() << "Encrypted streams are not supported for this VDA";
    return false;
  }

  if (!device_->SupportsDecodeProfileForV4L2PixelFormats(
          config.profile, arraysize(supported_input_fourccs_),
          supported_input_fourccs_)) {
    DVLOGF(1) << "unsupported profile " << config.profile;
    return false;
  }

  client_ptr_factory_.reset(
      new base::WeakPtrFactory<VideoDecodeAccelerator::Client>(client));
  client_ = client_ptr_factory_->GetWeakPtr();

  video_profile_ = config.profile;

  if (video_profile_ >= media::H264PROFILE_MIN &&
      video_profile_ <= media::H264PROFILE_MAX) {
    h264_accelerator_.reset(new V4L2H264Accelerator(this));
    decoder_.reset(new H264Decoder(h264_accelerator_.get()));
  } else if (video_profile_ >= media::VP8PROFILE_MIN &&
             video_profile_ <= media::VP8PROFILE_MAX) {
    vp8_accelerator_.reset(new V4L2VP8Accelerator(this));
    decoder_.reset(new VP8Decoder(vp8_accelerator_.get()));
  } else {
    NOTREACHED() << "Unsupported profile " << video_profile_;
    return false;
  }

  // TODO(posciak): This needs to be queried once supported.
  input_planes_count_ = 1;
  output_planes_count_ = 1;

  if (egl_display_ == EGL_NO_DISPLAY) {
    LOG(ERROR) << "Initialize(): could not get EGLDisplay";
    return false;
  }

  // We need the context to be initialized to query extensions.
  if (!make_context_current_.Run()) {
    LOG(ERROR) << "Initialize(): could not make context current";
    return false;
  }

  if (!gfx::g_driver_egl.ext.b_EGL_KHR_fence_sync) {
    LOG(ERROR) << "Initialize(): context does not have EGL_KHR_fence_sync";
    return false;
  }

  // Capabilities check.
  struct v4l2_capability caps;
  const __u32 kCapsRequired = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_QUERYCAP, &caps);
  if ((caps.capabilities & kCapsRequired) != kCapsRequired) {
    LOG(ERROR) << "Initialize(): ioctl() failed: VIDIOC_QUERYCAP"
                  ", caps check failed: 0x" << std::hex << caps.capabilities;
    return false;
  }

  if (!SetupFormats())
    return false;

  if (!decoder_thread_.Start()) {
    DLOG(ERROR) << "Initialize(): device thread failed to start";
    return false;
  }
  decoder_thread_task_runner_ = decoder_thread_.task_runner();

  state_ = kInitialized;

  // InitializeTask will NOTIFY_ERROR on failure.
  decoder_thread_task_runner_->PostTask(
      FROM_HERE, base::Bind(&V4L2SliceVideoDecodeAccelerator::InitializeTask,
                            base::Unretained(this)));

  DVLOGF(1) << "V4L2SliceVideoDecodeAccelerator initialized";
  return true;
}

void V4L2SliceVideoDecodeAccelerator::InitializeTask() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(state_, kInitialized);

  if (!CreateInputBuffers())
    NOTIFY_ERROR(PLATFORM_FAILURE);

  // Output buffers will be created once decoder gives us information
  // about their size and required count.
  state_ = kDecoding;
}

void V4L2SliceVideoDecodeAccelerator::Destroy() {
  DVLOGF(3);
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  if (decoder_thread_.IsRunning()) {
    decoder_thread_task_runner_->PostTask(
        FROM_HERE, base::Bind(&V4L2SliceVideoDecodeAccelerator::DestroyTask,
                              base::Unretained(this)));

    // Wake up decoder thread in case we are waiting in CreateOutputBuffers
    // for client to provide pictures. Since this is Destroy, we won't be
    // getting them anymore (AssignPictureBuffers won't be called).
    pictures_assigned_.Signal();

    // Wait for tasks to finish/early-exit.
    decoder_thread_.Stop();
  }

  delete this;
  DVLOGF(3) << "Destroyed";
}

void V4L2SliceVideoDecodeAccelerator::DestroyTask() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  state_ = kError;

  decoder_->Reset();

  decoder_current_bitstream_buffer_.reset();
  while (!decoder_input_queue_.empty())
    decoder_input_queue_.pop();

  // Stop streaming and the device_poll_thread_.
  StopDevicePoll(false);

  DestroyInputBuffers();
  DestroyOutputs(false);

  DCHECK(surfaces_at_device_.empty());
  DCHECK(surfaces_at_display_.empty());
  DCHECK(decoder_display_queue_.empty());
}

bool V4L2SliceVideoDecodeAccelerator::SetupFormats() {
  DCHECK_EQ(state_, kUninitialized);

  __u32 input_format_fourcc =
      V4L2Device::VideoCodecProfileToV4L2PixFmt(video_profile_, true);
  if (!input_format_fourcc) {
    NOTREACHED();
    return false;
  }

  size_t input_size;
  gfx::Size max_resolution, min_resolution;
  device_->GetSupportedResolution(input_format_fourcc, &min_resolution,
                                  &max_resolution);
  if (max_resolution.width() > 1920 && max_resolution.height() > 1088)
    input_size = kInputBufferMaxSizeFor4k;
  else
    input_size = kInputBufferMaxSizeFor1080p;

  struct v4l2_fmtdesc fmtdesc;
  memset(&fmtdesc, 0, sizeof(fmtdesc));
  fmtdesc.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  bool is_format_supported = false;
  while (device_->Ioctl(VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    if (fmtdesc.pixelformat == input_format_fourcc) {
      is_format_supported = true;
      break;
    }
    ++fmtdesc.index;
  }

  if (!is_format_supported) {
    DVLOG(1) << "Input fourcc " << input_format_fourcc
             << " not supported by device.";
    return false;
  }

  struct v4l2_format format;
  memset(&format, 0, sizeof(format));
  format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  format.fmt.pix_mp.pixelformat = input_format_fourcc;
  format.fmt.pix_mp.plane_fmt[0].sizeimage = input_size;
  format.fmt.pix_mp.num_planes = input_planes_count_;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_S_FMT, &format);

  // We have to set up the format for output, because the driver may not allow
  // changing it once we start streaming; whether it can support our chosen
  // output format or not may depend on the input format.
  memset(&fmtdesc, 0, sizeof(fmtdesc));
  fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  output_format_fourcc_ = 0;
  while (device_->Ioctl(VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    if (device_->CanCreateEGLImageFrom(fmtdesc.pixelformat)) {
      output_format_fourcc_ = fmtdesc.pixelformat;
      break;
    }
    ++fmtdesc.index;
  }

  if (output_format_fourcc_ == 0) {
    LOG(ERROR) << "Could not find a usable output format";
    return false;
  }

  // Only set fourcc for output; resolution, etc., will come from the
  // driver once it extracts it from the stream.
  memset(&format, 0, sizeof(format));
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  format.fmt.pix_mp.pixelformat = output_format_fourcc_;
  format.fmt.pix_mp.num_planes = output_planes_count_;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_S_FMT, &format);

  return true;
}

bool V4L2SliceVideoDecodeAccelerator::CreateInputBuffers() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());
  DCHECK(!input_streamon_);
  DCHECK(input_buffer_map_.empty());

  struct v4l2_requestbuffers reqbufs;
  memset(&reqbufs, 0, sizeof(reqbufs));
  reqbufs.count = kNumInputBuffers;
  reqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  reqbufs.memory = V4L2_MEMORY_MMAP;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_REQBUFS, &reqbufs);
  if (reqbufs.count < kNumInputBuffers) {
    PLOG(ERROR) << "Could not allocate enough output buffers";
    return false;
  }
  input_buffer_map_.resize(reqbufs.count);
  for (size_t i = 0; i < input_buffer_map_.size(); ++i) {
    free_input_buffers_.push_back(i);

    // Query for the MEMORY_MMAP pointer.
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    struct v4l2_buffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    memset(planes, 0, sizeof(planes));
    buffer.index = i;
    buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.m.planes = planes;
    buffer.length = input_planes_count_;
    IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_QUERYBUF, &buffer);
    void* address = device_->Mmap(nullptr,
                                  buffer.m.planes[0].length,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED,
                                  buffer.m.planes[0].m.mem_offset);
    if (address == MAP_FAILED) {
      PLOG(ERROR) << "CreateInputBuffers(): mmap() failed";
      return false;
    }
    input_buffer_map_[i].address = address;
    input_buffer_map_[i].length = buffer.m.planes[0].length;
  }

  return true;
}

bool V4L2SliceVideoDecodeAccelerator::CreateOutputBuffers() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());
  DCHECK(!output_streamon_);
  DCHECK(output_buffer_map_.empty());
  DCHECK(surfaces_at_display_.empty());
  DCHECK(surfaces_at_device_.empty());

  visible_size_ = decoder_->GetPicSize();
  size_t num_pictures = decoder_->GetRequiredNumOfPictures();

  DCHECK_GT(num_pictures, 0u);
  DCHECK(!visible_size_.IsEmpty());

  struct v4l2_format format;
  memset(&format, 0, sizeof(format));
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  format.fmt.pix_mp.pixelformat = output_format_fourcc_;
  format.fmt.pix_mp.width = visible_size_.width();
  format.fmt.pix_mp.height = visible_size_.height();
  format.fmt.pix_mp.num_planes = input_planes_count_;

  if (device_->Ioctl(VIDIOC_S_FMT, &format) != 0) {
    PLOG(ERROR) << "Failed setting format to: " << output_format_fourcc_;
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return false;
  }

  coded_size_.SetSize(base::checked_cast<int>(format.fmt.pix_mp.width),
                      base::checked_cast<int>(format.fmt.pix_mp.height));
  DCHECK_EQ(coded_size_.width() % 16, 0);
  DCHECK_EQ(coded_size_.height() % 16, 0);

  if (!gfx::Rect(coded_size_).Contains(gfx::Rect(visible_size_))) {
    LOG(ERROR) << "Got invalid adjusted coded size: " << coded_size_.ToString();
    return false;
  }

  DVLOGF(3) << "buffer_count=" << num_pictures
            << ", visible size=" << visible_size_.ToString()
            << ", coded size=" << coded_size_.ToString();

  child_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&VideoDecodeAccelerator::Client::ProvidePictureBuffers,
                 client_, num_pictures, coded_size_,
                 device_->GetTextureTarget()));

  // Wait for the client to call AssignPictureBuffers() on the Child thread.
  // We do this, because if we continue decoding without finishing buffer
  // allocation, we may end up Resetting before AssignPictureBuffers arrives,
  // resulting in unnecessary complications and subtle bugs.
  pictures_assigned_.Wait();

  return true;
}

void V4L2SliceVideoDecodeAccelerator::DestroyInputBuffers() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread() ||
         !decoder_thread_.IsRunning());
  DCHECK(!input_streamon_);

  for (auto& input_record : input_buffer_map_) {
    if (input_record.address != nullptr)
      device_->Munmap(input_record.address, input_record.length);
  }

  struct v4l2_requestbuffers reqbufs;
  memset(&reqbufs, 0, sizeof(reqbufs));
  reqbufs.count = 0;
  reqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  reqbufs.memory = V4L2_MEMORY_MMAP;
  IOCTL_OR_LOG_ERROR(VIDIOC_REQBUFS, &reqbufs);

  input_buffer_map_.clear();
  free_input_buffers_.clear();
}

void V4L2SliceVideoDecodeAccelerator::DismissPictures(
    std::vector<int32_t> picture_buffer_ids,
    base::WaitableEvent* done) {
  DVLOGF(3);
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  for (auto picture_buffer_id : picture_buffer_ids) {
    DVLOGF(1) << "dismissing PictureBuffer id=" << picture_buffer_id;
    client_->DismissPictureBuffer(picture_buffer_id);
  }

  done->Signal();
}

void V4L2SliceVideoDecodeAccelerator::DevicePollTask(bool poll_device) {
  DVLOGF(4);
  DCHECK_EQ(device_poll_thread_.message_loop(), base::MessageLoop::current());

  bool event_pending;
  if (!device_->Poll(poll_device, &event_pending)) {
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  // All processing should happen on ServiceDeviceTask(), since we shouldn't
  // touch encoder state from this thread.
  decoder_thread_task_runner_->PostTask(
      FROM_HERE, base::Bind(&V4L2SliceVideoDecodeAccelerator::ServiceDeviceTask,
                            base::Unretained(this)));
}

void V4L2SliceVideoDecodeAccelerator::ServiceDeviceTask() {
  DVLOGF(4);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  // ServiceDeviceTask() should only ever be scheduled from DevicePollTask().

  Dequeue();
  SchedulePollIfNeeded();
}

void V4L2SliceVideoDecodeAccelerator::SchedulePollIfNeeded() {
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  if (!device_poll_thread_.IsRunning()) {
    DVLOGF(2) << "Device poll thread stopped, will not schedule poll";
    return;
  }

  DCHECK(input_streamon_ || output_streamon_);

  if (input_buffer_queued_count_ + output_buffer_queued_count_ == 0) {
    DVLOGF(4) << "No buffers queued, will not schedule poll";
    return;
  }

  DVLOGF(4) << "Scheduling device poll task";

  device_poll_thread_.message_loop()->PostTask(
      FROM_HERE, base::Bind(&V4L2SliceVideoDecodeAccelerator::DevicePollTask,
                            base::Unretained(this), true));

  DVLOGF(2) << "buffer counts: "
            << "INPUT[" << decoder_input_queue_.size() << "]"
            << " => DEVICE["
            << free_input_buffers_.size() << "+"
            << input_buffer_queued_count_ << "/"
            << input_buffer_map_.size() << "]->["
            << free_output_buffers_.size() << "+"
            << output_buffer_queued_count_ << "/"
            << output_buffer_map_.size() << "]"
            << " => DISPLAYQ[" << decoder_display_queue_.size() << "]"
            << " => CLIENT[" << surfaces_at_display_.size() << "]";
}

void V4L2SliceVideoDecodeAccelerator::Enqueue(
    const scoped_refptr<V4L2DecodeSurface>& dec_surface) {
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  const int old_inputs_queued = input_buffer_queued_count_;
  const int old_outputs_queued = output_buffer_queued_count_;

  if (!EnqueueInputRecord(dec_surface->input_record(),
                          dec_surface->config_store())) {
    DVLOGF(1) << "Failed queueing an input buffer";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  if (!EnqueueOutputRecord(dec_surface->output_record())) {
    DVLOGF(1) << "Failed queueing an output buffer";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  bool inserted =
      surfaces_at_device_.insert(std::make_pair(dec_surface->output_record(),
                                                dec_surface)).second;
  DCHECK(inserted);

  if (old_inputs_queued == 0 && old_outputs_queued == 0)
    SchedulePollIfNeeded();
}

void V4L2SliceVideoDecodeAccelerator::Dequeue() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  struct v4l2_buffer dqbuf;
  struct v4l2_plane planes[VIDEO_MAX_PLANES];
  while (input_buffer_queued_count_ > 0) {
    DCHECK(input_streamon_);
    memset(&dqbuf, 0, sizeof(dqbuf));
    memset(&planes, 0, sizeof(planes));
    dqbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    dqbuf.memory = V4L2_MEMORY_USERPTR;
    dqbuf.m.planes = planes;
    dqbuf.length = input_planes_count_;
    if (device_->Ioctl(VIDIOC_DQBUF, &dqbuf) != 0) {
      if (errno == EAGAIN) {
        // EAGAIN if we're just out of buffers to dequeue.
        break;
      }
      PLOG(ERROR) << "ioctl() failed: VIDIOC_DQBUF";
      NOTIFY_ERROR(PLATFORM_FAILURE);
      return;
    }
    InputRecord& input_record = input_buffer_map_[dqbuf.index];
    DCHECK(input_record.at_device);
    input_record.at_device = false;
    ReuseInputBuffer(dqbuf.index);
    input_buffer_queued_count_--;
    DVLOGF(4) << "Dequeued input=" << dqbuf.index
              << " count: " << input_buffer_queued_count_;
  }

  while (output_buffer_queued_count_ > 0) {
    DCHECK(output_streamon_);
    memset(&dqbuf, 0, sizeof(dqbuf));
    memset(&planes, 0, sizeof(planes));
    dqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    dqbuf.memory = V4L2_MEMORY_MMAP;
    dqbuf.m.planes = planes;
    dqbuf.length = output_planes_count_;
    if (device_->Ioctl(VIDIOC_DQBUF, &dqbuf) != 0) {
      if (errno == EAGAIN) {
        // EAGAIN if we're just out of buffers to dequeue.
        break;
      }
      PLOG(ERROR) << "ioctl() failed: VIDIOC_DQBUF";
      NOTIFY_ERROR(PLATFORM_FAILURE);
      return;
    }
    OutputRecord& output_record = output_buffer_map_[dqbuf.index];
    DCHECK(output_record.at_device);
    output_record.at_device = false;
    output_buffer_queued_count_--;
    DVLOGF(3) << "Dequeued output=" << dqbuf.index
              << " count " << output_buffer_queued_count_;

    V4L2DecodeSurfaceByOutputId::iterator it =
        surfaces_at_device_.find(dqbuf.index);
    if (it == surfaces_at_device_.end()) {
      DLOG(ERROR) << "Got invalid surface from device.";
      NOTIFY_ERROR(PLATFORM_FAILURE);
    }

    it->second->SetDecoded();
    surfaces_at_device_.erase(it);
  }

  // A frame was decoded, see if we can output it.
  TryOutputSurfaces();

  ProcessPendingEventsIfNeeded();
}

void V4L2SliceVideoDecodeAccelerator::ProcessPendingEventsIfNeeded() {
  // Process pending events, if any, in the correct order.
  // We always first process the surface set change, as it is an internal
  // event from the decoder and interleaving it with external requests would
  // put the decoder in an undefined state.
  FinishSurfaceSetChangeIfNeeded();

  // Process external (client) requests.
  FinishFlushIfNeeded();
  FinishResetIfNeeded();
}

void V4L2SliceVideoDecodeAccelerator::ReuseInputBuffer(int index) {
  DVLOGF(4) << "Reusing input buffer, index=" << index;
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  DCHECK_LT(index, static_cast<int>(input_buffer_map_.size()));
  InputRecord& input_record = input_buffer_map_[index];

  DCHECK(!input_record.at_device);
  input_record.input_id = -1;
  input_record.bytes_used = 0;

  DCHECK_EQ(std::count(free_input_buffers_.begin(), free_input_buffers_.end(),
            index), 0);
  free_input_buffers_.push_back(index);
}

void V4L2SliceVideoDecodeAccelerator::ReuseOutputBuffer(int index) {
  DVLOGF(4) << "Reusing output buffer, index=" << index;
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  DCHECK_LT(index, static_cast<int>(output_buffer_map_.size()));
  OutputRecord& output_record = output_buffer_map_[index];
  DCHECK(!output_record.at_device);
  DCHECK(!output_record.at_client);

  DCHECK_EQ(std::count(free_output_buffers_.begin(), free_output_buffers_.end(),
            index), 0);
  free_output_buffers_.push_back(index);

  ScheduleDecodeBufferTaskIfNeeded();
}

bool V4L2SliceVideoDecodeAccelerator::EnqueueInputRecord(
    int index,
    uint32_t config_store) {
  DVLOGF(3);
  DCHECK_LT(index, static_cast<int>(input_buffer_map_.size()));
  DCHECK_GT(config_store, 0u);

  // Enqueue an input (VIDEO_OUTPUT) buffer for an input video frame.
  InputRecord& input_record = input_buffer_map_[index];
  DCHECK(!input_record.at_device);
  struct v4l2_buffer qbuf;
  struct v4l2_plane qbuf_planes[VIDEO_MAX_PLANES];
  memset(&qbuf, 0, sizeof(qbuf));
  memset(qbuf_planes, 0, sizeof(qbuf_planes));
  qbuf.index = index;
  qbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  qbuf.memory = V4L2_MEMORY_MMAP;
  qbuf.m.planes = qbuf_planes;
  qbuf.m.planes[0].bytesused = input_record.bytes_used;
  qbuf.length = input_planes_count_;
  qbuf.config_store = config_store;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_QBUF, &qbuf);
  input_record.at_device = true;
  input_buffer_queued_count_++;
  DVLOGF(4) << "Enqueued input=" << qbuf.index
            << " count: " << input_buffer_queued_count_;

  return true;
}

bool V4L2SliceVideoDecodeAccelerator::EnqueueOutputRecord(int index) {
  DVLOGF(3);
  DCHECK_LT(index, static_cast<int>(output_buffer_map_.size()));

  // Enqueue an output (VIDEO_CAPTURE) buffer.
  OutputRecord& output_record = output_buffer_map_[index];
  DCHECK(!output_record.at_device);
  DCHECK(!output_record.at_client);
  DCHECK_NE(output_record.egl_image, EGL_NO_IMAGE_KHR);
  DCHECK_NE(output_record.picture_id, -1);

  if (output_record.egl_sync != EGL_NO_SYNC_KHR) {
    // If we have to wait for completion, wait.  Note that
    // free_output_buffers_ is a FIFO queue, so we always wait on the
    // buffer that has been in the queue the longest.
    if (eglClientWaitSyncKHR(egl_display_, output_record.egl_sync, 0,
                             EGL_FOREVER_KHR) == EGL_FALSE) {
      // This will cause tearing, but is safe otherwise.
      DVLOGF(1) << "eglClientWaitSyncKHR failed!";
    }
    if (eglDestroySyncKHR(egl_display_, output_record.egl_sync) != EGL_TRUE) {
      LOGF(ERROR) << "eglDestroySyncKHR failed!";
      NOTIFY_ERROR(PLATFORM_FAILURE);
      return false;
    }
    output_record.egl_sync = EGL_NO_SYNC_KHR;
  }

  struct v4l2_buffer qbuf;
  struct v4l2_plane qbuf_planes[VIDEO_MAX_PLANES];
  memset(&qbuf, 0, sizeof(qbuf));
  memset(qbuf_planes, 0, sizeof(qbuf_planes));
  qbuf.index = index;
  qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  qbuf.memory = V4L2_MEMORY_MMAP;
  qbuf.m.planes = qbuf_planes;
  qbuf.length = output_planes_count_;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_QBUF, &qbuf);
  output_record.at_device = true;
  output_buffer_queued_count_++;
  DVLOGF(4) << "Enqueued output=" << qbuf.index
            << " count: " << output_buffer_queued_count_;

  return true;
}

bool V4L2SliceVideoDecodeAccelerator::StartDevicePoll() {
  DVLOGF(3) << "Starting device poll";
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());
  DCHECK(!device_poll_thread_.IsRunning());

  // Start up the device poll thread and schedule its first DevicePollTask().
  if (!device_poll_thread_.Start()) {
    DLOG(ERROR) << "StartDevicePoll(): Device thread failed to start";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return false;
  }
  if (!input_streamon_) {
    __u32 type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_STREAMON, &type);
    input_streamon_ = true;
  }

  if (!output_streamon_) {
    __u32 type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_STREAMON, &type);
    output_streamon_ = true;
  }

  device_poll_thread_.message_loop()->PostTask(
      FROM_HERE, base::Bind(&V4L2SliceVideoDecodeAccelerator::DevicePollTask,
                            base::Unretained(this), true));

  return true;
}

bool V4L2SliceVideoDecodeAccelerator::StopDevicePoll(bool keep_input_state) {
  DVLOGF(3) << "Stopping device poll";
  if (decoder_thread_.IsRunning())
    DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  // Signal the DevicePollTask() to stop, and stop the device poll thread.
  if (!device_->SetDevicePollInterrupt()) {
    PLOG(ERROR) << "SetDevicePollInterrupt(): failed";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return false;
  }
  device_poll_thread_.Stop();
  DVLOGF(3) << "Device poll thread stopped";

  // Clear the interrupt now, to be sure.
  if (!device_->ClearDevicePollInterrupt()) {
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return false;
  }

  if (!keep_input_state) {
    if (input_streamon_) {
      __u32 type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_STREAMOFF, &type);
    }
    input_streamon_ = false;
  }

  if (output_streamon_) {
    __u32 type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_STREAMOFF, &type);
  }
  output_streamon_ = false;

  if (!keep_input_state) {
    for (size_t i = 0; i < input_buffer_map_.size(); ++i) {
      InputRecord& input_record = input_buffer_map_[i];
      if (input_record.at_device) {
        input_record.at_device = false;
        ReuseInputBuffer(i);
        input_buffer_queued_count_--;
      }
    }
    DCHECK_EQ(input_buffer_queued_count_, 0);
  }

  // STREAMOFF makes the driver drop all buffers without decoding and DQBUFing,
  // so we mark them all as at_device = false and clear surfaces_at_device_.
  for (size_t i = 0; i < output_buffer_map_.size(); ++i) {
    OutputRecord& output_record = output_buffer_map_[i];
    if (output_record.at_device) {
      output_record.at_device = false;
      output_buffer_queued_count_--;
    }
  }
  surfaces_at_device_.clear();
  DCHECK_EQ(output_buffer_queued_count_, 0);

  // Drop all surfaces that were awaiting decode before being displayed,
  // since we've just cancelled all outstanding decodes.
  while (!decoder_display_queue_.empty())
    decoder_display_queue_.pop();

  DVLOGF(3) << "Device poll stopped";
  return true;
}

void V4L2SliceVideoDecodeAccelerator::Decode(
    const media::BitstreamBuffer& bitstream_buffer) {
  DVLOGF(3) << "input_id=" << bitstream_buffer.id()
            << ", size=" << bitstream_buffer.size();
  DCHECK(io_task_runner_->BelongsToCurrentThread());

  if (bitstream_buffer.id() < 0) {
    LOG(ERROR) << "Invalid bitstream_buffer, id: " << bitstream_buffer.id();
    if (base::SharedMemory::IsHandleValid(bitstream_buffer.handle()))
      base::SharedMemory::CloseHandle(bitstream_buffer.handle());
    NOTIFY_ERROR(INVALID_ARGUMENT);
    return;
  }

  decoder_thread_task_runner_->PostTask(
      FROM_HERE, base::Bind(&V4L2SliceVideoDecodeAccelerator::DecodeTask,
                            base::Unretained(this), bitstream_buffer));
}

void V4L2SliceVideoDecodeAccelerator::DecodeTask(
    const media::BitstreamBuffer& bitstream_buffer) {
  DVLOGF(3) << "input_id=" << bitstream_buffer.id()
            << " size=" << bitstream_buffer.size();
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  scoped_ptr<BitstreamBufferRef> bitstream_record(new BitstreamBufferRef(
      io_client_, io_task_runner_,
      new base::SharedMemory(bitstream_buffer.handle(), true),
      bitstream_buffer.size(), bitstream_buffer.id()));
  if (!bitstream_record->shm->Map(bitstream_buffer.size())) {
    LOGF(ERROR) << "Could not map bitstream_buffer";
    NOTIFY_ERROR(UNREADABLE_INPUT);
    return;
  }
  DVLOGF(3) << "mapped at=" << bitstream_record->shm->memory();

  decoder_input_queue_.push(
      linked_ptr<BitstreamBufferRef>(bitstream_record.release()));

  ScheduleDecodeBufferTaskIfNeeded();
}

bool V4L2SliceVideoDecodeAccelerator::TrySetNewBistreamBuffer() {
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());
  DCHECK(!decoder_current_bitstream_buffer_);

  if (decoder_input_queue_.empty())
    return false;

  decoder_current_bitstream_buffer_.reset(
      decoder_input_queue_.front().release());
  decoder_input_queue_.pop();

  if (decoder_current_bitstream_buffer_->input_id == kFlushBufferId) {
    // This is a buffer we queued for ourselves to trigger flush at this time.
    InitiateFlush();
    return false;
  }

  const uint8_t* const data = reinterpret_cast<const uint8_t*>(
      decoder_current_bitstream_buffer_->shm->memory());
  const size_t data_size = decoder_current_bitstream_buffer_->size;
  decoder_->SetStream(data, data_size);

  return true;
}

void V4L2SliceVideoDecodeAccelerator::ScheduleDecodeBufferTaskIfNeeded() {
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());
  if (state_ == kDecoding) {
    decoder_thread_task_runner_->PostTask(
        FROM_HERE,
        base::Bind(&V4L2SliceVideoDecodeAccelerator::DecodeBufferTask,
                   base::Unretained(this)));
  }
}

void V4L2SliceVideoDecodeAccelerator::DecodeBufferTask() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  if (state_ != kDecoding) {
    DVLOGF(3) << "Early exit, not in kDecoding";
    return;
  }

  while (true) {
    AcceleratedVideoDecoder::DecodeResult res;
    res = decoder_->Decode();
    switch (res) {
      case AcceleratedVideoDecoder::kAllocateNewSurfaces:
        DVLOGF(2) << "Decoder requesting a new set of surfaces";
        InitiateSurfaceSetChange();
        return;

      case AcceleratedVideoDecoder::kRanOutOfStreamData:
        decoder_current_bitstream_buffer_.reset();
        if (!TrySetNewBistreamBuffer())
          return;

        break;

      case AcceleratedVideoDecoder::kRanOutOfSurfaces:
        // No more surfaces for the decoder, we'll come back once we have more.
        DVLOGF(4) << "Ran out of surfaces";
        return;

      case AcceleratedVideoDecoder::kDecodeError:
        DVLOGF(1) << "Error decoding stream";
        NOTIFY_ERROR(PLATFORM_FAILURE);
        return;
    }
  }
}

void V4L2SliceVideoDecodeAccelerator::InitiateSurfaceSetChange() {
  DVLOGF(2);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  DCHECK_EQ(state_, kDecoding);
  state_ = kIdle;

  DCHECK(!surface_set_change_pending_);
  surface_set_change_pending_ = true;

  FinishSurfaceSetChangeIfNeeded();
}

void V4L2SliceVideoDecodeAccelerator::FinishSurfaceSetChangeIfNeeded() {
  DVLOGF(2);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  if (!surface_set_change_pending_ || !surfaces_at_device_.empty())
    return;

  DCHECK_EQ(state_, kIdle);
  DCHECK(decoder_display_queue_.empty());
  // All output buffers should've been returned from decoder and device by now.
  // The only remaining owner of surfaces may be display (client), and we will
  // dismiss them when destroying output buffers below.
  DCHECK_EQ(free_output_buffers_.size() + surfaces_at_display_.size(),
            output_buffer_map_.size());

  // Keep input queue running while we switch outputs.
  if (!StopDevicePoll(true)) {
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  // This will return only once all buffers are dismissed and destroyed.
  // This does not wait until they are displayed however, as display retains
  // references to the buffers bound to textures and will release them
  // after displaying.
  if (!DestroyOutputs(true)) {
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  if (!CreateOutputBuffers()) {
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  if (!StartDevicePoll()) {
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  DVLOGF(3) << "Surface set change finished";

  surface_set_change_pending_ = false;
  state_ = kDecoding;
  ScheduleDecodeBufferTaskIfNeeded();
}

bool V4L2SliceVideoDecodeAccelerator::DestroyOutputs(bool dismiss) {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());
  std::vector<EGLImageKHR> egl_images_to_destroy;
  std::vector<int32_t> picture_buffers_to_dismiss;

  if (output_buffer_map_.empty())
    return true;

  for (auto output_record : output_buffer_map_) {
    DCHECK(!output_record.at_device);

    if (output_record.egl_sync != EGL_NO_SYNC_KHR) {
      if (eglDestroySyncKHR(egl_display_, output_record.egl_sync) != EGL_TRUE)
        DVLOGF(1) << "eglDestroySyncKHR failed.";
    }

    if (output_record.egl_image != EGL_NO_IMAGE_KHR) {
      child_task_runner_->PostTask(
          FROM_HERE,
          base::Bind(base::IgnoreResult(&V4L2Device::DestroyEGLImage), device_,
                     egl_display_, output_record.egl_image));
    }

    picture_buffers_to_dismiss.push_back(output_record.picture_id);
  }

  if (dismiss) {
    DVLOGF(2) << "Scheduling picture dismissal";
    base::WaitableEvent done(false, false);
    child_task_runner_->PostTask(
        FROM_HERE, base::Bind(&V4L2SliceVideoDecodeAccelerator::DismissPictures,
                              weak_this_, picture_buffers_to_dismiss, &done));
    done.Wait();
  }

  // At this point client can't call ReusePictureBuffer on any of the pictures
  // anymore, so it's safe to destroy.
  return DestroyOutputBuffers();
}

bool V4L2SliceVideoDecodeAccelerator::DestroyOutputBuffers() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread() ||
         !decoder_thread_.IsRunning());
  DCHECK(!output_streamon_);
  DCHECK(surfaces_at_device_.empty());
  DCHECK(decoder_display_queue_.empty());
  DCHECK_EQ(surfaces_at_display_.size() + free_output_buffers_.size(),
            output_buffer_map_.size());

  if (output_buffer_map_.empty())
    return true;

  // It's ok to do this, client will retain references to textures, but we are
  // not interested in reusing the surfaces anymore.
  // This will prevent us from reusing old surfaces in case we have some
  // ReusePictureBuffer() pending on ChildThread already. It's ok to ignore
  // them, because we have already dismissed them (in DestroyOutputs()).
  for (const auto& surface_at_display : surfaces_at_display_) {
    size_t index = surface_at_display.second->output_record();
    DCHECK_LT(index, output_buffer_map_.size());
    OutputRecord& output_record = output_buffer_map_[index];
    DCHECK(output_record.at_client);
    output_record.at_client = false;
  }
  surfaces_at_display_.clear();
  DCHECK_EQ(free_output_buffers_.size(), output_buffer_map_.size());

  free_output_buffers_.clear();
  output_buffer_map_.clear();

  struct v4l2_requestbuffers reqbufs;
  memset(&reqbufs, 0, sizeof(reqbufs));
  reqbufs.count = 0;
  reqbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  reqbufs.memory = V4L2_MEMORY_MMAP;
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_REQBUFS, &reqbufs);

  return true;
}

void V4L2SliceVideoDecodeAccelerator::AssignPictureBuffers(
    const std::vector<media::PictureBuffer>& buffers) {
  DVLOGF(3);
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  const uint32_t req_buffer_count = decoder_->GetRequiredNumOfPictures();

  if (buffers.size() < req_buffer_count) {
    DLOG(ERROR) << "Failed to provide requested picture buffers. "
                << "(Got " << buffers.size()
                << ", requested " << req_buffer_count << ")";
    NOTIFY_ERROR(INVALID_ARGUMENT);
    return;
  }

  if (!make_context_current_.Run()) {
    DLOG(ERROR) << "could not make context current";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  gfx::ScopedTextureBinder bind_restore(GL_TEXTURE_EXTERNAL_OES, 0);

  // It's safe to manipulate all the buffer state here, because the decoder
  // thread is waiting on pictures_assigned_.

  // Allocate the output buffers.
  struct v4l2_requestbuffers reqbufs;
  memset(&reqbufs, 0, sizeof(reqbufs));
  reqbufs.count = buffers.size();
  reqbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  reqbufs.memory = V4L2_MEMORY_MMAP;
  IOCTL_OR_ERROR_RETURN(VIDIOC_REQBUFS, &reqbufs);

  if (reqbufs.count != buffers.size()) {
    DLOG(ERROR) << "Could not allocate enough output buffers";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  output_buffer_map_.resize(buffers.size());

  DCHECK(free_output_buffers_.empty());
  for (size_t i = 0; i < output_buffer_map_.size(); ++i) {
    DCHECK(buffers[i].size() == coded_size_);

    OutputRecord& output_record = output_buffer_map_[i];
    DCHECK(!output_record.at_device);
    DCHECK(!output_record.at_client);
    DCHECK_EQ(output_record.egl_image, EGL_NO_IMAGE_KHR);
    DCHECK_EQ(output_record.egl_sync, EGL_NO_SYNC_KHR);
    DCHECK_EQ(output_record.picture_id, -1);
    DCHECK_EQ(output_record.cleared, false);

    EGLImageKHR egl_image = device_->CreateEGLImage(
        egl_display_, egl_context_, buffers[i].texture_id(),
        buffers[i].size(), i, output_format_fourcc_, output_planes_count_);
    if (egl_image == EGL_NO_IMAGE_KHR) {
      LOGF(ERROR) << "Could not create EGLImageKHR";
      // Ownership of EGLImages allocated in previous iterations of this loop
      // has been transferred to output_buffer_map_. After we error-out here
      // the destructor will handle their cleanup.
      NOTIFY_ERROR(PLATFORM_FAILURE);
      return;
    }

    output_record.egl_image = egl_image;
    output_record.picture_id = buffers[i].id();
    free_output_buffers_.push_back(i);
    DVLOGF(3) << "buffer[" << i << "]: picture_id=" << output_record.picture_id;
  }

  pictures_assigned_.Signal();
}

void V4L2SliceVideoDecodeAccelerator::ReusePictureBuffer(
    int32_t picture_buffer_id) {
  DCHECK(child_task_runner_->BelongsToCurrentThread());
  DVLOGF(4) << "picture_buffer_id=" << picture_buffer_id;

  if (!make_context_current_.Run()) {
    LOGF(ERROR) << "could not make context current";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  EGLSyncKHR egl_sync =
      eglCreateSyncKHR(egl_display_, EGL_SYNC_FENCE_KHR, NULL);
  if (egl_sync == EGL_NO_SYNC_KHR) {
    LOGF(ERROR) << "eglCreateSyncKHR() failed";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  scoped_ptr<EGLSyncKHRRef> egl_sync_ref(
      new EGLSyncKHRRef(egl_display_, egl_sync));
  decoder_thread_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&V4L2SliceVideoDecodeAccelerator::ReusePictureBufferTask,
                 base::Unretained(this), picture_buffer_id,
                 base::Passed(&egl_sync_ref)));
}

void V4L2SliceVideoDecodeAccelerator::ReusePictureBufferTask(
    int32_t picture_buffer_id,
    scoped_ptr<EGLSyncKHRRef> egl_sync_ref) {
  DVLOGF(3) << "picture_buffer_id=" << picture_buffer_id;
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  V4L2DecodeSurfaceByPictureBufferId::iterator it =
      surfaces_at_display_.find(picture_buffer_id);
  if (it == surfaces_at_display_.end()) {
    // It's possible that we've already posted a DismissPictureBuffer for this
    // picture, but it has not yet executed when this ReusePictureBuffer was
    // posted to us by the client. In that case just ignore this (we've already
    // dismissed it and accounted for that) and let the sync object get
    // destroyed.
    DVLOGF(3) << "got picture id=" << picture_buffer_id
              << " not in use (anymore?).";
    return;
  }

  OutputRecord& output_record = output_buffer_map_[it->second->output_record()];
  if (output_record.at_device || !output_record.at_client) {
    DVLOGF(1) << "picture_buffer_id not reusable";
    NOTIFY_ERROR(INVALID_ARGUMENT);
    return;
  }

  DCHECK_EQ(output_record.egl_sync, EGL_NO_SYNC_KHR);
  DCHECK(!output_record.at_device);
  output_record.at_client = false;
  output_record.egl_sync = egl_sync_ref->egl_sync;
  // Take ownership of the EGLSync.
  egl_sync_ref->egl_sync = EGL_NO_SYNC_KHR;
  surfaces_at_display_.erase(it);
}

void V4L2SliceVideoDecodeAccelerator::Flush() {
  DVLOGF(3);
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  decoder_thread_task_runner_->PostTask(
      FROM_HERE, base::Bind(&V4L2SliceVideoDecodeAccelerator::FlushTask,
                            base::Unretained(this)));
}

void V4L2SliceVideoDecodeAccelerator::FlushTask() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  if (!decoder_input_queue_.empty()) {
    // We are not done with pending inputs, so queue an empty buffer,
    // which - when reached - will trigger flush sequence.
    decoder_input_queue_.push(
        linked_ptr<BitstreamBufferRef>(new BitstreamBufferRef(
            io_client_, io_task_runner_, nullptr, 0, kFlushBufferId)));
    return;
  }

  // No more inputs pending, so just finish flushing here.
  InitiateFlush();
}

void V4L2SliceVideoDecodeAccelerator::InitiateFlush() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  DCHECK(!decoder_flushing_);
  DCHECK_EQ(state_, kDecoding);
  state_ = kIdle;

  // This will trigger output for all remaining surfaces in the decoder.
  // However, not all of them may be decoded yet (they would be queued
  // in hardware then).
  if (!decoder_->Flush()) {
    DVLOGF(1) << "Failed flushing the decoder.";
    NOTIFY_ERROR(PLATFORM_FAILURE);
    return;
  }

  // Put the decoder in an idle state, ready to resume.
  decoder_->Reset();

  decoder_flushing_ = true;

  decoder_thread_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&V4L2SliceVideoDecodeAccelerator::FinishFlushIfNeeded,
                 base::Unretained(this)));
}

void V4L2SliceVideoDecodeAccelerator::FinishFlushIfNeeded() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  if (!decoder_flushing_ || !surfaces_at_device_.empty())
    return;

  DCHECK_EQ(state_, kIdle);

  // At this point, all remaining surfaces are decoded and dequeued, and since
  // we have already scheduled output for them in InitiateFlush(), their
  // respective PictureReady calls have been posted (or they have been queued on
  // pending_picture_ready_). So at this time, once we SendPictureReady(),
  // we will have all remaining PictureReady() posted to the client and we
  // can post NotifyFlushDone().
  DCHECK(decoder_display_queue_.empty());

  // Decoder should have already returned all surfaces and all surfaces are
  // out of hardware. There can be no other owners of input buffers.
  DCHECK_EQ(free_input_buffers_.size(), input_buffer_map_.size());

  SendPictureReady();

  child_task_runner_->PostTask(FROM_HERE,
                               base::Bind(&Client::NotifyFlushDone, client_));

  decoder_flushing_ = false;

  DVLOGF(3) << "Flush finished";
  state_ = kDecoding;
  ScheduleDecodeBufferTaskIfNeeded();
}

void V4L2SliceVideoDecodeAccelerator::Reset() {
  DVLOGF(3);
  DCHECK(child_task_runner_->BelongsToCurrentThread());

  decoder_thread_task_runner_->PostTask(
      FROM_HERE, base::Bind(&V4L2SliceVideoDecodeAccelerator::ResetTask,
                            base::Unretained(this)));
}

void V4L2SliceVideoDecodeAccelerator::ResetTask() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  if (decoder_resetting_) {
    // This is a bug in the client, multiple Reset()s before NotifyResetDone()
    // are not allowed.
    NOTREACHED() << "Client should not be requesting multiple Reset()s";
    return;
  }

  DCHECK_EQ(state_, kDecoding);
  state_ = kIdle;

  // Put the decoder in an idle state, ready to resume.
  decoder_->Reset();

  decoder_resetting_ = true;

  // Drop all remaining inputs.
  decoder_current_bitstream_buffer_.reset();
  while (!decoder_input_queue_.empty())
    decoder_input_queue_.pop();

  FinishResetIfNeeded();
}

void V4L2SliceVideoDecodeAccelerator::FinishResetIfNeeded() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  if (!decoder_resetting_ || !surfaces_at_device_.empty())
    return;

  DCHECK_EQ(state_, kIdle);
  DCHECK(!decoder_flushing_);
  SendPictureReady();

  // Drop any pending outputs.
  while (!decoder_display_queue_.empty())
    decoder_display_queue_.pop();

  // At this point we can have no input buffers in the decoder, because we
  // Reset()ed it in ResetTask(), and have not scheduled any new Decode()s
  // having been in kIdle since. We don't have any surfaces in the HW either -
  // we just checked that surfaces_at_device_.empty(), and inputs are tied
  // to surfaces. Since there can be no other owners of input buffers, we can
  // simply mark them all as available.
  DCHECK_EQ(input_buffer_queued_count_, 0);
  free_input_buffers_.clear();
  for (size_t i = 0; i < input_buffer_map_.size(); ++i) {
    DCHECK(!input_buffer_map_[i].at_device);
    ReuseInputBuffer(i);
  }

  decoder_resetting_ = false;

  child_task_runner_->PostTask(FROM_HERE,
                               base::Bind(&Client::NotifyResetDone, client_));

  DVLOGF(3) << "Reset finished";

  state_ = kDecoding;
  ScheduleDecodeBufferTaskIfNeeded();
}

void V4L2SliceVideoDecodeAccelerator::SetErrorState(Error error) {
  // We can touch decoder_state_ only if this is the decoder thread or the
  // decoder thread isn't running.
  if (decoder_thread_.IsRunning() &&
      !decoder_thread_task_runner_->BelongsToCurrentThread()) {
    decoder_thread_task_runner_->PostTask(
        FROM_HERE, base::Bind(&V4L2SliceVideoDecodeAccelerator::SetErrorState,
                              base::Unretained(this), error));
    return;
  }

  // Post NotifyError only if we are already initialized, as the API does
  // not allow doing so before that.
  if (state_ != kError && state_ != kUninitialized)
    NotifyError(error);

  state_ = kError;
}

V4L2SliceVideoDecodeAccelerator::V4L2H264Accelerator::V4L2H264Accelerator(
    V4L2SliceVideoDecodeAccelerator* v4l2_dec)
    : num_slices_(0), v4l2_dec_(v4l2_dec) {
  DCHECK(v4l2_dec_);
}

V4L2SliceVideoDecodeAccelerator::V4L2H264Accelerator::~V4L2H264Accelerator() {
}

scoped_refptr<H264Picture>
V4L2SliceVideoDecodeAccelerator::V4L2H264Accelerator::CreateH264Picture() {
  scoped_refptr<V4L2DecodeSurface> dec_surface = v4l2_dec_->CreateSurface();
  if (!dec_surface)
    return nullptr;

  return new V4L2H264Picture(dec_surface);
}

void V4L2SliceVideoDecodeAccelerator::V4L2H264Accelerator::
    H264PictureListToDPBIndicesList(const H264Picture::Vector& src_pic_list,
                                    uint8_t dst_list[kDPBIndicesListSize]) {
  size_t i;
  for (i = 0; i < src_pic_list.size() && i < kDPBIndicesListSize; ++i) {
    const scoped_refptr<H264Picture>& pic = src_pic_list[i];
    dst_list[i] = pic ? pic->dpb_position : VIDEO_MAX_FRAME;
  }

  while (i < kDPBIndicesListSize)
    dst_list[i++] = VIDEO_MAX_FRAME;
}

void V4L2SliceVideoDecodeAccelerator::V4L2H264Accelerator::H264DPBToV4L2DPB(
    const H264DPB& dpb,
    std::vector<scoped_refptr<V4L2DecodeSurface>>* ref_surfaces) {
  memset(v4l2_decode_param_.dpb, 0, sizeof(v4l2_decode_param_.dpb));
  size_t i = 0;
  for (const auto& pic : dpb) {
    if (i >= arraysize(v4l2_decode_param_.dpb)) {
      DVLOG(1) << "Invalid DPB size";
      break;
    }

    int index = VIDEO_MAX_FRAME;
    if (!pic->nonexisting) {
      scoped_refptr<V4L2DecodeSurface> dec_surface =
          H264PictureToV4L2DecodeSurface(pic);
      index = dec_surface->output_record();
      ref_surfaces->push_back(dec_surface);
    }

    struct v4l2_h264_dpb_entry& entry = v4l2_decode_param_.dpb[i++];
    entry.buf_index = index;
    entry.frame_num = pic->frame_num;
    entry.pic_num = pic->pic_num;
    entry.top_field_order_cnt = pic->top_field_order_cnt;
    entry.bottom_field_order_cnt = pic->bottom_field_order_cnt;
    entry.flags = (pic->ref ? V4L2_H264_DPB_ENTRY_FLAG_ACTIVE : 0) |
                  (pic->long_term ? V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM : 0);
  }
}

bool V4L2SliceVideoDecodeAccelerator::V4L2H264Accelerator::SubmitFrameMetadata(
    const media::H264SPS* sps,
    const media::H264PPS* pps,
    const H264DPB& dpb,
    const H264Picture::Vector& ref_pic_listp0,
    const H264Picture::Vector& ref_pic_listb0,
    const H264Picture::Vector& ref_pic_listb1,
    const scoped_refptr<H264Picture>& pic) {
  struct v4l2_ext_control ctrl;
  std::vector<struct v4l2_ext_control> ctrls;

  struct v4l2_ctrl_h264_sps v4l2_sps;
  memset(&v4l2_sps, 0, sizeof(v4l2_sps));
  v4l2_sps.constraint_set_flags =
    sps->constraint_set0_flag ? V4L2_H264_SPS_CONSTRAINT_SET0_FLAG : 0 |
    sps->constraint_set1_flag ? V4L2_H264_SPS_CONSTRAINT_SET1_FLAG : 0 |
    sps->constraint_set2_flag ? V4L2_H264_SPS_CONSTRAINT_SET2_FLAG : 0 |
    sps->constraint_set3_flag ? V4L2_H264_SPS_CONSTRAINT_SET3_FLAG : 0 |
    sps->constraint_set4_flag ? V4L2_H264_SPS_CONSTRAINT_SET4_FLAG : 0 |
    sps->constraint_set5_flag ? V4L2_H264_SPS_CONSTRAINT_SET5_FLAG : 0;
#define SPS_TO_V4L2SPS(a) v4l2_sps.a = sps->a
  SPS_TO_V4L2SPS(profile_idc);
  SPS_TO_V4L2SPS(level_idc);
  SPS_TO_V4L2SPS(seq_parameter_set_id);
  SPS_TO_V4L2SPS(chroma_format_idc);
  SPS_TO_V4L2SPS(bit_depth_luma_minus8);
  SPS_TO_V4L2SPS(bit_depth_chroma_minus8);
  SPS_TO_V4L2SPS(log2_max_frame_num_minus4);
  SPS_TO_V4L2SPS(pic_order_cnt_type);
  SPS_TO_V4L2SPS(log2_max_pic_order_cnt_lsb_minus4);
  SPS_TO_V4L2SPS(offset_for_non_ref_pic);
  SPS_TO_V4L2SPS(offset_for_top_to_bottom_field);
  SPS_TO_V4L2SPS(num_ref_frames_in_pic_order_cnt_cycle);

  static_assert(arraysize(v4l2_sps.offset_for_ref_frame) ==
                arraysize(sps->offset_for_ref_frame),
                "offset_for_ref_frame arrays must be same size");
  for (size_t i = 0; i < arraysize(v4l2_sps.offset_for_ref_frame); ++i)
    v4l2_sps.offset_for_ref_frame[i] = sps->offset_for_ref_frame[i];
  SPS_TO_V4L2SPS(max_num_ref_frames);
  SPS_TO_V4L2SPS(pic_width_in_mbs_minus1);
  SPS_TO_V4L2SPS(pic_height_in_map_units_minus1);
#undef SPS_TO_V4L2SPS

#define SET_V4L2_SPS_FLAG_IF(cond, flag) \
  v4l2_sps.flags |= ((sps->cond) ? (flag) : 0)
  SET_V4L2_SPS_FLAG_IF(separate_colour_plane_flag,
                       V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE);
  SET_V4L2_SPS_FLAG_IF(qpprime_y_zero_transform_bypass_flag,
                       V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS);
  SET_V4L2_SPS_FLAG_IF(delta_pic_order_always_zero_flag,
                       V4L2_H264_SPS_FLAG_DELTA_PIC_ORDER_ALWAYS_ZERO);
  SET_V4L2_SPS_FLAG_IF(gaps_in_frame_num_value_allowed_flag,
                       V4L2_H264_SPS_FLAG_GAPS_IN_FRAME_NUM_VALUE_ALLOWED);
  SET_V4L2_SPS_FLAG_IF(frame_mbs_only_flag, V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY);
  SET_V4L2_SPS_FLAG_IF(mb_adaptive_frame_field_flag,
                       V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD);
  SET_V4L2_SPS_FLAG_IF(direct_8x8_inference_flag,
                       V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE);
#undef SET_FLAG
  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = V4L2_CID_MPEG_VIDEO_H264_SPS;
  ctrl.size = sizeof(v4l2_sps);
  ctrl.p_h264_sps = &v4l2_sps;
  ctrls.push_back(ctrl);

  struct v4l2_ctrl_h264_pps v4l2_pps;
  memset(&v4l2_pps, 0, sizeof(v4l2_pps));
#define PPS_TO_V4L2PPS(a) v4l2_pps.a = pps->a
  PPS_TO_V4L2PPS(pic_parameter_set_id);
  PPS_TO_V4L2PPS(seq_parameter_set_id);
  PPS_TO_V4L2PPS(num_slice_groups_minus1);
  PPS_TO_V4L2PPS(num_ref_idx_l0_default_active_minus1);
  PPS_TO_V4L2PPS(num_ref_idx_l1_default_active_minus1);
  PPS_TO_V4L2PPS(weighted_bipred_idc);
  PPS_TO_V4L2PPS(pic_init_qp_minus26);
  PPS_TO_V4L2PPS(pic_init_qs_minus26);
  PPS_TO_V4L2PPS(chroma_qp_index_offset);
  PPS_TO_V4L2PPS(second_chroma_qp_index_offset);
#undef PPS_TO_V4L2PPS

#define SET_V4L2_PPS_FLAG_IF(cond, flag) \
  v4l2_pps.flags |= ((pps->cond) ? (flag) : 0)
  SET_V4L2_PPS_FLAG_IF(entropy_coding_mode_flag,
                       V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE);
  SET_V4L2_PPS_FLAG_IF(
      bottom_field_pic_order_in_frame_present_flag,
      V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT);
  SET_V4L2_PPS_FLAG_IF(weighted_pred_flag, V4L2_H264_PPS_FLAG_WEIGHTED_PRED);
  SET_V4L2_PPS_FLAG_IF(deblocking_filter_control_present_flag,
                       V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT);
  SET_V4L2_PPS_FLAG_IF(constrained_intra_pred_flag,
                       V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED);
  SET_V4L2_PPS_FLAG_IF(redundant_pic_cnt_present_flag,
                       V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT);
  SET_V4L2_PPS_FLAG_IF(transform_8x8_mode_flag,
                       V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE);
  SET_V4L2_PPS_FLAG_IF(pic_scaling_matrix_present_flag,
                       V4L2_H264_PPS_FLAG_PIC_SCALING_MATRIX_PRESENT);
#undef SET_V4L2_PPS_FLAG_IF
  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = V4L2_CID_MPEG_VIDEO_H264_PPS;
  ctrl.size = sizeof(v4l2_pps);
  ctrl.p_h264_pps = &v4l2_pps;
  ctrls.push_back(ctrl);

  struct v4l2_ctrl_h264_scaling_matrix v4l2_scaling_matrix;
  memset(&v4l2_scaling_matrix, 0, sizeof(v4l2_scaling_matrix));
  static_assert(arraysize(v4l2_scaling_matrix.scaling_list_4x4) <=
                arraysize(pps->scaling_list4x4) &&
                arraysize(v4l2_scaling_matrix.scaling_list_4x4[0]) <=
                arraysize(pps->scaling_list4x4[0]) &&
                arraysize(v4l2_scaling_matrix.scaling_list_8x8) <=
                arraysize(pps->scaling_list8x8) &&
                arraysize(v4l2_scaling_matrix.scaling_list_8x8[0]) <=
                arraysize(pps->scaling_list8x8[0]),
                "scaling_lists must be of correct size");
  for (size_t i = 0; i < arraysize(v4l2_scaling_matrix.scaling_list_4x4); ++i) {
    for (size_t j = 0; j < arraysize(v4l2_scaling_matrix.scaling_list_4x4[i]);
         ++j) {
      v4l2_scaling_matrix.scaling_list_4x4[i][j] = pps->scaling_list4x4[i][j];
    }
  }
  for (size_t i = 0; i < arraysize(v4l2_scaling_matrix.scaling_list_8x8); ++i) {
    for (size_t j = 0; j < arraysize(v4l2_scaling_matrix.scaling_list_8x8[i]);
         ++j) {
      v4l2_scaling_matrix.scaling_list_8x8[i][j] = pps->scaling_list8x8[i][j];
    }
  }
  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = V4L2_CID_MPEG_VIDEO_H264_SCALING_MATRIX;
  ctrl.size = sizeof(v4l2_scaling_matrix);
  ctrl.p_h264_scal_mtrx = &v4l2_scaling_matrix;
  ctrls.push_back(ctrl);

  scoped_refptr<V4L2DecodeSurface> dec_surface =
      H264PictureToV4L2DecodeSurface(pic);

  struct v4l2_ext_controls ext_ctrls;
  memset(&ext_ctrls, 0, sizeof(ext_ctrls));
  ext_ctrls.count = ctrls.size();
  ext_ctrls.controls = &ctrls[0];
  ext_ctrls.config_store = dec_surface->config_store();
  v4l2_dec_->SubmitExtControls(&ext_ctrls);

  H264PictureListToDPBIndicesList(ref_pic_listp0,
                                  v4l2_decode_param_.ref_pic_list_p0);
  H264PictureListToDPBIndicesList(ref_pic_listb0,
                                  v4l2_decode_param_.ref_pic_list_b0);
  H264PictureListToDPBIndicesList(ref_pic_listb1,
                                  v4l2_decode_param_.ref_pic_list_b1);

  std::vector<scoped_refptr<V4L2DecodeSurface>> ref_surfaces;
  H264DPBToV4L2DPB(dpb, &ref_surfaces);
  dec_surface->SetReferenceSurfaces(ref_surfaces);

  return true;
}

bool V4L2SliceVideoDecodeAccelerator::V4L2H264Accelerator::SubmitSlice(
    const media::H264PPS* pps,
    const media::H264SliceHeader* slice_hdr,
    const H264Picture::Vector& ref_pic_list0,
    const H264Picture::Vector& ref_pic_list1,
    const scoped_refptr<H264Picture>& pic,
    const uint8_t* data,
    size_t size) {
  if (num_slices_ == kMaxSlices) {
    LOGF(ERROR) << "Over limit of supported slices per frame";
    return false;
  }

  struct v4l2_ctrl_h264_slice_param& v4l2_slice_param =
      v4l2_slice_params_[num_slices_++];
  memset(&v4l2_slice_param, 0, sizeof(v4l2_slice_param));

  v4l2_slice_param.size = size;
#define SHDR_TO_V4L2SPARM(a) v4l2_slice_param.a = slice_hdr->a
  SHDR_TO_V4L2SPARM(header_bit_size);
  SHDR_TO_V4L2SPARM(first_mb_in_slice);
  SHDR_TO_V4L2SPARM(slice_type);
  SHDR_TO_V4L2SPARM(pic_parameter_set_id);
  SHDR_TO_V4L2SPARM(colour_plane_id);
  SHDR_TO_V4L2SPARM(frame_num);
  SHDR_TO_V4L2SPARM(idr_pic_id);
  SHDR_TO_V4L2SPARM(pic_order_cnt_lsb);
  SHDR_TO_V4L2SPARM(delta_pic_order_cnt_bottom);
  SHDR_TO_V4L2SPARM(delta_pic_order_cnt0);
  SHDR_TO_V4L2SPARM(delta_pic_order_cnt1);
  SHDR_TO_V4L2SPARM(redundant_pic_cnt);
  SHDR_TO_V4L2SPARM(dec_ref_pic_marking_bit_size);
  SHDR_TO_V4L2SPARM(cabac_init_idc);
  SHDR_TO_V4L2SPARM(slice_qp_delta);
  SHDR_TO_V4L2SPARM(slice_qs_delta);
  SHDR_TO_V4L2SPARM(disable_deblocking_filter_idc);
  SHDR_TO_V4L2SPARM(slice_alpha_c0_offset_div2);
  SHDR_TO_V4L2SPARM(slice_beta_offset_div2);
  SHDR_TO_V4L2SPARM(num_ref_idx_l0_active_minus1);
  SHDR_TO_V4L2SPARM(num_ref_idx_l1_active_minus1);
  SHDR_TO_V4L2SPARM(pic_order_cnt_bit_size);
#undef SHDR_TO_V4L2SPARM

#define SET_V4L2_SPARM_FLAG_IF(cond, flag) \
  v4l2_slice_param.flags |= ((slice_hdr->cond) ? (flag) : 0)
  SET_V4L2_SPARM_FLAG_IF(field_pic_flag, V4L2_SLICE_FLAG_FIELD_PIC);
  SET_V4L2_SPARM_FLAG_IF(bottom_field_flag, V4L2_SLICE_FLAG_BOTTOM_FIELD);
  SET_V4L2_SPARM_FLAG_IF(direct_spatial_mv_pred_flag,
                         V4L2_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED);
  SET_V4L2_SPARM_FLAG_IF(sp_for_switch_flag, V4L2_SLICE_FLAG_SP_FOR_SWITCH);
#undef SET_V4L2_SPARM_FLAG_IF

  struct v4l2_h264_pred_weight_table* pred_weight_table =
      &v4l2_slice_param.pred_weight_table;

  if (((slice_hdr->IsPSlice() || slice_hdr->IsSPSlice()) &&
       pps->weighted_pred_flag) ||
      (slice_hdr->IsBSlice() && pps->weighted_bipred_idc == 1)) {
    pred_weight_table->luma_log2_weight_denom =
        slice_hdr->luma_log2_weight_denom;
    pred_weight_table->chroma_log2_weight_denom =
        slice_hdr->chroma_log2_weight_denom;

    struct v4l2_h264_weight_factors* factorsl0 =
        &pred_weight_table->weight_factors[0];

    for (int i = 0; i < 32; ++i) {
      factorsl0->luma_weight[i] =
          slice_hdr->pred_weight_table_l0.luma_weight[i];
      factorsl0->luma_offset[i] =
          slice_hdr->pred_weight_table_l0.luma_offset[i];

      for (int j = 0; j < 2; ++j) {
        factorsl0->chroma_weight[i][j] =
            slice_hdr->pred_weight_table_l0.chroma_weight[i][j];
        factorsl0->chroma_offset[i][j] =
            slice_hdr->pred_weight_table_l0.chroma_offset[i][j];
      }
    }

    if (slice_hdr->IsBSlice()) {
      struct v4l2_h264_weight_factors* factorsl1 =
          &pred_weight_table->weight_factors[1];

      for (int i = 0; i < 32; ++i) {
        factorsl1->luma_weight[i] =
            slice_hdr->pred_weight_table_l1.luma_weight[i];
        factorsl1->luma_offset[i] =
            slice_hdr->pred_weight_table_l1.luma_offset[i];

        for (int j = 0; j < 2; ++j) {
          factorsl1->chroma_weight[i][j] =
              slice_hdr->pred_weight_table_l1.chroma_weight[i][j];
          factorsl1->chroma_offset[i][j] =
              slice_hdr->pred_weight_table_l1.chroma_offset[i][j];
        }
      }
    }
  }

  H264PictureListToDPBIndicesList(ref_pic_list0,
                                  v4l2_slice_param.ref_pic_list0);
  H264PictureListToDPBIndicesList(ref_pic_list1,
                                  v4l2_slice_param.ref_pic_list1);

  scoped_refptr<V4L2DecodeSurface> dec_surface =
      H264PictureToV4L2DecodeSurface(pic);

  v4l2_decode_param_.nal_ref_idc = slice_hdr->nal_ref_idc;

  // TODO(posciak): Don't add start code back here, but have it passed from
  // the parser.
  size_t data_copy_size = size + 3;
  scoped_ptr<uint8_t[]> data_copy(new uint8_t[data_copy_size]);
  memset(data_copy.get(), 0, data_copy_size);
  data_copy[2] = 0x01;
  memcpy(data_copy.get() + 3, data, size);
  return v4l2_dec_->SubmitSlice(dec_surface->input_record(), data_copy.get(),
                                data_copy_size);
}

bool V4L2SliceVideoDecodeAccelerator::SubmitSlice(int index,
                                                  const uint8_t* data,
                                                  size_t size) {
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  InputRecord& input_record = input_buffer_map_[index];

  if (input_record.bytes_used + size > input_record.length) {
    DVLOGF(1) << "Input buffer too small";
    return false;
  }

  memcpy(static_cast<uint8_t*>(input_record.address) + input_record.bytes_used,
         data, size);
  input_record.bytes_used += size;

  return true;
}

bool V4L2SliceVideoDecodeAccelerator::SubmitExtControls(
    struct v4l2_ext_controls* ext_ctrls) {
  DCHECK_GT(ext_ctrls->config_store, 0u);
  IOCTL_OR_ERROR_RETURN_FALSE(VIDIOC_S_EXT_CTRLS, ext_ctrls);
  return true;
}

bool V4L2SliceVideoDecodeAccelerator::V4L2H264Accelerator::SubmitDecode(
    const scoped_refptr<H264Picture>& pic) {
  scoped_refptr<V4L2DecodeSurface> dec_surface =
      H264PictureToV4L2DecodeSurface(pic);

  v4l2_decode_param_.num_slices = num_slices_;
  v4l2_decode_param_.idr_pic_flag = pic->idr;
  v4l2_decode_param_.top_field_order_cnt = pic->top_field_order_cnt;
  v4l2_decode_param_.bottom_field_order_cnt = pic->bottom_field_order_cnt;

  struct v4l2_ext_control ctrl;
  std::vector<struct v4l2_ext_control> ctrls;

  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = V4L2_CID_MPEG_VIDEO_H264_SLICE_PARAM;
  ctrl.size = sizeof(v4l2_slice_params_);
  ctrl.p_h264_slice_param = v4l2_slice_params_;
  ctrls.push_back(ctrl);

  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = V4L2_CID_MPEG_VIDEO_H264_DECODE_PARAM;
  ctrl.size = sizeof(v4l2_decode_param_);
  ctrl.p_h264_decode_param = &v4l2_decode_param_;
  ctrls.push_back(ctrl);

  struct v4l2_ext_controls ext_ctrls;
  memset(&ext_ctrls, 0, sizeof(ext_ctrls));
  ext_ctrls.count = ctrls.size();
  ext_ctrls.controls = &ctrls[0];
  ext_ctrls.config_store = dec_surface->config_store();
  v4l2_dec_->SubmitExtControls(&ext_ctrls);

  Reset();

  v4l2_dec_->DecodeSurface(dec_surface);
  return true;
}

bool V4L2SliceVideoDecodeAccelerator::V4L2H264Accelerator::OutputPicture(
    const scoped_refptr<H264Picture>& pic) {
  scoped_refptr<V4L2DecodeSurface> dec_surface =
      H264PictureToV4L2DecodeSurface(pic);
  v4l2_dec_->SurfaceReady(dec_surface);
  return true;
}

void V4L2SliceVideoDecodeAccelerator::V4L2H264Accelerator::Reset() {
  num_slices_ = 0;
  memset(&v4l2_decode_param_, 0, sizeof(v4l2_decode_param_));
  memset(&v4l2_slice_params_, 0, sizeof(v4l2_slice_params_));
}

scoped_refptr<V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface>
V4L2SliceVideoDecodeAccelerator::V4L2H264Accelerator::
    H264PictureToV4L2DecodeSurface(const scoped_refptr<H264Picture>& pic) {
  V4L2H264Picture* v4l2_pic = pic->AsV4L2H264Picture();
  CHECK(v4l2_pic);
  return v4l2_pic->dec_surface();
}

V4L2SliceVideoDecodeAccelerator::V4L2VP8Accelerator::V4L2VP8Accelerator(
    V4L2SliceVideoDecodeAccelerator* v4l2_dec)
    : v4l2_dec_(v4l2_dec) {
  DCHECK(v4l2_dec_);
}

V4L2SliceVideoDecodeAccelerator::V4L2VP8Accelerator::~V4L2VP8Accelerator() {
}

scoped_refptr<VP8Picture>
V4L2SliceVideoDecodeAccelerator::V4L2VP8Accelerator::CreateVP8Picture() {
  scoped_refptr<V4L2DecodeSurface> dec_surface = v4l2_dec_->CreateSurface();
  if (!dec_surface)
    return nullptr;

  return new V4L2VP8Picture(dec_surface);
}

#define ARRAY_MEMCPY_CHECKED(to, from)                               \
  do {                                                               \
    static_assert(sizeof(to) == sizeof(from),                        \
                  #from " and " #to " arrays must be of same size"); \
    memcpy(to, from, sizeof(to));                                    \
  } while (0)

static void FillV4L2SegmentationHeader(
    const media::Vp8SegmentationHeader& vp8_sgmnt_hdr,
    struct v4l2_vp8_sgmnt_hdr* v4l2_sgmnt_hdr) {
#define SET_V4L2_SGMNT_HDR_FLAG_IF(cond, flag) \
  v4l2_sgmnt_hdr->flags |= ((vp8_sgmnt_hdr.cond) ? (flag) : 0)
  SET_V4L2_SGMNT_HDR_FLAG_IF(segmentation_enabled,
                             V4L2_VP8_SEGMNT_HDR_FLAG_ENABLED);
  SET_V4L2_SGMNT_HDR_FLAG_IF(update_mb_segmentation_map,
                             V4L2_VP8_SEGMNT_HDR_FLAG_UPDATE_MAP);
  SET_V4L2_SGMNT_HDR_FLAG_IF(update_segment_feature_data,
                             V4L2_VP8_SEGMNT_HDR_FLAG_UPDATE_FEATURE_DATA);
#undef SET_V4L2_SPARM_FLAG_IF
  v4l2_sgmnt_hdr->segment_feature_mode = vp8_sgmnt_hdr.segment_feature_mode;

  ARRAY_MEMCPY_CHECKED(v4l2_sgmnt_hdr->quant_update,
                       vp8_sgmnt_hdr.quantizer_update_value);
  ARRAY_MEMCPY_CHECKED(v4l2_sgmnt_hdr->lf_update,
                       vp8_sgmnt_hdr.lf_update_value);
  ARRAY_MEMCPY_CHECKED(v4l2_sgmnt_hdr->segment_probs,
                       vp8_sgmnt_hdr.segment_prob);
}

static void FillV4L2LoopfilterHeader(
    const media::Vp8LoopFilterHeader& vp8_loopfilter_hdr,
    struct v4l2_vp8_loopfilter_hdr* v4l2_lf_hdr) {
#define SET_V4L2_LF_HDR_FLAG_IF(cond, flag) \
  v4l2_lf_hdr->flags |= ((vp8_loopfilter_hdr.cond) ? (flag) : 0)
  SET_V4L2_LF_HDR_FLAG_IF(loop_filter_adj_enable, V4L2_VP8_LF_HDR_ADJ_ENABLE);
  SET_V4L2_LF_HDR_FLAG_IF(mode_ref_lf_delta_update,
                          V4L2_VP8_LF_HDR_DELTA_UPDATE);
#undef SET_V4L2_SGMNT_HDR_FLAG_IF

#define LF_HDR_TO_V4L2_LF_HDR(a) v4l2_lf_hdr->a = vp8_loopfilter_hdr.a;
  LF_HDR_TO_V4L2_LF_HDR(type);
  LF_HDR_TO_V4L2_LF_HDR(level);
  LF_HDR_TO_V4L2_LF_HDR(sharpness_level);
#undef LF_HDR_TO_V4L2_LF_HDR

  ARRAY_MEMCPY_CHECKED(v4l2_lf_hdr->ref_frm_delta_magnitude,
                       vp8_loopfilter_hdr.ref_frame_delta);
  ARRAY_MEMCPY_CHECKED(v4l2_lf_hdr->mb_mode_delta_magnitude,
                       vp8_loopfilter_hdr.mb_mode_delta);
}

static void FillV4L2QuantizationHeader(
    const media::Vp8QuantizationHeader& vp8_quant_hdr,
    struct v4l2_vp8_quantization_hdr* v4l2_quant_hdr) {
  v4l2_quant_hdr->y_ac_qi = vp8_quant_hdr.y_ac_qi;
  v4l2_quant_hdr->y_dc_delta = vp8_quant_hdr.y_dc_delta;
  v4l2_quant_hdr->y2_dc_delta = vp8_quant_hdr.y2_dc_delta;
  v4l2_quant_hdr->y2_ac_delta = vp8_quant_hdr.y2_ac_delta;
  v4l2_quant_hdr->uv_dc_delta = vp8_quant_hdr.uv_dc_delta;
  v4l2_quant_hdr->uv_ac_delta = vp8_quant_hdr.uv_ac_delta;
}

static void FillV4L2EntropyHeader(
    const media::Vp8EntropyHeader& vp8_entropy_hdr,
    struct v4l2_vp8_entropy_hdr* v4l2_entropy_hdr) {
  ARRAY_MEMCPY_CHECKED(v4l2_entropy_hdr->coeff_probs,
                       vp8_entropy_hdr.coeff_probs);
  ARRAY_MEMCPY_CHECKED(v4l2_entropy_hdr->y_mode_probs,
                       vp8_entropy_hdr.y_mode_probs);
  ARRAY_MEMCPY_CHECKED(v4l2_entropy_hdr->uv_mode_probs,
                       vp8_entropy_hdr.uv_mode_probs);
  ARRAY_MEMCPY_CHECKED(v4l2_entropy_hdr->mv_probs,
                       vp8_entropy_hdr.mv_probs);
}

bool V4L2SliceVideoDecodeAccelerator::V4L2VP8Accelerator::SubmitDecode(
    const scoped_refptr<VP8Picture>& pic,
    const media::Vp8FrameHeader* frame_hdr,
    const scoped_refptr<VP8Picture>& last_frame,
    const scoped_refptr<VP8Picture>& golden_frame,
    const scoped_refptr<VP8Picture>& alt_frame) {
  struct v4l2_ctrl_vp8_frame_hdr v4l2_frame_hdr;
  memset(&v4l2_frame_hdr, 0, sizeof(v4l2_frame_hdr));

#define FHDR_TO_V4L2_FHDR(a) v4l2_frame_hdr.a = frame_hdr->a
  FHDR_TO_V4L2_FHDR(key_frame);
  FHDR_TO_V4L2_FHDR(version);
  FHDR_TO_V4L2_FHDR(width);
  FHDR_TO_V4L2_FHDR(horizontal_scale);
  FHDR_TO_V4L2_FHDR(height);
  FHDR_TO_V4L2_FHDR(vertical_scale);
  FHDR_TO_V4L2_FHDR(sign_bias_golden);
  FHDR_TO_V4L2_FHDR(sign_bias_alternate);
  FHDR_TO_V4L2_FHDR(prob_skip_false);
  FHDR_TO_V4L2_FHDR(prob_intra);
  FHDR_TO_V4L2_FHDR(prob_last);
  FHDR_TO_V4L2_FHDR(prob_gf);
  FHDR_TO_V4L2_FHDR(bool_dec_range);
  FHDR_TO_V4L2_FHDR(bool_dec_value);
  FHDR_TO_V4L2_FHDR(bool_dec_count);
#undef FHDR_TO_V4L2_FHDR

#define SET_V4L2_FRM_HDR_FLAG_IF(cond, flag) \
  v4l2_frame_hdr.flags |= ((frame_hdr->cond) ? (flag) : 0)
  SET_V4L2_FRM_HDR_FLAG_IF(is_experimental,
                           V4L2_VP8_FRAME_HDR_FLAG_EXPERIMENTAL);
  SET_V4L2_FRM_HDR_FLAG_IF(show_frame, V4L2_VP8_FRAME_HDR_FLAG_SHOW_FRAME);
  SET_V4L2_FRM_HDR_FLAG_IF(mb_no_skip_coeff,
                           V4L2_VP8_FRAME_HDR_FLAG_MB_NO_SKIP_COEFF);
#undef SET_V4L2_FRM_HDR_FLAG_IF

  FillV4L2SegmentationHeader(frame_hdr->segmentation_hdr,
                             &v4l2_frame_hdr.sgmnt_hdr);

  FillV4L2LoopfilterHeader(frame_hdr->loopfilter_hdr, &v4l2_frame_hdr.lf_hdr);

  FillV4L2QuantizationHeader(frame_hdr->quantization_hdr,
                             &v4l2_frame_hdr.quant_hdr);

  FillV4L2EntropyHeader(frame_hdr->entropy_hdr, &v4l2_frame_hdr.entropy_hdr);

  v4l2_frame_hdr.first_part_size =
      base::checked_cast<__u32>(frame_hdr->first_part_size);
  v4l2_frame_hdr.first_part_offset =
      base::checked_cast<__u32>(frame_hdr->first_part_offset);
  v4l2_frame_hdr.macroblock_bit_offset =
      base::checked_cast<__u32>(frame_hdr->macroblock_bit_offset);
  v4l2_frame_hdr.num_dct_parts = frame_hdr->num_of_dct_partitions;

  static_assert(arraysize(v4l2_frame_hdr.dct_part_sizes) ==
                arraysize(frame_hdr->dct_partition_sizes),
                "DCT partition size arrays must have equal number of elements");
  for (size_t i = 0; i < frame_hdr->num_of_dct_partitions &&
       i < arraysize(v4l2_frame_hdr.dct_part_sizes); ++i)
    v4l2_frame_hdr.dct_part_sizes[i] = frame_hdr->dct_partition_sizes[i];

  scoped_refptr<V4L2DecodeSurface> dec_surface =
      VP8PictureToV4L2DecodeSurface(pic);
  std::vector<scoped_refptr<V4L2DecodeSurface>> ref_surfaces;

  if (last_frame) {
    scoped_refptr<V4L2DecodeSurface> last_frame_surface =
        VP8PictureToV4L2DecodeSurface(last_frame);
    v4l2_frame_hdr.last_frame = last_frame_surface->output_record();
    ref_surfaces.push_back(last_frame_surface);
  } else {
    v4l2_frame_hdr.last_frame = VIDEO_MAX_FRAME;
  }

  if (golden_frame) {
    scoped_refptr<V4L2DecodeSurface> golden_frame_surface =
        VP8PictureToV4L2DecodeSurface(golden_frame);
    v4l2_frame_hdr.golden_frame = golden_frame_surface->output_record();
    ref_surfaces.push_back(golden_frame_surface);
  } else {
    v4l2_frame_hdr.golden_frame = VIDEO_MAX_FRAME;
  }

  if (alt_frame) {
    scoped_refptr<V4L2DecodeSurface> alt_frame_surface =
        VP8PictureToV4L2DecodeSurface(alt_frame);
    v4l2_frame_hdr.alt_frame = alt_frame_surface->output_record();
    ref_surfaces.push_back(alt_frame_surface);
  } else {
    v4l2_frame_hdr.alt_frame = VIDEO_MAX_FRAME;
  }

  struct v4l2_ext_control ctrl;
  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = V4L2_CID_MPEG_VIDEO_VP8_FRAME_HDR;
  ctrl.size = sizeof(v4l2_frame_hdr);
  ctrl.p_vp8_frame_hdr = &v4l2_frame_hdr;

  struct v4l2_ext_controls ext_ctrls;
  memset(&ext_ctrls, 0, sizeof(ext_ctrls));
  ext_ctrls.count = 1;
  ext_ctrls.controls = &ctrl;
  ext_ctrls.config_store = dec_surface->config_store();

  if (!v4l2_dec_->SubmitExtControls(&ext_ctrls))
    return false;

  dec_surface->SetReferenceSurfaces(ref_surfaces);

  if (!v4l2_dec_->SubmitSlice(dec_surface->input_record(), frame_hdr->data,
                              frame_hdr->frame_size))
    return false;

  v4l2_dec_->DecodeSurface(dec_surface);
  return true;
}

bool V4L2SliceVideoDecodeAccelerator::V4L2VP8Accelerator::OutputPicture(
    const scoped_refptr<VP8Picture>& pic) {
  scoped_refptr<V4L2DecodeSurface> dec_surface =
      VP8PictureToV4L2DecodeSurface(pic);

  v4l2_dec_->SurfaceReady(dec_surface);
  return true;
}

scoped_refptr<V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface>
V4L2SliceVideoDecodeAccelerator::V4L2VP8Accelerator::
    VP8PictureToV4L2DecodeSurface(const scoped_refptr<VP8Picture>& pic) {
  V4L2VP8Picture* v4l2_pic = pic->AsV4L2VP8Picture();
  CHECK(v4l2_pic);
  return v4l2_pic->dec_surface();
}

void V4L2SliceVideoDecodeAccelerator::DecodeSurface(
    const scoped_refptr<V4L2DecodeSurface>& dec_surface) {
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  DVLOGF(3) << "Submitting decode for surface: " << dec_surface->ToString();
  Enqueue(dec_surface);
}

void V4L2SliceVideoDecodeAccelerator::SurfaceReady(
    const scoped_refptr<V4L2DecodeSurface>& dec_surface) {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  decoder_display_queue_.push(dec_surface);
  TryOutputSurfaces();
}

void V4L2SliceVideoDecodeAccelerator::TryOutputSurfaces() {
  while (!decoder_display_queue_.empty()) {
    scoped_refptr<V4L2DecodeSurface> dec_surface =
        decoder_display_queue_.front();

    if (!dec_surface->decoded())
      break;

    decoder_display_queue_.pop();
    OutputSurface(dec_surface);
  }
}

void V4L2SliceVideoDecodeAccelerator::OutputSurface(
    const scoped_refptr<V4L2DecodeSurface>& dec_surface) {
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());

  OutputRecord& output_record =
      output_buffer_map_[dec_surface->output_record()];

  bool inserted =
      surfaces_at_display_.insert(std::make_pair(output_record.picture_id,
                                                 dec_surface)).second;
  DCHECK(inserted);

  DCHECK(!output_record.at_client);
  DCHECK(!output_record.at_device);
  DCHECK_NE(output_record.egl_image, EGL_NO_IMAGE_KHR);
  DCHECK_NE(output_record.picture_id, -1);
  output_record.at_client = true;

  // TODO(posciak): Use visible size from decoder here instead
  // (crbug.com/402760). Passing (0, 0) results in the client using the
  // visible size extracted from the container instead.
  media::Picture picture(output_record.picture_id, dec_surface->bitstream_id(),
                         gfx::Rect(0, 0), false);
  DVLOGF(3) << dec_surface->ToString()
            << ", bitstream_id: " << picture.bitstream_buffer_id()
            << ", picture_id: " << picture.picture_buffer_id();
  pending_picture_ready_.push(PictureRecord(output_record.cleared, picture));
  SendPictureReady();
  output_record.cleared = true;
}

scoped_refptr<V4L2SliceVideoDecodeAccelerator::V4L2DecodeSurface>
V4L2SliceVideoDecodeAccelerator::CreateSurface() {
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());
  DCHECK_EQ(state_, kDecoding);

  if (free_input_buffers_.empty() || free_output_buffers_.empty())
    return nullptr;

  int input = free_input_buffers_.front();
  free_input_buffers_.pop_front();
  int output = free_output_buffers_.front();
  free_output_buffers_.pop_front();

  InputRecord& input_record = input_buffer_map_[input];
  DCHECK_EQ(input_record.bytes_used, 0u);
  DCHECK_EQ(input_record.input_id, -1);
  DCHECK(decoder_current_bitstream_buffer_ != nullptr);
  input_record.input_id = decoder_current_bitstream_buffer_->input_id;

  scoped_refptr<V4L2DecodeSurface> dec_surface = new V4L2DecodeSurface(
      decoder_current_bitstream_buffer_->input_id, input, output,
      base::Bind(&V4L2SliceVideoDecodeAccelerator::ReuseOutputBuffer,
                 base::Unretained(this)));

  DVLOGF(4) << "Created surface " << input << " -> " << output;
  return dec_surface;
}

void V4L2SliceVideoDecodeAccelerator::SendPictureReady() {
  DVLOGF(3);
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());
  bool resetting_or_flushing = (decoder_resetting_ || decoder_flushing_);
  while (!pending_picture_ready_.empty()) {
    bool cleared = pending_picture_ready_.front().cleared;
    const media::Picture& picture = pending_picture_ready_.front().picture;
    if (cleared && picture_clearing_count_ == 0) {
      DVLOGF(4) << "Posting picture ready to IO for: "
                << picture.picture_buffer_id();
      // This picture is cleared. Post it to IO thread to reduce latency. This
      // should be the case after all pictures are cleared at the beginning.
      io_task_runner_->PostTask(
          FROM_HERE, base::Bind(&Client::PictureReady, io_client_, picture));
      pending_picture_ready_.pop();
    } else if (!cleared || resetting_or_flushing) {
      DVLOGF(3) << "cleared=" << pending_picture_ready_.front().cleared
                << ", decoder_resetting_=" << decoder_resetting_
                << ", decoder_flushing_=" << decoder_flushing_
                << ", picture_clearing_count_=" << picture_clearing_count_;
      DVLOGF(4) << "Posting picture ready to GPU for: "
                << picture.picture_buffer_id();
      // If the picture is not cleared, post it to the child thread because it
      // has to be cleared in the child thread. A picture only needs to be
      // cleared once. If the decoder is resetting or flushing, send all
      // pictures to ensure PictureReady arrive before reset or flush done.
      child_task_runner_->PostTaskAndReply(
          FROM_HERE, base::Bind(&Client::PictureReady, client_, picture),
          // Unretained is safe. If Client::PictureReady gets to run, |this| is
          // alive. Destroy() will wait the decode thread to finish.
          base::Bind(&V4L2SliceVideoDecodeAccelerator::PictureCleared,
                     base::Unretained(this)));
      picture_clearing_count_++;
      pending_picture_ready_.pop();
    } else {
      // This picture is cleared. But some pictures are about to be cleared on
      // the child thread. To preserve the order, do not send this until those
      // pictures are cleared.
      break;
    }
  }
}

void V4L2SliceVideoDecodeAccelerator::PictureCleared() {
  DVLOGF(3) << "clearing count=" << picture_clearing_count_;
  DCHECK(decoder_thread_task_runner_->BelongsToCurrentThread());
  DCHECK_GT(picture_clearing_count_, 0);
  picture_clearing_count_--;
  SendPictureReady();
}

bool V4L2SliceVideoDecodeAccelerator::CanDecodeOnIOThread() {
  return true;
}

// static
media::VideoDecodeAccelerator::SupportedProfiles
V4L2SliceVideoDecodeAccelerator::GetSupportedProfiles() {
  scoped_refptr<V4L2Device> device = V4L2Device::Create(V4L2Device::kDecoder);
  if (!device)
    return SupportedProfiles();

  return device->GetSupportedDecodeProfiles(arraysize(supported_input_fourccs_),
                                            supported_input_fourccs_);
}

}  // namespace content
