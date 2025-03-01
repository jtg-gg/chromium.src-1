// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// IPC messages for the Cast transport API.

#include <stdint.h>

#include "ipc/ipc_message_macros.h"
#include "media/cast/common/rtp_time.h"
#include "media/cast/logging/logging_defines.h"
#include "media/cast/net/cast_transport_sender.h"
#include "media/cast/net/rtcp/rtcp_defines.h"
#include "net/base/ip_endpoint.h"

#ifndef CHROME_COMMON_CAST_MESSAGES_H_
#define CHROME_COMMON_CAST_MESSAGES_H_

namespace IPC {

template<>
struct ParamTraits<media::cast::RtpTimeTicks> {
  using param_type = media::cast::RtpTimeTicks;
  static void Write(base::Pickle* m, const param_type& p);
  static bool Read(const base::Pickle* m,
                   base::PickleIterator* iter,
                   param_type* r);
  static void Log(const param_type& p, std::string* l);
};

}  // namespace IPC

#endif  // CHROME_COMMON_CAST_MESSAGES_H_

// Multiply-included message file, hence no include guard from here.

#undef IPC_MESSAGE_EXPORT
#define IPC_MESSAGE_EXPORT
#define IPC_MESSAGE_START CastMsgStart

IPC_ENUM_TRAITS_MAX_VALUE(
    media::cast::EncodedFrame::Dependency,
    media::cast::EncodedFrame::DEPENDENCY_LAST)
IPC_ENUM_TRAITS_MAX_VALUE(media::cast::Codec,
                          media::cast::CODEC_LAST)
IPC_ENUM_TRAITS_MAX_VALUE(media::cast::CastTransportStatus,
                          media::cast::CAST_TRANSPORT_STATUS_LAST)
IPC_ENUM_TRAITS_MAX_VALUE(media::cast::CastLoggingEvent,
                          media::cast::kNumOfLoggingEvents)
IPC_ENUM_TRAITS_MAX_VALUE(media::cast::EventMediaType,
                          media::cast::EVENT_MEDIA_TYPE_LAST)

IPC_STRUCT_TRAITS_BEGIN(media::cast::EncodedFrame)
  IPC_STRUCT_TRAITS_MEMBER(dependency)
  IPC_STRUCT_TRAITS_MEMBER(frame_id)
  IPC_STRUCT_TRAITS_MEMBER(referenced_frame_id)
  IPC_STRUCT_TRAITS_MEMBER(rtp_timestamp)
  IPC_STRUCT_TRAITS_MEMBER(reference_time)
  IPC_STRUCT_TRAITS_MEMBER(new_playout_delay_ms)
  IPC_STRUCT_TRAITS_MEMBER(data)
IPC_STRUCT_TRAITS_END()

IPC_STRUCT_TRAITS_BEGIN(media::cast::RtcpDlrrReportBlock)
  IPC_STRUCT_TRAITS_MEMBER(last_rr)
  IPC_STRUCT_TRAITS_MEMBER(delay_since_last_rr)
IPC_STRUCT_TRAITS_END()

IPC_STRUCT_TRAITS_BEGIN(media::cast::CastTransportRtpConfig)
  IPC_STRUCT_TRAITS_MEMBER(ssrc)
  IPC_STRUCT_TRAITS_MEMBER(feedback_ssrc)
  IPC_STRUCT_TRAITS_MEMBER(rtp_payload_type)
  IPC_STRUCT_TRAITS_MEMBER(aes_key)
  IPC_STRUCT_TRAITS_MEMBER(aes_iv_mask)
IPC_STRUCT_TRAITS_END()

IPC_STRUCT_TRAITS_BEGIN(media::cast::PacketEvent)
  IPC_STRUCT_TRAITS_MEMBER(rtp_timestamp)
  IPC_STRUCT_TRAITS_MEMBER(frame_id)
  IPC_STRUCT_TRAITS_MEMBER(max_packet_id)
  IPC_STRUCT_TRAITS_MEMBER(packet_id)
  IPC_STRUCT_TRAITS_MEMBER(size)
  IPC_STRUCT_TRAITS_MEMBER(timestamp)
  IPC_STRUCT_TRAITS_MEMBER(type)
  IPC_STRUCT_TRAITS_MEMBER(media_type)
IPC_STRUCT_TRAITS_END()

IPC_STRUCT_TRAITS_BEGIN(media::cast::FrameEvent)
  IPC_STRUCT_TRAITS_MEMBER(rtp_timestamp)
  IPC_STRUCT_TRAITS_MEMBER(frame_id)
  IPC_STRUCT_TRAITS_MEMBER(size)
  IPC_STRUCT_TRAITS_MEMBER(timestamp)
  IPC_STRUCT_TRAITS_MEMBER(type)
  IPC_STRUCT_TRAITS_MEMBER(media_type)
  IPC_STRUCT_TRAITS_MEMBER(delay_delta)
  IPC_STRUCT_TRAITS_MEMBER(key_frame)
  IPC_STRUCT_TRAITS_MEMBER(target_bitrate)
IPC_STRUCT_TRAITS_END()

IPC_STRUCT_TRAITS_BEGIN(media::cast::RtcpCastMessage)
  IPC_STRUCT_TRAITS_MEMBER(media_ssrc)
  IPC_STRUCT_TRAITS_MEMBER(ack_frame_id)
  IPC_STRUCT_TRAITS_MEMBER(target_delay_ms)
  IPC_STRUCT_TRAITS_MEMBER(missing_frames_and_packets)
IPC_STRUCT_TRAITS_END()

IPC_STRUCT_TRAITS_BEGIN(media::cast::RtpReceiverStatistics)
  IPC_STRUCT_TRAITS_MEMBER(fraction_lost)
  IPC_STRUCT_TRAITS_MEMBER(cumulative_lost)
  IPC_STRUCT_TRAITS_MEMBER(extended_high_sequence_number)
  IPC_STRUCT_TRAITS_MEMBER(jitter)
IPC_STRUCT_TRAITS_END()

IPC_STRUCT_TRAITS_BEGIN(media::cast::RtcpEvent)
  IPC_STRUCT_TRAITS_MEMBER(type)
  IPC_STRUCT_TRAITS_MEMBER(timestamp)
  IPC_STRUCT_TRAITS_MEMBER(delay_delta)
  IPC_STRUCT_TRAITS_MEMBER(packet_id)
IPC_STRUCT_TRAITS_END()

IPC_STRUCT_TRAITS_BEGIN(media::cast::RtcpTimeData)
  IPC_STRUCT_TRAITS_MEMBER(ntp_seconds)
  IPC_STRUCT_TRAITS_MEMBER(ntp_fraction)
  IPC_STRUCT_TRAITS_MEMBER(timestamp)
IPC_STRUCT_TRAITS_END()

IPC_STRUCT_TRAITS_BEGIN(media::cast::SendRtcpFromRtpReceiver_Params)
  IPC_STRUCT_TRAITS_MEMBER(ssrc)
  IPC_STRUCT_TRAITS_MEMBER(sender_ssrc)
  IPC_STRUCT_TRAITS_MEMBER(time_data)
  IPC_STRUCT_TRAITS_MEMBER(cast_message)
  IPC_STRUCT_TRAITS_MEMBER(target_delay)
  IPC_STRUCT_TRAITS_MEMBER(rtcp_events)
  IPC_STRUCT_TRAITS_MEMBER(rtp_receiver_statistics)
IPC_STRUCT_TRAITS_END()


// Cast messages sent from the browser to the renderer.

IPC_MESSAGE_CONTROL2(CastMsg_ReceivedPacket,
                     int32_t /* channel_id */,
                     media::cast::Packet /* packet */)

IPC_MESSAGE_CONTROL3(CastMsg_Rtt,
                     int32_t /* channel_id */,
                     uint32_t /* ssrc */,
                     base::TimeDelta /* rtt */)

IPC_MESSAGE_CONTROL3(CastMsg_RtcpCastMessage,
                     int32_t /* channel_id */,
                     uint32_t /* ssrc */,
                     media::cast::RtcpCastMessage /* cast_message */)

IPC_MESSAGE_CONTROL2(CastMsg_NotifyStatusChange,
                     int32_t /* channel_id */,
                     media::cast::CastTransportStatus /* status */)

IPC_MESSAGE_CONTROL3(CastMsg_RawEvents,
                     int32_t /* channel_id */,
                     std::vector<media::cast::PacketEvent> /* packet_events */,
                     std::vector<media::cast::FrameEvent> /* frame_events */)

// Cast messages sent from the renderer to the browser.

IPC_MESSAGE_CONTROL2(CastHostMsg_InitializeAudio,
                     int32_t /*channel_id*/,
                     media::cast::CastTransportRtpConfig /*config*/)

IPC_MESSAGE_CONTROL2(CastHostMsg_InitializeVideo,
                     int32_t /*channel_id*/,
                     media::cast::CastTransportRtpConfig /*config*/)

IPC_MESSAGE_CONTROL3(CastHostMsg_InsertFrame,
                     int32_t /* channel_id */,
                     uint32_t /* ssrc */,
                     media::cast::EncodedFrame /* audio/video frame */)

IPC_MESSAGE_CONTROL4(CastHostMsg_SendSenderReport,
                     int32_t /* channel_id */,
                     uint32_t /* ssrc */,
                     base::TimeTicks /* current_time */,
                     media::cast::RtpTimeTicks /* cur_time_as_rtp_timestamp */)

IPC_MESSAGE_CONTROL3(CastHostMsg_CancelSendingFrames,
                     int32_t /* channel_id */,
                     uint32_t /* ssrc */,
                     std::vector<uint32_t> /* frame_ids */)

IPC_MESSAGE_CONTROL3(CastHostMsg_ResendFrameForKickstart,
                     int32_t /* channel_id */,
                     uint32_t /* ssrc */,
                     uint32_t /* frame_id */)

IPC_MESSAGE_CONTROL2(CastHostMsg_AddValidSsrc,
                     int32_t /* channel id */,
                     uint32_t /* ssrc */)

IPC_MESSAGE_CONTROL2(CastHostMsg_SendRtcpFromRtpReceiver,
                     int32_t /* channel id */,
                     media::cast::SendRtcpFromRtpReceiver_Params /* data */)

IPC_MESSAGE_CONTROL4(CastHostMsg_New,
                     int32_t /* channel_id */,
                     net::IPEndPoint /* local_end_point */,
                     net::IPEndPoint /* remote_end_point */,
                     base::DictionaryValue /* options */)

IPC_MESSAGE_CONTROL1(CastHostMsg_Delete, int32_t /* channel_id */)
