// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/spdy/spdy_session.h"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

#include "base/bind.h"
#include "base/compiler_specific.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/metrics/field_trial.h"
#include "base/metrics/histogram_macros.h"
#include "base/metrics/sparse_histogram.h"
#include "base/profiler/scoped_tracker.h"
#include "base/single_thread_task_runner.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "base/values.h"
#include "crypto/ec_private_key.h"
#include "crypto/ec_signature_creator.h"
#include "net/base/connection_type_histograms.h"
#include "net/base/proxy_delegate.h"
#include "net/cert/asn1_util.h"
#include "net/cert/cert_verify_result.h"
#include "net/http/http_log_util.h"
#include "net/http/http_network_session.h"
#include "net/http/http_server_properties.h"
#include "net/http/http_util.h"
#include "net/http/transport_security_state.h"
#include "net/log/net_log.h"
#include "net/proxy/proxy_server.h"
#include "net/socket/ssl_client_socket.h"
#include "net/spdy/spdy_buffer_producer.h"
#include "net/spdy/spdy_frame_builder.h"
#include "net/spdy/spdy_http_utils.h"
#include "net/spdy/spdy_protocol.h"
#include "net/spdy/spdy_session_pool.h"
#include "net/spdy/spdy_stream.h"
#include "net/ssl/channel_id_service.h"
#include "net/ssl/ssl_cipher_suite_names.h"
#include "net/ssl/ssl_connection_status_flags.h"

namespace net {

namespace {

const int kReadBufferSize = 8 * 1024;
const int kDefaultConnectionAtRiskOfLossSeconds = 10;
const int kHungIntervalSeconds = 10;

// Minimum seconds that unclaimed pushed streams will be kept in memory.
const int kMinPushedStreamLifetimeSeconds = 300;

// Field trial constants
const char kSpdyDependenciesFieldTrial[] = "SpdyEnableDependencies";
const char kSpdyDepencenciesFieldTrialEnable[] = "Enable";

// Whether the creation of SPDY dependencies based on priority is
// enabled by default.
static bool priority_dependency_enabled_default = false;

scoped_ptr<base::ListValue> SpdyHeaderBlockToListValue(
    const SpdyHeaderBlock& headers,
    NetLogCaptureMode capture_mode) {
  scoped_ptr<base::ListValue> headers_list(new base::ListValue());
  for (SpdyHeaderBlock::const_iterator it = headers.begin();
       it != headers.end(); ++it) {
    headers_list->AppendString(
        it->first.as_string() + ": " +
        ElideHeaderValueForNetLog(capture_mode, it->first.as_string(),
                                  it->second.as_string()));
  }
  return headers_list;
}

scoped_ptr<base::Value> NetLogSpdySynStreamSentCallback(
    const SpdyHeaderBlock* headers,
    bool fin,
    bool unidirectional,
    SpdyPriority spdy_priority,
    SpdyStreamId stream_id,
    NetLogCaptureMode capture_mode) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->Set("headers", SpdyHeaderBlockToListValue(*headers, capture_mode));
  dict->SetBoolean("fin", fin);
  dict->SetBoolean("unidirectional", unidirectional);
  dict->SetInteger("priority", static_cast<int>(spdy_priority));
  dict->SetInteger("stream_id", stream_id);
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdyHeadersSentCallback(
    const SpdyHeaderBlock* headers,
    bool fin,
    SpdyStreamId stream_id,
    bool has_priority,
    uint32_t priority,
    SpdyStreamId parent_stream_id,
    bool exclusive,
    NetLogCaptureMode capture_mode) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->Set("headers", SpdyHeaderBlockToListValue(*headers, capture_mode));
  dict->SetBoolean("fin", fin);
  dict->SetInteger("stream_id", stream_id);
  dict->SetBoolean("has_priority", has_priority);
  if (has_priority) {
    dict->SetInteger("parent_stream_id", parent_stream_id);
    dict->SetInteger("priority", static_cast<int>(priority));
    dict->SetBoolean("exclusive", exclusive);
  }
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdySynStreamReceivedCallback(
    const SpdyHeaderBlock* headers,
    bool fin,
    bool unidirectional,
    SpdyPriority spdy_priority,
    SpdyStreamId stream_id,
    SpdyStreamId associated_stream,
    NetLogCaptureMode capture_mode) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->Set("headers", SpdyHeaderBlockToListValue(*headers, capture_mode));
  dict->SetBoolean("fin", fin);
  dict->SetBoolean("unidirectional", unidirectional);
  dict->SetInteger("priority", static_cast<int>(spdy_priority));
  dict->SetInteger("stream_id", stream_id);
  dict->SetInteger("associated_stream", associated_stream);
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdySynReplyOrHeadersReceivedCallback(
    const SpdyHeaderBlock* headers,
    bool fin,
    SpdyStreamId stream_id,
    NetLogCaptureMode capture_mode) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->Set("headers", SpdyHeaderBlockToListValue(*headers, capture_mode));
  dict->SetBoolean("fin", fin);
  dict->SetInteger("stream_id", stream_id);
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdySessionCloseCallback(
    int net_error,
    const std::string* description,
    NetLogCaptureMode /* capture_mode */) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->SetInteger("net_error", net_error);
  dict->SetString("description", *description);
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdySessionCallback(
    const HostPortProxyPair* host_pair,
    NetLogCaptureMode /* capture_mode */) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->SetString("host", host_pair->first.ToString());
  dict->SetString("proxy", host_pair->second.ToPacString());
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdyInitializedCallback(
    NetLog::Source source,
    const NextProto protocol_version,
    NetLogCaptureMode /* capture_mode */) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  if (source.IsValid()) {
    source.AddToEventParameters(dict.get());
  }
  dict->SetString("protocol",
                  SSLClientSocket::NextProtoToString(protocol_version));
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdySettingsCallback(
    const HostPortPair& host_port_pair,
    bool clear_persisted,
    NetLogCaptureMode /* capture_mode */) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->SetString("host", host_port_pair.ToString());
  dict->SetBoolean("clear_persisted", clear_persisted);
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdySettingCallback(
    SpdySettingsIds id,
    const SpdyMajorVersion protocol_version,
    SpdySettingsFlags flags,
    uint32_t value,
    NetLogCaptureMode /* capture_mode */) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->SetInteger("id",
                   SpdyConstants::SerializeSettingId(protocol_version, id));
  dict->SetInteger("flags", flags);
  dict->SetInteger("value", value);
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdySendSettingsCallback(
    const SettingsMap* settings,
    const SpdyMajorVersion protocol_version,
    NetLogCaptureMode /* capture_mode */) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  scoped_ptr<base::ListValue> settings_list(new base::ListValue());
  for (SettingsMap::const_iterator it = settings->begin();
       it != settings->end(); ++it) {
    const SpdySettingsIds id = it->first;
    const SpdySettingsFlags flags = it->second.first;
    const uint32_t value = it->second.second;
    settings_list->Append(new base::StringValue(base::StringPrintf(
        "[id:%u flags:%u value:%u]",
        SpdyConstants::SerializeSettingId(protocol_version, id),
        flags,
        value)));
  }
  dict->Set("settings", std::move(settings_list));
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdyWindowUpdateFrameCallback(
    SpdyStreamId stream_id,
    uint32_t delta,
    NetLogCaptureMode /* capture_mode */) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->SetInteger("stream_id", static_cast<int>(stream_id));
  dict->SetInteger("delta", delta);
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdySessionWindowUpdateCallback(
    int32_t delta,
    int32_t window_size,
    NetLogCaptureMode /* capture_mode */) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->SetInteger("delta", delta);
  dict->SetInteger("window_size", window_size);
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdyDataCallback(
    SpdyStreamId stream_id,
    int size,
    bool fin,
    NetLogCaptureMode /* capture_mode */) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->SetInteger("stream_id", static_cast<int>(stream_id));
  dict->SetInteger("size", size);
  dict->SetBoolean("fin", fin);
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdyRstCallback(
    SpdyStreamId stream_id,
    int status,
    const std::string* description,
    NetLogCaptureMode /* capture_mode */) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->SetInteger("stream_id", static_cast<int>(stream_id));
  dict->SetInteger("status", status);
  dict->SetString("description", *description);
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdyPingCallback(
    SpdyPingId unique_id,
    bool is_ack,
    const char* type,
    NetLogCaptureMode /* capture_mode */) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->SetInteger("unique_id", static_cast<int>(unique_id));
  dict->SetString("type", type);
  dict->SetBoolean("is_ack", is_ack);
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdyGoAwayCallback(
    SpdyStreamId last_stream_id,
    int active_streams,
    int unclaimed_streams,
    SpdyGoAwayStatus status,
    StringPiece debug_data,
    NetLogCaptureMode capture_mode) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->SetInteger("last_accepted_stream_id",
                   static_cast<int>(last_stream_id));
  dict->SetInteger("active_streams", active_streams);
  dict->SetInteger("unclaimed_streams", unclaimed_streams);
  dict->SetInteger("status", static_cast<int>(status));
  dict->SetString("debug_data",
                  ElideGoAwayDebugDataForNetLog(capture_mode, debug_data));
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdyPushPromiseReceivedCallback(
    const SpdyHeaderBlock* headers,
    SpdyStreamId stream_id,
    SpdyStreamId promised_stream_id,
    NetLogCaptureMode capture_mode) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->Set("headers", SpdyHeaderBlockToListValue(*headers, capture_mode));
  dict->SetInteger("id", stream_id);
  dict->SetInteger("promised_stream_id", promised_stream_id);
  return std::move(dict);
}

scoped_ptr<base::Value> NetLogSpdyAdoptedPushStreamCallback(
    SpdyStreamId stream_id,
    const GURL* url,
    NetLogCaptureMode capture_mode) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->SetInteger("stream_id", stream_id);
  dict->SetString("url", url->spec());
  return std::move(dict);
}

// Helper function to return the total size of an array of objects
// with .size() member functions.
template <typename T, size_t N> size_t GetTotalSize(const T (&arr)[N]) {
  size_t total_size = 0;
  for (size_t i = 0; i < N; ++i) {
    total_size += arr[i].size();
  }
  return total_size;
}

// Helper class for std:find_if on STL container containing
// SpdyStreamRequest weak pointers.
class RequestEquals {
 public:
  RequestEquals(const base::WeakPtr<SpdyStreamRequest>& request)
      : request_(request) {}

  bool operator()(const base::WeakPtr<SpdyStreamRequest>& request) const {
    return request_.get() == request.get();
  }

 private:
  const base::WeakPtr<SpdyStreamRequest> request_;
};

// The maximum number of concurrent streams we will ever create.  Even if
// the server permits more, we will never exceed this limit.
const size_t kMaxConcurrentStreamLimit = 256;

}  // namespace

SpdyProtocolErrorDetails MapFramerErrorToProtocolError(
    SpdyFramer::SpdyError err) {
  switch(err) {
    case SpdyFramer::SPDY_NO_ERROR:
      return SPDY_ERROR_NO_ERROR;
    case SpdyFramer::SPDY_INVALID_CONTROL_FRAME:
      return SPDY_ERROR_INVALID_CONTROL_FRAME;
    case SpdyFramer::SPDY_CONTROL_PAYLOAD_TOO_LARGE:
      return SPDY_ERROR_CONTROL_PAYLOAD_TOO_LARGE;
    case SpdyFramer::SPDY_ZLIB_INIT_FAILURE:
      return SPDY_ERROR_ZLIB_INIT_FAILURE;
    case SpdyFramer::SPDY_UNSUPPORTED_VERSION:
      return SPDY_ERROR_UNSUPPORTED_VERSION;
    case SpdyFramer::SPDY_DECOMPRESS_FAILURE:
      return SPDY_ERROR_DECOMPRESS_FAILURE;
    case SpdyFramer::SPDY_COMPRESS_FAILURE:
      return SPDY_ERROR_COMPRESS_FAILURE;
    case SpdyFramer::SPDY_GOAWAY_FRAME_CORRUPT:
      return SPDY_ERROR_GOAWAY_FRAME_CORRUPT;
    case SpdyFramer::SPDY_RST_STREAM_FRAME_CORRUPT:
      return SPDY_ERROR_RST_STREAM_FRAME_CORRUPT;
    case SpdyFramer::SPDY_INVALID_DATA_FRAME_FLAGS:
      return SPDY_ERROR_INVALID_DATA_FRAME_FLAGS;
    case SpdyFramer::SPDY_INVALID_CONTROL_FRAME_FLAGS:
      return SPDY_ERROR_INVALID_CONTROL_FRAME_FLAGS;
    case SpdyFramer::SPDY_UNEXPECTED_FRAME:
      return SPDY_ERROR_UNEXPECTED_FRAME;
    default:
      NOTREACHED();
      return static_cast<SpdyProtocolErrorDetails>(-1);
  }
}

Error MapFramerErrorToNetError(SpdyFramer::SpdyError err) {
  switch (err) {
    case SpdyFramer::SPDY_NO_ERROR:
      return OK;
    case SpdyFramer::SPDY_INVALID_CONTROL_FRAME:
      return ERR_SPDY_PROTOCOL_ERROR;
    case SpdyFramer::SPDY_CONTROL_PAYLOAD_TOO_LARGE:
      return ERR_SPDY_FRAME_SIZE_ERROR;
    case SpdyFramer::SPDY_ZLIB_INIT_FAILURE:
      return ERR_SPDY_COMPRESSION_ERROR;
    case SpdyFramer::SPDY_UNSUPPORTED_VERSION:
      return ERR_SPDY_PROTOCOL_ERROR;
    case SpdyFramer::SPDY_DECOMPRESS_FAILURE:
      return ERR_SPDY_COMPRESSION_ERROR;
    case SpdyFramer::SPDY_COMPRESS_FAILURE:
      return ERR_SPDY_COMPRESSION_ERROR;
    case SpdyFramer::SPDY_GOAWAY_FRAME_CORRUPT:
      return ERR_SPDY_PROTOCOL_ERROR;
    case SpdyFramer::SPDY_RST_STREAM_FRAME_CORRUPT:
      return ERR_SPDY_PROTOCOL_ERROR;
    case SpdyFramer::SPDY_INVALID_DATA_FRAME_FLAGS:
      return ERR_SPDY_PROTOCOL_ERROR;
    case SpdyFramer::SPDY_INVALID_CONTROL_FRAME_FLAGS:
      return ERR_SPDY_PROTOCOL_ERROR;
    case SpdyFramer::SPDY_UNEXPECTED_FRAME:
      return ERR_SPDY_PROTOCOL_ERROR;
    default:
      NOTREACHED();
      return ERR_SPDY_PROTOCOL_ERROR;
  }
}

SpdyProtocolErrorDetails MapRstStreamStatusToProtocolError(
    SpdyRstStreamStatus status) {
  switch(status) {
    case RST_STREAM_PROTOCOL_ERROR:
      return STATUS_CODE_PROTOCOL_ERROR;
    case RST_STREAM_INVALID_STREAM:
      return STATUS_CODE_INVALID_STREAM;
    case RST_STREAM_REFUSED_STREAM:
      return STATUS_CODE_REFUSED_STREAM;
    case RST_STREAM_UNSUPPORTED_VERSION:
      return STATUS_CODE_UNSUPPORTED_VERSION;
    case RST_STREAM_CANCEL:
      return STATUS_CODE_CANCEL;
    case RST_STREAM_INTERNAL_ERROR:
      return STATUS_CODE_INTERNAL_ERROR;
    case RST_STREAM_FLOW_CONTROL_ERROR:
      return STATUS_CODE_FLOW_CONTROL_ERROR;
    case RST_STREAM_STREAM_IN_USE:
      return STATUS_CODE_STREAM_IN_USE;
    case RST_STREAM_STREAM_ALREADY_CLOSED:
      return STATUS_CODE_STREAM_ALREADY_CLOSED;
    case RST_STREAM_FRAME_SIZE_ERROR:
      return STATUS_CODE_FRAME_SIZE_ERROR;
    case RST_STREAM_SETTINGS_TIMEOUT:
      return STATUS_CODE_SETTINGS_TIMEOUT;
    case RST_STREAM_CONNECT_ERROR:
      return STATUS_CODE_CONNECT_ERROR;
    case RST_STREAM_ENHANCE_YOUR_CALM:
      return STATUS_CODE_ENHANCE_YOUR_CALM;
    case RST_STREAM_INADEQUATE_SECURITY:
      return STATUS_CODE_INADEQUATE_SECURITY;
    case RST_STREAM_HTTP_1_1_REQUIRED:
      return STATUS_CODE_HTTP_1_1_REQUIRED;
    default:
      NOTREACHED();
      return static_cast<SpdyProtocolErrorDetails>(-1);
  }
}

SpdyGoAwayStatus MapNetErrorToGoAwayStatus(Error err) {
  switch (err) {
    case OK:
      return GOAWAY_NO_ERROR;
    case ERR_SPDY_PROTOCOL_ERROR:
      return GOAWAY_PROTOCOL_ERROR;
    case ERR_SPDY_FLOW_CONTROL_ERROR:
      return GOAWAY_FLOW_CONTROL_ERROR;
    case ERR_SPDY_FRAME_SIZE_ERROR:
      return GOAWAY_FRAME_SIZE_ERROR;
    case ERR_SPDY_COMPRESSION_ERROR:
      return GOAWAY_COMPRESSION_ERROR;
    case ERR_SPDY_INADEQUATE_TRANSPORT_SECURITY:
      return GOAWAY_INADEQUATE_SECURITY;
    default:
      return GOAWAY_PROTOCOL_ERROR;
  }
}

void SplitPushedHeadersToRequestAndResponse(const SpdyHeaderBlock& headers,
                                            SpdyMajorVersion protocol_version,
                                            SpdyHeaderBlock* request_headers,
                                            SpdyHeaderBlock* response_headers) {
  DCHECK(response_headers);
  DCHECK(request_headers);
  for (SpdyHeaderBlock::const_iterator it = headers.begin();
       it != headers.end();
       ++it) {
    SpdyHeaderBlock* to_insert = response_headers;
    const char* host = protocol_version >= HTTP2 ? ":authority" : ":host";
    static const char scheme[] = ":scheme";
    static const char path[] = ":path";
    if (it->first == host || it->first == scheme || it->first == path) {
      to_insert = request_headers;
    }
    to_insert->insert(*it);
  }
}

SpdyStreamRequest::SpdyStreamRequest() : weak_ptr_factory_(this) {
  Reset();
}

SpdyStreamRequest::~SpdyStreamRequest() {
  CancelRequest();
}

int SpdyStreamRequest::StartRequest(
    SpdyStreamType type,
    const base::WeakPtr<SpdySession>& session,
    const GURL& url,
    RequestPriority priority,
    const BoundNetLog& net_log,
    const CompletionCallback& callback) {
  DCHECK(session);
  DCHECK(!session_);
  DCHECK(!stream_);
  DCHECK(callback_.is_null());

  type_ = type;
  session_ = session;
  url_ = url;
  priority_ = priority;
  net_log_ = net_log;
  callback_ = callback;

  base::WeakPtr<SpdyStream> stream;
  int rv = session->TryCreateStream(weak_ptr_factory_.GetWeakPtr(), &stream);
  if (rv == OK) {
    Reset();
    stream_ = stream;
  }
  return rv;
}

void SpdyStreamRequest::CancelRequest() {
  if (session_)
    session_->CancelStreamRequest(weak_ptr_factory_.GetWeakPtr());
  Reset();
  // Do this to cancel any pending CompleteStreamRequest() tasks.
  weak_ptr_factory_.InvalidateWeakPtrs();
}

base::WeakPtr<SpdyStream> SpdyStreamRequest::ReleaseStream() {
  DCHECK(!session_);
  base::WeakPtr<SpdyStream> stream = stream_;
  DCHECK(stream);
  Reset();
  return stream;
}

void SpdyStreamRequest::OnRequestCompleteSuccess(
    const base::WeakPtr<SpdyStream>& stream) {
  DCHECK(session_);
  DCHECK(!stream_);
  DCHECK(!callback_.is_null());
  CompletionCallback callback = callback_;
  Reset();
  DCHECK(stream);
  stream_ = stream;
  callback.Run(OK);
}

void SpdyStreamRequest::OnRequestCompleteFailure(int rv) {
  DCHECK(session_);
  DCHECK(!stream_);
  DCHECK(!callback_.is_null());
  CompletionCallback callback = callback_;
  Reset();
  DCHECK_NE(rv, OK);
  callback.Run(rv);
}

void SpdyStreamRequest::Reset() {
  type_ = SPDY_BIDIRECTIONAL_STREAM;
  session_.reset();
  stream_.reset();
  url_ = GURL();
  priority_ = MINIMUM_PRIORITY;
  net_log_ = BoundNetLog();
  callback_.Reset();
}

SpdySession::ActiveStreamInfo::ActiveStreamInfo()
    : stream(NULL),
      waiting_for_syn_reply(false) {}

SpdySession::ActiveStreamInfo::ActiveStreamInfo(SpdyStream* stream)
    : stream(stream),
      waiting_for_syn_reply(stream->type() != SPDY_PUSH_STREAM) {
}

SpdySession::ActiveStreamInfo::~ActiveStreamInfo() {}

SpdySession::PushedStreamInfo::PushedStreamInfo() : stream_id(0) {}

SpdySession::PushedStreamInfo::PushedStreamInfo(
    SpdyStreamId stream_id,
    base::TimeTicks creation_time)
    : stream_id(stream_id),
      creation_time(creation_time) {}

SpdySession::PushedStreamInfo::~PushedStreamInfo() {}

// static
bool SpdySession::CanPool(TransportSecurityState* transport_security_state,
                          const SSLInfo& ssl_info,
                          const std::string& old_hostname,
                          const std::string& new_hostname) {
  // Pooling is prohibited if the server cert is not valid for the new domain,
  // and for connections on which client certs were sent. It is also prohibited
  // when channel ID was sent if the hosts are from different eTLDs+1.
  if (IsCertStatusError(ssl_info.cert_status))
    return false;

  if (ssl_info.client_cert_sent)
    return false;

  if (ssl_info.channel_id_sent &&
      ChannelIDService::GetDomainForHost(new_hostname) !=
      ChannelIDService::GetDomainForHost(old_hostname)) {
    return false;
  }

  bool unused = false;
  if (!ssl_info.cert->VerifyNameMatch(new_hostname, &unused))
    return false;

  std::string pinning_failure_log;
  // DISABLE_PIN_REPORTS is set here because this check can fail in
  // normal operation without being indicative of a misconfiguration or
  // attack.
  //
  // TODO(estark): replace 0 below with the port of the connection
  // (though it won't actually be used since reports aren't getting
  // sent).
  if (!transport_security_state->CheckPublicKeyPins(
          HostPortPair(new_hostname, 0), ssl_info.is_issued_by_known_root,
          ssl_info.public_key_hashes, ssl_info.unverified_cert.get(),
          ssl_info.cert.get(), TransportSecurityState::DISABLE_PIN_REPORTS,
          &pinning_failure_log)) {
    return false;
  }

  return true;
}

SpdySession::SpdySession(
    const SpdySessionKey& spdy_session_key,
    const base::WeakPtr<HttpServerProperties>& http_server_properties,
    TransportSecurityState* transport_security_state,
    bool verify_domain_authentication,
    bool enable_sending_initial_data,
    bool enable_ping_based_connection_checking,
    NextProto default_protocol,
    size_t session_max_recv_window_size,
    size_t stream_max_recv_window_size,
    TimeFunc time_func,
    ProxyDelegate* proxy_delegate,
    NetLog* net_log)
    : in_io_loop_(false),
      spdy_session_key_(spdy_session_key),
      pool_(NULL),
      http_server_properties_(http_server_properties),
      transport_security_state_(transport_security_state),
      read_buffer_(new IOBuffer(kReadBufferSize)),
      stream_hi_water_mark_(kFirstStreamId),
      last_accepted_push_stream_id_(0),
      num_pushed_streams_(0u),
      num_active_pushed_streams_(0u),
      in_flight_write_frame_type_(DATA),
      in_flight_write_frame_size_(0),
      is_secure_(false),
      certificate_error_code_(OK),
      availability_state_(STATE_AVAILABLE),
      read_state_(READ_STATE_DO_READ),
      write_state_(WRITE_STATE_IDLE),
      error_on_close_(OK),
      max_concurrent_streams_(kInitialMaxConcurrentStreams),
      max_concurrent_pushed_streams_(kMaxConcurrentPushedStreams),
      streams_initiated_count_(0),
      streams_pushed_count_(0),
      streams_pushed_and_claimed_count_(0),
      streams_abandoned_count_(0),
      total_bytes_received_(0),
      sent_settings_(false),
      received_settings_(false),
      stalled_streams_(0),
      pings_in_flight_(0),
      next_ping_id_(1),
      last_activity_time_(time_func()),
      last_compressed_frame_len_(0),
      check_ping_status_pending_(false),
      send_connection_header_prefix_(false),
      session_send_window_size_(0),
      session_max_recv_window_size_(session_max_recv_window_size),
      session_recv_window_size_(0),
      session_unacked_recv_window_bytes_(0),
      stream_initial_send_window_size_(
          GetDefaultInitialWindowSize(default_protocol)),
      stream_max_recv_window_size_(stream_max_recv_window_size),
      net_log_(BoundNetLog::Make(net_log, NetLog::SOURCE_HTTP2_SESSION)),
      verify_domain_authentication_(verify_domain_authentication),
      enable_sending_initial_data_(enable_sending_initial_data),
      enable_ping_based_connection_checking_(
          enable_ping_based_connection_checking),
      protocol_(default_protocol),
      connection_at_risk_of_loss_time_(
          base::TimeDelta::FromSeconds(kDefaultConnectionAtRiskOfLossSeconds)),
      hung_interval_(base::TimeDelta::FromSeconds(kHungIntervalSeconds)),
      proxy_delegate_(proxy_delegate),
      time_func_(time_func),
      send_priority_dependency_(priority_dependency_enabled_default),
      weak_factory_(this) {
  DCHECK_GE(protocol_, kProtoSPDYMinimumVersion);
  DCHECK_LE(protocol_, kProtoSPDYMaximumVersion);
  DCHECK(HttpStreamFactory::spdy_enabled());
  net_log_.BeginEvent(
      NetLog::TYPE_HTTP2_SESSION,
      base::Bind(&NetLogSpdySessionCallback, &host_port_proxy_pair()));
  next_unclaimed_push_stream_sweep_time_ = time_func_() +
      base::TimeDelta::FromSeconds(kMinPushedStreamLifetimeSeconds);
  if (base::FieldTrialList::FindFullName(kSpdyDependenciesFieldTrial) ==
      kSpdyDepencenciesFieldTrialEnable) {
    send_priority_dependency_ = true;
  }
  // TODO(mbelshe): consider randomization of the stream_hi_water_mark.
}

SpdySession::~SpdySession() {
  CHECK(!in_io_loop_);
  DcheckDraining();

  // TODO(akalin): Check connection->is_initialized() instead. This
  // requires re-working CreateFakeSpdySession(), though.
  DCHECK(connection_->socket());
  // With SPDY we can't recycle sockets.
  connection_->socket()->Disconnect();

  RecordHistograms();

  net_log_.EndEvent(NetLog::TYPE_HTTP2_SESSION);
}

void SpdySession::InitializeWithSocket(
    scoped_ptr<ClientSocketHandle> connection,
    SpdySessionPool* pool,
    bool is_secure,
    int certificate_error_code) {
  CHECK(!in_io_loop_);
  DCHECK_EQ(availability_state_, STATE_AVAILABLE);
  DCHECK_EQ(read_state_, READ_STATE_DO_READ);
  DCHECK_EQ(write_state_, WRITE_STATE_IDLE);
  DCHECK(!connection_);

  DCHECK(certificate_error_code == OK ||
         certificate_error_code < ERR_IO_PENDING);
  // TODO(akalin): Check connection->is_initialized() instead. This
  // requires re-working CreateFakeSpdySession(), though.
  DCHECK(connection->socket());

  connection_ = std::move(connection);
  is_secure_ = is_secure;
  certificate_error_code_ = certificate_error_code;

  NextProto protocol_negotiated =
      connection_->socket()->GetNegotiatedProtocol();
  if (protocol_negotiated != kProtoUnknown) {
    protocol_ = protocol_negotiated;
    stream_initial_send_window_size_ = GetDefaultInitialWindowSize(protocol_);
  }
  DCHECK_GE(protocol_, kProtoSPDYMinimumVersion);
  DCHECK_LE(protocol_, kProtoSPDYMaximumVersion);

  if (protocol_ == kProtoHTTP2)
    send_connection_header_prefix_ = true;

  session_send_window_size_ = GetDefaultInitialWindowSize(protocol_);
  session_recv_window_size_ = GetDefaultInitialWindowSize(protocol_);

  buffered_spdy_framer_.reset(
      new BufferedSpdyFramer(NextProtoToSpdyMajorVersion(protocol_)));
  buffered_spdy_framer_->set_visitor(this);
  buffered_spdy_framer_->set_debug_visitor(this);
  UMA_HISTOGRAM_ENUMERATION(
      "Net.SpdyVersion3", protocol_ - kProtoSPDYHistogramOffset,
      kProtoSPDYMaximumVersion - kProtoSPDYHistogramOffset + 1);

  net_log_.AddEvent(
      NetLog::TYPE_HTTP2_SESSION_INITIALIZED,
      base::Bind(&NetLogSpdyInitializedCallback,
                 connection_->socket()->NetLog().source(), protocol_));

  DCHECK_EQ(availability_state_, STATE_AVAILABLE);
  connection_->AddHigherLayeredPool(this);
  if (enable_sending_initial_data_)
    SendInitialData();
  pool_ = pool;

  // Bootstrap the read loop.
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::Bind(&SpdySession::PumpReadLoop, weak_factory_.GetWeakPtr(),
                 READ_STATE_DO_READ, OK));
}

bool SpdySession::VerifyDomainAuthentication(const std::string& domain) {
  if (!verify_domain_authentication_)
    return true;

  if (availability_state_ == STATE_DRAINING)
    return false;

  SSLInfo ssl_info;
  bool was_npn_negotiated;
  NextProto protocol_negotiated = kProtoUnknown;
  if (!GetSSLInfo(&ssl_info, &was_npn_negotiated, &protocol_negotiated))
    return true;   // This is not a secure session, so all domains are okay.

  return CanPool(transport_security_state_, ssl_info,
                 host_port_pair().host(), domain);
}

int SpdySession::GetPushStream(
    const GURL& url,
    base::WeakPtr<SpdyStream>* stream,
    const BoundNetLog& stream_net_log) {
  CHECK(!in_io_loop_);

  stream->reset();

  if (availability_state_ == STATE_DRAINING)
    return ERR_CONNECTION_CLOSED;

  Error err = TryAccessStream(url);
  if (err != OK)
    return err;

  *stream = GetActivePushStream(url);
  if (*stream) {
    DCHECK_LT(streams_pushed_and_claimed_count_, streams_pushed_count_);
    streams_pushed_and_claimed_count_++;
  }
  return OK;
}

// {,Try}CreateStream() and TryAccessStream() can be called with
// |in_io_loop_| set if a stream is being created in response to
// another being closed due to received data.

Error SpdySession::TryAccessStream(const GURL& url) {
  if (is_secure_ && certificate_error_code_ != OK &&
      (url.SchemeIs("https") || url.SchemeIs("wss"))) {
    RecordProtocolErrorHistogram(
        PROTOCOL_ERROR_REQUEST_FOR_SECURE_CONTENT_OVER_INSECURE_SESSION);
    DoDrainSession(
        static_cast<Error>(certificate_error_code_),
        "Tried to get SPDY stream for secure content over an unauthenticated "
        "session.");
    return ERR_SPDY_PROTOCOL_ERROR;
  }
  return OK;
}

int SpdySession::TryCreateStream(
    const base::WeakPtr<SpdyStreamRequest>& request,
    base::WeakPtr<SpdyStream>* stream) {
  DCHECK(request);

  if (availability_state_ == STATE_GOING_AWAY)
    return ERR_FAILED;

  if (availability_state_ == STATE_DRAINING)
    return ERR_CONNECTION_CLOSED;

  Error err = TryAccessStream(request->url());
  if (err != OK)
    return err;

  if ((active_streams_.size() + created_streams_.size() - num_pushed_streams_ <
       max_concurrent_streams_)) {
    return CreateStream(*request, stream);
  }

  stalled_streams_++;
  net_log().AddEvent(NetLog::TYPE_HTTP2_SESSION_STALLED_MAX_STREAMS);
  RequestPriority priority = request->priority();
  CHECK_GE(priority, MINIMUM_PRIORITY);
  CHECK_LE(priority, MAXIMUM_PRIORITY);
  pending_create_stream_queues_[priority].push_back(request);
  return ERR_IO_PENDING;
}

int SpdySession::CreateStream(const SpdyStreamRequest& request,
                              base::WeakPtr<SpdyStream>* stream) {
  DCHECK_GE(request.priority(), MINIMUM_PRIORITY);
  DCHECK_LE(request.priority(), MAXIMUM_PRIORITY);

  if (availability_state_ == STATE_GOING_AWAY)
    return ERR_FAILED;

  if (availability_state_ == STATE_DRAINING)
    return ERR_CONNECTION_CLOSED;

  Error err = TryAccessStream(request.url());
  if (err != OK) {
    // This should have been caught in TryCreateStream().
    NOTREACHED();
    return err;
  }

  DCHECK(connection_->socket());
  UMA_HISTOGRAM_BOOLEAN("Net.SpdySession.CreateStreamWithSocketConnected",
                        connection_->socket()->IsConnected());
  if (!connection_->socket()->IsConnected()) {
    DoDrainSession(
        ERR_CONNECTION_CLOSED,
        "Tried to create SPDY stream for a closed socket connection.");
    return ERR_CONNECTION_CLOSED;
  }

  scoped_ptr<SpdyStream> new_stream(
      new SpdyStream(request.type(), GetWeakPtr(), request.url(),
                     request.priority(), stream_initial_send_window_size_,
                     stream_max_recv_window_size_, request.net_log()));
  *stream = new_stream->GetWeakPtr();
  InsertCreatedStream(std::move(new_stream));

  UMA_HISTOGRAM_CUSTOM_COUNTS(
      "Net.SpdyPriorityCount",
      static_cast<int>(request.priority()), 0, 10, 11);

  return OK;
}

void SpdySession::CancelStreamRequest(
    const base::WeakPtr<SpdyStreamRequest>& request) {
  DCHECK(request);
  RequestPriority priority = request->priority();
  CHECK_GE(priority, MINIMUM_PRIORITY);
  CHECK_LE(priority, MAXIMUM_PRIORITY);

#if DCHECK_IS_ON()
  // |request| should not be in a queue not matching its priority.
  for (int i = MINIMUM_PRIORITY; i <= MAXIMUM_PRIORITY; ++i) {
    if (priority == i)
      continue;
    PendingStreamRequestQueue* queue = &pending_create_stream_queues_[i];
    DCHECK(std::find_if(queue->begin(),
                        queue->end(),
                        RequestEquals(request)) == queue->end());
  }
#endif

  PendingStreamRequestQueue* queue =
      &pending_create_stream_queues_[priority];
  // Remove |request| from |queue| while preserving the order of the
  // other elements.
  PendingStreamRequestQueue::iterator it =
      std::find_if(queue->begin(), queue->end(), RequestEquals(request));
  // The request may already be removed if there's a
  // CompleteStreamRequest() in flight.
  if (it != queue->end()) {
    it = queue->erase(it);
    // |request| should be in the queue at most once, and if it is
    // present, should not be pending completion.
    DCHECK(std::find_if(it, queue->end(), RequestEquals(request)) ==
           queue->end());
  }
}

base::WeakPtr<SpdyStreamRequest> SpdySession::GetNextPendingStreamRequest() {
  for (int j = MAXIMUM_PRIORITY; j >= MINIMUM_PRIORITY; --j) {
    if (pending_create_stream_queues_[j].empty())
      continue;

    base::WeakPtr<SpdyStreamRequest> pending_request =
        pending_create_stream_queues_[j].front();
    DCHECK(pending_request);
    pending_create_stream_queues_[j].pop_front();
    return pending_request;
  }
  return base::WeakPtr<SpdyStreamRequest>();
}

void SpdySession::ProcessPendingStreamRequests() {
  size_t max_requests_to_process =
      max_concurrent_streams_ -
      (active_streams_.size() + created_streams_.size());
  for (size_t i = 0; i < max_requests_to_process; ++i) {
    base::WeakPtr<SpdyStreamRequest> pending_request =
        GetNextPendingStreamRequest();
    if (!pending_request)
      break;

    // Note that this post can race with other stream creations, and it's
    // possible that the un-stalled stream will be stalled again if it loses.
    // TODO(jgraettinger): Provide stronger ordering guarantees.
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::Bind(&SpdySession::CompleteStreamRequest,
                              weak_factory_.GetWeakPtr(), pending_request));
  }
}

void SpdySession::AddPooledAlias(const SpdySessionKey& alias_key) {
  pooled_aliases_.insert(alias_key);
}

SpdyMajorVersion SpdySession::GetProtocolVersion() const {
  DCHECK(buffered_spdy_framer_.get());
  return buffered_spdy_framer_->protocol_version();
}

bool SpdySession::HasAcceptableTransportSecurity() const {
  // If we're not even using TLS, we have no standards to meet.
  if (!is_secure_) {
    return true;
  }

  // We don't enforce transport security standards for older SPDY versions.
  if (GetProtocolVersion() < HTTP2) {
    return true;
  }

  SSLInfo ssl_info;
  CHECK(connection_->socket()->GetSSLInfo(&ssl_info));

  // HTTP/2 requires TLS 1.2+
  if (SSLConnectionStatusToVersion(ssl_info.connection_status) <
      SSL_CONNECTION_VERSION_TLS1_2) {
    return false;
  }

  if (!IsTLSCipherSuiteAllowedByHTTP2(
          SSLConnectionStatusToCipherSuite(ssl_info.connection_status))) {
    return false;
  }

  return true;
}

base::WeakPtr<SpdySession> SpdySession::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

bool SpdySession::CloseOneIdleConnection() {
  CHECK(!in_io_loop_);
  DCHECK(pool_);
  if (active_streams_.empty()) {
    DoDrainSession(ERR_CONNECTION_CLOSED, "Closing idle connection.");
  }
  // Return false as the socket wasn't immediately closed.
  return false;
}

// static
void SpdySession::SetPriorityDependencyDefaultForTesting(bool enable) {
  priority_dependency_enabled_default = enable;
}

void SpdySession::EnqueueStreamWrite(
    const base::WeakPtr<SpdyStream>& stream,
    SpdyFrameType frame_type,
    scoped_ptr<SpdyBufferProducer> producer) {
  DCHECK(frame_type == HEADERS ||
         frame_type == DATA ||
         frame_type == SYN_STREAM);
  EnqueueWrite(stream->priority(), frame_type, std::move(producer), stream);
}

scoped_ptr<SpdyFrame> SpdySession::CreateSynStream(
    SpdyStreamId stream_id,
    RequestPriority priority,
    SpdyControlFlags flags,
    const SpdyHeaderBlock& block) {
  ActiveStreamMap::const_iterator it = active_streams_.find(stream_id);
  CHECK(it != active_streams_.end());
  CHECK_EQ(it->second.stream->stream_id(), stream_id);

  SendPrefacePingIfNoneInFlight();

  DCHECK(buffered_spdy_framer_.get());
  SpdyPriority spdy_priority =
      ConvertRequestPriorityToSpdyPriority(priority, GetProtocolVersion());

  scoped_ptr<SpdyFrame> syn_frame;
  // TODO(hkhalil): Avoid copy of |block|.
  if (GetProtocolVersion() <= SPDY3) {
    SpdySynStreamIR syn_stream(stream_id);
    syn_stream.set_associated_to_stream_id(0);
    syn_stream.set_priority(spdy_priority);
    syn_stream.set_fin((flags & CONTROL_FLAG_FIN) != 0);
    syn_stream.set_unidirectional((flags & CONTROL_FLAG_UNIDIRECTIONAL) != 0);
    syn_stream.set_header_block(block);
    syn_frame.reset(buffered_spdy_framer_->SerializeFrame(syn_stream));

    if (net_log().IsCapturing()) {
      net_log().AddEvent(NetLog::TYPE_HTTP2_SESSION_SYN_STREAM,
                         base::Bind(&NetLogSpdySynStreamSentCallback, &block,
                                    (flags & CONTROL_FLAG_FIN) != 0,
                                    (flags & CONTROL_FLAG_UNIDIRECTIONAL) != 0,
                                    spdy_priority, stream_id));
    }
  } else {
    SpdyHeadersIR headers(stream_id);
    headers.set_priority(spdy_priority);
    headers.set_has_priority(true);

    if (send_priority_dependency_) {
      // Set dependencies to reflect request priority.  A newly created
      // stream should be dependent on the most recent previously created
      // stream of the same priority level.  The newly created stream
      // should also have all streams of a lower priority level dependent
      // on it, which is guaranteed by setting the exclusive bit.
      //
      // Note that this depends on stream ids being allocated in a monotonically
      // increasing fashion, and on all streams in
      // active_streams_{,by_priority_} having stream ids set.
      for (int i = priority; i >= IDLE; --i) {
        if (active_streams_by_priority_[i].empty())
          continue;

        auto candidate_it = active_streams_by_priority_[i].rbegin();

        // |active_streams_by_priority_| is updated before the
        // SYN stream frame is created, so the current streams
        // id is already on the list.  Skip over it, skipping this
        // priority level if it's singular.
        if (candidate_it->second->stream_id() == stream_id)
          ++candidate_it;
        if (candidate_it == active_streams_by_priority_[i].rend())
          continue;

        headers.set_parent_stream_id(candidate_it->second->stream_id());
        break;
      }

      // If there are no streams of priority <= the current stream, the
      // current stream will default to a child of the idle node (0).
      headers.set_exclusive(true);
    }

    headers.set_fin((flags & CONTROL_FLAG_FIN) != 0);
    headers.set_header_block(block);
    syn_frame.reset(buffered_spdy_framer_->SerializeFrame(headers));

    if (net_log().IsCapturing()) {
      net_log().AddEvent(
          NetLog::TYPE_HTTP2_SESSION_SEND_HEADERS,
          base::Bind(&NetLogSpdyHeadersSentCallback, &block,
                     (flags & CONTROL_FLAG_FIN) != 0, stream_id,
                     headers.has_priority(), headers.priority(),
                     headers.parent_stream_id(), headers.exclusive()));
    }
  }

  streams_initiated_count_++;

  return syn_frame;
}

scoped_ptr<SpdyBuffer> SpdySession::CreateDataBuffer(SpdyStreamId stream_id,
                                                     IOBuffer* data,
                                                     int len,
                                                     SpdyDataFlags flags) {
  if (availability_state_ == STATE_DRAINING) {
    return scoped_ptr<SpdyBuffer>();
  }

  ActiveStreamMap::const_iterator it = active_streams_.find(stream_id);
  CHECK(it != active_streams_.end());
  SpdyStream* stream = it->second.stream;
  CHECK_EQ(stream->stream_id(), stream_id);

  if (len < 0) {
    NOTREACHED();
    return scoped_ptr<SpdyBuffer>();
  }

  int effective_len = std::min(len, kMaxSpdyFrameChunkSize);

  bool send_stalled_by_stream = (stream->send_window_size() <= 0);
  bool send_stalled_by_session = IsSendStalled();

  // NOTE: There's an enum of the same name in histograms.xml.
  enum SpdyFrameFlowControlState {
    SEND_NOT_STALLED,
    SEND_STALLED_BY_STREAM,
    SEND_STALLED_BY_SESSION,
    SEND_STALLED_BY_STREAM_AND_SESSION,
  };

  SpdyFrameFlowControlState frame_flow_control_state = SEND_NOT_STALLED;
  if (send_stalled_by_stream) {
    if (send_stalled_by_session) {
      frame_flow_control_state = SEND_STALLED_BY_STREAM_AND_SESSION;
    } else {
      frame_flow_control_state = SEND_STALLED_BY_STREAM;
    }
  } else if (send_stalled_by_session) {
    frame_flow_control_state = SEND_STALLED_BY_SESSION;
  }

  UMA_HISTOGRAM_ENUMERATION("Net.SpdyFrameStreamAndSessionFlowControlState",
                            frame_flow_control_state,
                            SEND_STALLED_BY_STREAM_AND_SESSION + 1);

  // Obey send window size of the stream.
  if (send_stalled_by_stream) {
    stream->set_send_stalled_by_flow_control(true);
    // Even though we're currently stalled only by the stream, we
    // might end up being stalled by the session also.
    QueueSendStalledStream(*stream);
    net_log().AddEvent(
        NetLog::TYPE_HTTP2_SESSION_STREAM_STALLED_BY_STREAM_SEND_WINDOW,
        NetLog::IntCallback("stream_id", stream_id));
    return scoped_ptr<SpdyBuffer>();
  }

  effective_len = std::min(effective_len, stream->send_window_size());

  // Obey send window size of the session.
  if (send_stalled_by_session) {
    stream->set_send_stalled_by_flow_control(true);
    QueueSendStalledStream(*stream);
    net_log().AddEvent(
        NetLog::TYPE_HTTP2_SESSION_STREAM_STALLED_BY_SESSION_SEND_WINDOW,
        NetLog::IntCallback("stream_id", stream_id));
    return scoped_ptr<SpdyBuffer>();
  }

  effective_len = std::min(effective_len, session_send_window_size_);

  DCHECK_GE(effective_len, 0);

  // Clear FIN flag if only some of the data will be in the data
  // frame.
  if (effective_len < len)
    flags = static_cast<SpdyDataFlags>(flags & ~DATA_FLAG_FIN);

  if (net_log().IsCapturing()) {
    net_log().AddEvent(NetLog::TYPE_HTTP2_SESSION_SEND_DATA,
                       base::Bind(&NetLogSpdyDataCallback, stream_id,
                                  effective_len, (flags & DATA_FLAG_FIN) != 0));
  }

  // Send PrefacePing for DATA_FRAMEs with nonzero payload size.
  if (effective_len > 0)
    SendPrefacePingIfNoneInFlight();

  // TODO(mbelshe): reduce memory copies here.
  DCHECK(buffered_spdy_framer_.get());
  scoped_ptr<SpdyFrame> frame(buffered_spdy_framer_->CreateDataFrame(
      stream_id, data->data(), static_cast<uint32_t>(effective_len), flags));

  scoped_ptr<SpdyBuffer> data_buffer(new SpdyBuffer(std::move(frame)));

  // Send window size is based on payload size, so nothing to do if this is
  // just a FIN with no payload.
  if (effective_len != 0) {
    DecreaseSendWindowSize(static_cast<int32_t>(effective_len));
    data_buffer->AddConsumeCallback(
        base::Bind(&SpdySession::OnWriteBufferConsumed,
                   weak_factory_.GetWeakPtr(),
                   static_cast<size_t>(effective_len)));
  }

  return data_buffer;
}

void SpdySession::CloseActiveStream(SpdyStreamId stream_id, int status) {
  DCHECK_NE(stream_id, 0u);

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end()) {
    NOTREACHED();
    return;
  }

  CloseActiveStreamIterator(it, status);
}

void SpdySession::CloseCreatedStream(
    const base::WeakPtr<SpdyStream>& stream, int status) {
  DCHECK_EQ(stream->stream_id(), 0u);

  CreatedStreamSet::iterator it = created_streams_.find(stream.get());
  if (it == created_streams_.end()) {
    NOTREACHED();
    return;
  }

  CloseCreatedStreamIterator(it, status);
}

void SpdySession::ResetStream(SpdyStreamId stream_id,
                              SpdyRstStreamStatus status,
                              const std::string& description) {
  DCHECK_NE(stream_id, 0u);

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end()) {
    NOTREACHED();
    return;
  }

  ResetStreamIterator(it, status, description);
}

bool SpdySession::IsStreamActive(SpdyStreamId stream_id) const {
  return ContainsKey(active_streams_, stream_id);
}

LoadState SpdySession::GetLoadState() const {
  // Just report that we're idle since the session could be doing
  // many things concurrently.
  return LOAD_STATE_IDLE;
}

void SpdySession::CloseActiveStreamIterator(ActiveStreamMap::iterator it,
                                            int status) {
  // TODO(mbelshe): We should send a RST_STREAM control frame here
  //                so that the server can cancel a large send.

  scoped_ptr<SpdyStream> owned_stream(it->second.stream);
  active_streams_.erase(it);
  active_streams_by_priority_[owned_stream->priority()].erase(
      owned_stream->stream_id());

  // TODO(akalin): When SpdyStream was ref-counted (and
  // |unclaimed_pushed_streams_| held scoped_refptr<SpdyStream>), this
  // was only done when status was not OK. This meant that pushed
  // streams can still be claimed after they're closed. This is
  // probably something that we still want to support, although server
  // push is hardly used. Write tests for this and fix this. (See
  // http://crbug.com/261712 .)
  if (owned_stream->type() == SPDY_PUSH_STREAM) {
    unclaimed_pushed_streams_.erase(owned_stream->url());
    num_pushed_streams_--;
    if (!owned_stream->IsReservedRemote())
      num_active_pushed_streams_--;
  }

  DeleteStream(std::move(owned_stream), status);

  // If there are no active streams and the socket pool is stalled, close the
  // session to free up a socket slot.
  if (active_streams_.empty() && created_streams_.empty() &&
      connection_->IsPoolStalled()) {
    DoDrainSession(ERR_CONNECTION_CLOSED, "Closing idle connection.");
  }
}

void SpdySession::CloseCreatedStreamIterator(CreatedStreamSet::iterator it,
                                             int status) {
  scoped_ptr<SpdyStream> owned_stream(*it);
  created_streams_.erase(it);
  DeleteStream(std::move(owned_stream), status);
}

void SpdySession::ResetStreamIterator(ActiveStreamMap::iterator it,
                                      SpdyRstStreamStatus status,
                                      const std::string& description) {
  // Send the RST_STREAM frame first as CloseActiveStreamIterator()
  // may close us.
  SpdyStreamId stream_id = it->first;
  RequestPriority priority = it->second.stream->priority();
  EnqueueResetStreamFrame(stream_id, priority, status, description);

  // Removes any pending writes for the stream except for possibly an
  // in-flight one.
  CloseActiveStreamIterator(it, ERR_SPDY_PROTOCOL_ERROR);
}

void SpdySession::EnqueueResetStreamFrame(SpdyStreamId stream_id,
                                          RequestPriority priority,
                                          SpdyRstStreamStatus status,
                                          const std::string& description) {
  DCHECK_NE(stream_id, 0u);

  net_log().AddEvent(
      NetLog::TYPE_HTTP2_SESSION_SEND_RST_STREAM,
      base::Bind(&NetLogSpdyRstCallback, stream_id, status, &description));

  DCHECK(buffered_spdy_framer_.get());
  scoped_ptr<SpdyFrame> rst_frame(
      buffered_spdy_framer_->CreateRstStream(stream_id, status));

  EnqueueSessionWrite(priority, RST_STREAM, std::move(rst_frame));
  RecordProtocolErrorHistogram(MapRstStreamStatusToProtocolError(status));
}

void SpdySession::PumpReadLoop(ReadState expected_read_state, int result) {
  // TODO(bnc): Remove ScopedTracker below once crbug.com/462774 is fixed.
  tracked_objects::ScopedTracker tracking_profile(
      FROM_HERE_WITH_EXPLICIT_FUNCTION("462774 SpdySession::PumpReadLoop"));

  CHECK(!in_io_loop_);
  if (availability_state_ == STATE_DRAINING) {
    return;
  }
  ignore_result(DoReadLoop(expected_read_state, result));
}

int SpdySession::DoReadLoop(ReadState expected_read_state, int result) {
  CHECK(!in_io_loop_);
  CHECK_EQ(read_state_, expected_read_state);

  in_io_loop_ = true;

  int bytes_read_without_yielding = 0;
  const base::TimeTicks yield_after_time =
      time_func_() +
      base::TimeDelta::FromMilliseconds(kYieldAfterDurationMilliseconds);

  // Loop until the session is draining, the read becomes blocked, or
  // the read limit is exceeded.
  while (true) {
    switch (read_state_) {
      case READ_STATE_DO_READ:
        CHECK_EQ(result, OK);
        result = DoRead();
        break;
      case READ_STATE_DO_READ_COMPLETE:
        if (result > 0)
          bytes_read_without_yielding += result;
        result = DoReadComplete(result);
        break;
      default:
        NOTREACHED() << "read_state_: " << read_state_;
        break;
    }

    if (availability_state_ == STATE_DRAINING)
      break;

    if (result == ERR_IO_PENDING)
      break;

    if (read_state_ == READ_STATE_DO_READ &&
        (bytes_read_without_yielding > kYieldAfterBytesRead ||
         time_func_() > yield_after_time)) {
      base::ThreadTaskRunnerHandle::Get()->PostTask(
          FROM_HERE,
          base::Bind(&SpdySession::PumpReadLoop, weak_factory_.GetWeakPtr(),
                     READ_STATE_DO_READ, OK));
      result = ERR_IO_PENDING;
      break;
    }
  }

  CHECK(in_io_loop_);
  in_io_loop_ = false;

  return result;
}

int SpdySession::DoRead() {
  CHECK(in_io_loop_);

  CHECK(connection_);
  CHECK(connection_->socket());
  read_state_ = READ_STATE_DO_READ_COMPLETE;
  return connection_->socket()->Read(
      read_buffer_.get(),
      kReadBufferSize,
      base::Bind(&SpdySession::PumpReadLoop,
                 weak_factory_.GetWeakPtr(), READ_STATE_DO_READ_COMPLETE));
}

int SpdySession::DoReadComplete(int result) {
  CHECK(in_io_loop_);

  // Parse a frame.  For now this code requires that the frame fit into our
  // buffer (kReadBufferSize).
  // TODO(mbelshe): support arbitrarily large frames!

  if (result == 0) {
    UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySession.BytesRead.EOF",
                                total_bytes_received_, 1, 100000000, 50);
    DoDrainSession(ERR_CONNECTION_CLOSED, "Connection closed");

    return ERR_CONNECTION_CLOSED;
  }

  if (result < 0) {
    DoDrainSession(
        static_cast<Error>(result),
        base::StringPrintf("Error %d reading from socket.", -result));
    return result;
  }
  CHECK_LE(result, kReadBufferSize);
  total_bytes_received_ += result;

  last_activity_time_ = time_func_();

  DCHECK(buffered_spdy_framer_.get());
  char* data = read_buffer_->data();
  while (result > 0) {
    uint32_t bytes_processed =
        buffered_spdy_framer_->ProcessInput(data, result);
    result -= bytes_processed;
    data += bytes_processed;

    if (availability_state_ == STATE_DRAINING) {
      return ERR_CONNECTION_CLOSED;
    }

    DCHECK_EQ(buffered_spdy_framer_->error_code(), SpdyFramer::SPDY_NO_ERROR);
  }

  read_state_ = READ_STATE_DO_READ;
  return OK;
}

void SpdySession::PumpWriteLoop(WriteState expected_write_state, int result) {
  CHECK(!in_io_loop_);
  DCHECK_EQ(write_state_, expected_write_state);

  DoWriteLoop(expected_write_state, result);

  if (availability_state_ == STATE_DRAINING && !in_flight_write_ &&
      write_queue_.IsEmpty()) {
    pool_->RemoveUnavailableSession(GetWeakPtr());  // Destroys |this|.
    return;
  }
}

int SpdySession::DoWriteLoop(WriteState expected_write_state, int result) {
  CHECK(!in_io_loop_);
  DCHECK_NE(write_state_, WRITE_STATE_IDLE);
  DCHECK_EQ(write_state_, expected_write_state);

  in_io_loop_ = true;

  // Loop until the session is closed or the write becomes blocked.
  while (true) {
    switch (write_state_) {
      case WRITE_STATE_DO_WRITE:
        DCHECK_EQ(result, OK);
        result = DoWrite();
        break;
      case WRITE_STATE_DO_WRITE_COMPLETE:
        result = DoWriteComplete(result);
        break;
      case WRITE_STATE_IDLE:
      default:
        NOTREACHED() << "write_state_: " << write_state_;
        break;
    }

    if (write_state_ == WRITE_STATE_IDLE) {
      DCHECK_EQ(result, ERR_IO_PENDING);
      break;
    }

    if (result == ERR_IO_PENDING)
      break;
  }

  CHECK(in_io_loop_);
  in_io_loop_ = false;

  return result;
}

int SpdySession::DoWrite() {
  CHECK(in_io_loop_);

  DCHECK(buffered_spdy_framer_);
  if (in_flight_write_) {
    DCHECK_GT(in_flight_write_->GetRemainingSize(), 0u);
  } else {
    // Grab the next frame to send.
    SpdyFrameType frame_type = DATA;
    scoped_ptr<SpdyBufferProducer> producer;
    base::WeakPtr<SpdyStream> stream;
    if (!write_queue_.Dequeue(&frame_type, &producer, &stream)) {
      write_state_ = WRITE_STATE_IDLE;
      return ERR_IO_PENDING;
    }

    if (stream.get())
      CHECK(!stream->IsClosed());

    // Activate the stream only when sending the SYN_STREAM frame to
    // guarantee monotonically-increasing stream IDs.
    if (frame_type == SYN_STREAM) {
      CHECK(stream.get());
      CHECK_EQ(stream->stream_id(), 0u);
      scoped_ptr<SpdyStream> owned_stream =
          ActivateCreatedStream(stream.get());
      InsertActivatedStream(std::move(owned_stream));

      if (stream_hi_water_mark_ > kLastStreamId) {
        CHECK_EQ(stream->stream_id(), kLastStreamId);
        // We've exhausted the stream ID space, and no new streams may be
        // created after this one.
        MakeUnavailable();
        StartGoingAway(kLastStreamId, ERR_ABORTED);
      }
    }

    // TODO(pkasting): Remove ScopedTracker below once crbug.com/457517 is
    // fixed.
    tracked_objects::ScopedTracker tracking_profile1(
        FROM_HERE_WITH_EXPLICIT_FUNCTION("457517 SpdySession::DoWrite1"));
    in_flight_write_ = producer->ProduceBuffer();
    if (!in_flight_write_) {
      NOTREACHED();
      return ERR_UNEXPECTED;
    }
    in_flight_write_frame_type_ = frame_type;
    in_flight_write_frame_size_ = in_flight_write_->GetRemainingSize();
    DCHECK_GE(in_flight_write_frame_size_,
              buffered_spdy_framer_->GetFrameMinimumSize());
    in_flight_write_stream_ = stream;
  }

  write_state_ = WRITE_STATE_DO_WRITE_COMPLETE;

  // Explicitly store in a scoped_refptr<IOBuffer> to avoid problems
  // with Socket implementations that don't store their IOBuffer
  // argument in a scoped_refptr<IOBuffer> (see crbug.com/232345).
  // TODO(pkasting): Remove ScopedTracker below once crbug.com/457517 is fixed.
  tracked_objects::ScopedTracker tracking_profile2(
      FROM_HERE_WITH_EXPLICIT_FUNCTION("457517 SpdySession::DoWrite2"));
  scoped_refptr<IOBuffer> write_io_buffer =
      in_flight_write_->GetIOBufferForRemainingData();
  return connection_->socket()->Write(
      write_io_buffer.get(),
      in_flight_write_->GetRemainingSize(),
      base::Bind(&SpdySession::PumpWriteLoop,
                 weak_factory_.GetWeakPtr(), WRITE_STATE_DO_WRITE_COMPLETE));
}

int SpdySession::DoWriteComplete(int result) {
  CHECK(in_io_loop_);
  DCHECK_NE(result, ERR_IO_PENDING);
  DCHECK_GT(in_flight_write_->GetRemainingSize(), 0u);

  last_activity_time_ = time_func_();

  if (result < 0) {
    DCHECK_NE(result, ERR_IO_PENDING);
    in_flight_write_.reset();
    in_flight_write_frame_type_ = DATA;
    in_flight_write_frame_size_ = 0;
    in_flight_write_stream_.reset();
    write_state_ = WRITE_STATE_DO_WRITE;
    DoDrainSession(static_cast<Error>(result), "Write error");
    return OK;
  }

  // It should not be possible to have written more bytes than our
  // in_flight_write_.
  DCHECK_LE(static_cast<size_t>(result),
            in_flight_write_->GetRemainingSize());

  if (result > 0) {
    in_flight_write_->Consume(static_cast<size_t>(result));
    if (in_flight_write_stream_.get())
      in_flight_write_stream_->AddRawSentBytes(static_cast<size_t>(result));

    // We only notify the stream when we've fully written the pending frame.
    if (in_flight_write_->GetRemainingSize() == 0) {
      // It is possible that the stream was cancelled while we were
      // writing to the socket.
      if (in_flight_write_stream_.get()) {
        DCHECK_GT(in_flight_write_frame_size_, 0u);
        in_flight_write_stream_->OnFrameWriteComplete(
            in_flight_write_frame_type_,
            in_flight_write_frame_size_);
      }

      // Cleanup the write which just completed.
      in_flight_write_.reset();
      in_flight_write_frame_type_ = DATA;
      in_flight_write_frame_size_ = 0;
      in_flight_write_stream_.reset();
    }
  }

  write_state_ = WRITE_STATE_DO_WRITE;
  return OK;
}

void SpdySession::DcheckGoingAway() const {
#if DCHECK_IS_ON()
  DCHECK_GE(availability_state_, STATE_GOING_AWAY);
  for (int i = MINIMUM_PRIORITY; i <= MAXIMUM_PRIORITY; ++i) {
    DCHECK(pending_create_stream_queues_[i].empty());
  }
  DCHECK(created_streams_.empty());
#endif
}

void SpdySession::DcheckDraining() const {
  DcheckGoingAway();
  DCHECK_EQ(availability_state_, STATE_DRAINING);
  DCHECK(active_streams_.empty());
  DCHECK(unclaimed_pushed_streams_.empty());
}

void SpdySession::StartGoingAway(SpdyStreamId last_good_stream_id,
                                 Error status) {
  DCHECK_GE(availability_state_, STATE_GOING_AWAY);

  // The loops below are carefully written to avoid reentrancy problems.

  while (true) {
    size_t old_size = GetTotalSize(pending_create_stream_queues_);
    base::WeakPtr<SpdyStreamRequest> pending_request =
        GetNextPendingStreamRequest();
    if (!pending_request)
      break;
    // No new stream requests should be added while the session is
    // going away.
    DCHECK_GT(old_size, GetTotalSize(pending_create_stream_queues_));
    pending_request->OnRequestCompleteFailure(ERR_ABORTED);
  }

  while (true) {
    size_t old_size = active_streams_.size();
    ActiveStreamMap::iterator it =
        active_streams_.lower_bound(last_good_stream_id + 1);
    if (it == active_streams_.end())
      break;
    LogAbandonedActiveStream(it, status);
    CloseActiveStreamIterator(it, status);
    // No new streams should be activated while the session is going
    // away.
    DCHECK_GT(old_size, active_streams_.size());
  }

  while (!created_streams_.empty()) {
    size_t old_size = created_streams_.size();
    CreatedStreamSet::iterator it = created_streams_.begin();
    LogAbandonedStream(*it, status);
    CloseCreatedStreamIterator(it, status);
    // No new streams should be created while the session is going
    // away.
    DCHECK_GT(old_size, created_streams_.size());
  }

  write_queue_.RemovePendingWritesForStreamsAfter(last_good_stream_id);

  DcheckGoingAway();
  MaybeFinishGoingAway();
}

void SpdySession::MaybeFinishGoingAway() {
  if (active_streams_.empty() && created_streams_.empty() &&
      availability_state_ == STATE_GOING_AWAY) {
    DoDrainSession(OK, "Finished going away");
  }
}

void SpdySession::DoDrainSession(Error err, const std::string& description) {
  if (availability_state_ == STATE_DRAINING) {
    return;
  }
  MakeUnavailable();

  // Mark host_port_pair requiring HTTP/1.1 for subsequent connections.
  if (err == ERR_HTTP_1_1_REQUIRED) {
    http_server_properties_->SetHTTP11Required(host_port_pair());
  }

  // If |err| indicates an error occurred, inform the peer that we're closing
  // and why. Don't GOAWAY on a graceful or idle close, as that may
  // unnecessarily wake the radio. We could technically GOAWAY on network errors
  // (we'll probably fail to actually write it, but that's okay), however many
  // unit-tests would need to be updated.
  if (err != OK &&
      err != ERR_ABORTED &&  // Used by SpdySessionPool to close idle sessions.
      err != ERR_NETWORK_CHANGED &&  // Used to deprecate sessions on IP change.
      err != ERR_SOCKET_NOT_CONNECTED && err != ERR_HTTP_1_1_REQUIRED &&
      err != ERR_CONNECTION_CLOSED && err != ERR_CONNECTION_RESET) {
    // Enqueue a GOAWAY to inform the peer of why we're closing the connection.
    SpdyGoAwayIR goaway_ir(last_accepted_push_stream_id_,
                           MapNetErrorToGoAwayStatus(err),
                           description);
    EnqueueSessionWrite(HIGHEST,
                        GOAWAY,
                        scoped_ptr<SpdyFrame>(
                            buffered_spdy_framer_->SerializeFrame(goaway_ir)));
  }

  availability_state_ = STATE_DRAINING;
  error_on_close_ = err;

  net_log_.AddEvent(
      NetLog::TYPE_HTTP2_SESSION_CLOSE,
      base::Bind(&NetLogSpdySessionCloseCallback, err, &description));

  UMA_HISTOGRAM_SPARSE_SLOWLY("Net.SpdySession.ClosedOnError", -err);
  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySession.BytesRead.OtherErrors",
                              total_bytes_received_, 1, 100000000, 50);

  if (err == OK) {
    // We ought to be going away already, as this is a graceful close.
    DcheckGoingAway();
  } else {
    StartGoingAway(0, err);
  }
  DcheckDraining();
  MaybePostWriteLoop();
}

void SpdySession::LogAbandonedStream(SpdyStream* stream, Error status) {
  DCHECK(stream);
  std::string description = base::StringPrintf(
      "ABANDONED (stream_id=%d): ", stream->stream_id()) +
      stream->url().spec();
  stream->LogStreamError(status, description);
  // We don't increment the streams abandoned counter here. If the
  // stream isn't active (i.e., it hasn't written anything to the wire
  // yet) then it's as if it never existed. If it is active, then
  // LogAbandonedActiveStream() will increment the counters.
}

void SpdySession::LogAbandonedActiveStream(ActiveStreamMap::const_iterator it,
                                           Error status) {
  DCHECK_GT(it->first, 0u);
  LogAbandonedStream(it->second.stream, status);
  ++streams_abandoned_count_;
  if (it->second.stream->type() == SPDY_PUSH_STREAM &&
      unclaimed_pushed_streams_.find(it->second.stream->url()) !=
      unclaimed_pushed_streams_.end()) {
  }
}

SpdyStreamId SpdySession::GetNewStreamId() {
  CHECK_LE(stream_hi_water_mark_, kLastStreamId);
  SpdyStreamId id = stream_hi_water_mark_;
  stream_hi_water_mark_ += 2;
  return id;
}

void SpdySession::CloseSessionOnError(Error err,
                                      const std::string& description) {
  DCHECK_LT(err, ERR_IO_PENDING);
  DoDrainSession(err, description);
}

void SpdySession::MakeUnavailable() {
  if (availability_state_ == STATE_AVAILABLE) {
    availability_state_ = STATE_GOING_AWAY;
    pool_->MakeSessionUnavailable(GetWeakPtr());
  }
}

scoped_ptr<base::Value> SpdySession::GetInfoAsValue() const {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());

  dict->SetInteger("source_id", net_log_.source().id);

  dict->SetString("host_port_pair", host_port_pair().ToString());
  if (!pooled_aliases_.empty()) {
    scoped_ptr<base::ListValue> alias_list(new base::ListValue());
    for (const auto& alias : pooled_aliases_) {
      alias_list->AppendString(alias.host_port_pair().ToString());
    }
    dict->Set("aliases", std::move(alias_list));
  }
  dict->SetString("proxy", host_port_proxy_pair().second.ToURI());

  dict->SetInteger("active_streams", active_streams_.size());

  dict->SetInteger("unclaimed_pushed_streams",
                   unclaimed_pushed_streams_.size());

  dict->SetBoolean("is_secure", is_secure_);

  dict->SetString("protocol_negotiated",
                  SSLClientSocket::NextProtoToString(
                      connection_->socket()->GetNegotiatedProtocol()));

  dict->SetInteger("error", error_on_close_);
  dict->SetInteger("max_concurrent_streams", max_concurrent_streams_);

  dict->SetInteger("streams_initiated_count", streams_initiated_count_);
  dict->SetInteger("streams_pushed_count", streams_pushed_count_);
  dict->SetInteger("streams_pushed_and_claimed_count",
      streams_pushed_and_claimed_count_);
  dict->SetInteger("streams_abandoned_count", streams_abandoned_count_);
  DCHECK(buffered_spdy_framer_.get());
  dict->SetInteger("frames_received", buffered_spdy_framer_->frames_received());

  dict->SetBoolean("sent_settings", sent_settings_);
  dict->SetBoolean("received_settings", received_settings_);

  dict->SetInteger("send_window_size", session_send_window_size_);
  dict->SetInteger("recv_window_size", session_recv_window_size_);
  dict->SetInteger("unacked_recv_window_bytes",
                   session_unacked_recv_window_bytes_);
  return std::move(dict);
}

bool SpdySession::IsReused() const {
  return buffered_spdy_framer_->frames_received() > 0 ||
      connection_->reuse_type() == ClientSocketHandle::UNUSED_IDLE;
}

bool SpdySession::GetLoadTimingInfo(SpdyStreamId stream_id,
                                    LoadTimingInfo* load_timing_info) const {
  return connection_->GetLoadTimingInfo(stream_id != kFirstStreamId,
                                        load_timing_info);
}

int SpdySession::GetPeerAddress(IPEndPoint* address) const {
  int rv = ERR_SOCKET_NOT_CONNECTED;
  if (connection_->socket()) {
    rv = connection_->socket()->GetPeerAddress(address);
  }

  UMA_HISTOGRAM_BOOLEAN("Net.SpdySessionSocketNotConnectedGetPeerAddress",
                        rv == ERR_SOCKET_NOT_CONNECTED);

  return rv;
}

int SpdySession::GetLocalAddress(IPEndPoint* address) const {
  int rv = ERR_SOCKET_NOT_CONNECTED;
  if (connection_->socket()) {
    rv = connection_->socket()->GetLocalAddress(address);
  }

  UMA_HISTOGRAM_BOOLEAN("Net.SpdySessionSocketNotConnectedGetLocalAddress",
                        rv == ERR_SOCKET_NOT_CONNECTED);

  return rv;
}

void SpdySession::EnqueueSessionWrite(RequestPriority priority,
                                      SpdyFrameType frame_type,
                                      scoped_ptr<SpdyFrame> frame) {
  DCHECK(frame_type == RST_STREAM || frame_type == SETTINGS ||
         frame_type == WINDOW_UPDATE || frame_type == PING ||
         frame_type == GOAWAY);
  EnqueueWrite(priority, frame_type,
               scoped_ptr<SpdyBufferProducer>(new SimpleBufferProducer(
                   scoped_ptr<SpdyBuffer>(new SpdyBuffer(std::move(frame))))),
               base::WeakPtr<SpdyStream>());
}

void SpdySession::EnqueueWrite(RequestPriority priority,
                               SpdyFrameType frame_type,
                               scoped_ptr<SpdyBufferProducer> producer,
                               const base::WeakPtr<SpdyStream>& stream) {
  if (availability_state_ == STATE_DRAINING)
    return;

  write_queue_.Enqueue(priority, frame_type, std::move(producer), stream);
  MaybePostWriteLoop();
}

void SpdySession::MaybePostWriteLoop() {
  if (write_state_ == WRITE_STATE_IDLE) {
    CHECK(!in_flight_write_);
    write_state_ = WRITE_STATE_DO_WRITE;
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE,
        base::Bind(&SpdySession::PumpWriteLoop, weak_factory_.GetWeakPtr(),
                   WRITE_STATE_DO_WRITE, OK));
  }
}

void SpdySession::InsertCreatedStream(scoped_ptr<SpdyStream> stream) {
  CHECK_EQ(stream->stream_id(), 0u);
  CHECK(created_streams_.find(stream.get()) == created_streams_.end());
  created_streams_.insert(stream.release());
}

scoped_ptr<SpdyStream> SpdySession::ActivateCreatedStream(SpdyStream* stream) {
  CHECK_EQ(stream->stream_id(), 0u);
  CHECK(created_streams_.find(stream) != created_streams_.end());
  stream->set_stream_id(GetNewStreamId());
  scoped_ptr<SpdyStream> owned_stream(stream);
  created_streams_.erase(stream);
  return owned_stream;
}

void SpdySession::InsertActivatedStream(scoped_ptr<SpdyStream> stream) {
  SpdyStreamId stream_id = stream->stream_id();
  CHECK_NE(stream_id, 0u);
  std::pair<ActiveStreamMap::iterator, bool> result =
      active_streams_.insert(
          std::make_pair(stream_id, ActiveStreamInfo(stream.get())));
  active_streams_by_priority_[stream->priority()].insert(
      std::make_pair(stream_id, stream.get()));
  CHECK(result.second);
  ignore_result(stream.release());
}

void SpdySession::DeleteStream(scoped_ptr<SpdyStream> stream, int status) {
  if (in_flight_write_stream_.get() == stream.get()) {
    // If we're deleting the stream for the in-flight write, we still
    // need to let the write complete, so we clear
    // |in_flight_write_stream_| and let the write finish on its own
    // without notifying |in_flight_write_stream_|.
    in_flight_write_stream_.reset();
  }

  write_queue_.RemovePendingWritesForStream(stream->GetWeakPtr());
  stream->OnClose(status);

  if (availability_state_ == STATE_AVAILABLE) {
    ProcessPendingStreamRequests();
  }
}

base::WeakPtr<SpdyStream> SpdySession::GetActivePushStream(const GURL& url) {
  PushedStreamMap::iterator unclaimed_it = unclaimed_pushed_streams_.find(url);
  if (unclaimed_it == unclaimed_pushed_streams_.end())
    return base::WeakPtr<SpdyStream>();

  SpdyStreamId stream_id = unclaimed_it->second.stream_id;
  unclaimed_pushed_streams_.erase(unclaimed_it);

  ActiveStreamMap::iterator active_it = active_streams_.find(stream_id);
  if (active_it == active_streams_.end()) {
    NOTREACHED();
    return base::WeakPtr<SpdyStream>();
  }

  net_log_.AddEvent(NetLog::TYPE_HTTP2_STREAM_ADOPTED_PUSH_STREAM,
                    base::Bind(&NetLogSpdyAdoptedPushStreamCallback,
                               active_it->second.stream->stream_id(), &url));
  return active_it->second.stream->GetWeakPtr();
}

bool SpdySession::GetSSLInfo(SSLInfo* ssl_info,
                             bool* was_npn_negotiated,
                             NextProto* protocol_negotiated) {
  *was_npn_negotiated = connection_->socket()->WasNpnNegotiated();
  *protocol_negotiated = connection_->socket()->GetNegotiatedProtocol();
  return connection_->socket()->GetSSLInfo(ssl_info);
}

Error SpdySession::GetSignedEKMForTokenBinding(crypto::ECPrivateKey* key,
                                               std::vector<uint8_t>* out) {
  if (!is_secure_) {
    NOTREACHED();
    return ERR_FAILED;
  }
  SSLClientSocket* ssl_socket =
      static_cast<SSLClientSocket*>(connection_->socket());
  return ssl_socket->GetSignedEKMForTokenBinding(key, out);
}

void SpdySession::OnError(SpdyFramer::SpdyError error_code) {
  CHECK(in_io_loop_);

  RecordProtocolErrorHistogram(MapFramerErrorToProtocolError(error_code));
  std::string description =
      base::StringPrintf("Framer error: %d (%s).",
                         error_code,
                         SpdyFramer::ErrorCodeToString(error_code));
  DoDrainSession(MapFramerErrorToNetError(error_code), description);
}

void SpdySession::OnStreamError(SpdyStreamId stream_id,
                                const std::string& description) {
  CHECK(in_io_loop_);

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end()) {
    // We still want to send a frame to reset the stream even if we
    // don't know anything about it.
    EnqueueResetStreamFrame(
        stream_id, IDLE, RST_STREAM_PROTOCOL_ERROR, description);
    return;
  }

  ResetStreamIterator(it, RST_STREAM_PROTOCOL_ERROR, description);
}

void SpdySession::OnDataFrameHeader(SpdyStreamId stream_id,
                                    size_t length,
                                    bool fin) {
  CHECK(in_io_loop_);

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);

  // By the time data comes in, the stream may already be inactive.
  if (it == active_streams_.end())
    return;

  SpdyStream* stream = it->second.stream;
  CHECK_EQ(stream->stream_id(), stream_id);

  DCHECK(buffered_spdy_framer_);
  size_t header_len = buffered_spdy_framer_->GetDataFrameMinimumSize();
  stream->AddRawReceivedBytes(header_len);
}

void SpdySession::OnStreamFrameData(SpdyStreamId stream_id,
                                    const char* data,
                                    size_t len,
                                    bool fin) {
  CHECK(in_io_loop_);
  DCHECK_LT(len, 1u << 24);
  if (net_log().IsCapturing()) {
    net_log().AddEvent(
        NetLog::TYPE_HTTP2_SESSION_RECV_DATA,
        base::Bind(&NetLogSpdyDataCallback, stream_id, len, fin));
  }

  // Build the buffer as early as possible so that we go through the
  // session flow control checks and update
  // |unacked_recv_window_bytes_| properly even when the stream is
  // inactive (since the other side has still reduced its session send
  // window).
  scoped_ptr<SpdyBuffer> buffer;
  if (data) {
    DCHECK_GT(len, 0u);
    CHECK_LE(len, static_cast<size_t>(kReadBufferSize));
    buffer.reset(new SpdyBuffer(data, len));

    DecreaseRecvWindowSize(static_cast<int32_t>(len));
    buffer->AddConsumeCallback(base::Bind(&SpdySession::OnReadBufferConsumed,
                                          weak_factory_.GetWeakPtr()));
  } else {
    DCHECK_EQ(len, 0u);
  }

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);

  // By the time data comes in, the stream may already be inactive.
  if (it == active_streams_.end())
    return;

  SpdyStream* stream = it->second.stream;
  CHECK_EQ(stream->stream_id(), stream_id);

  stream->AddRawReceivedBytes(len);

  if (it->second.waiting_for_syn_reply) {
    const std::string& error = "Data received before SYN_REPLY.";
    stream->LogStreamError(ERR_SPDY_PROTOCOL_ERROR, error);
    ResetStreamIterator(it, RST_STREAM_PROTOCOL_ERROR, error);
    return;
  }

  stream->OnDataReceived(std::move(buffer));
}

void SpdySession::OnStreamPadding(SpdyStreamId stream_id, size_t len) {
  CHECK(in_io_loop_);

  // Decrease window size because padding bytes are received.
  // Increase window size because padding bytes are consumed (by discarding).
  // Net result: |session_unacked_recv_window_bytes_| increases by |len|,
  // |session_recv_window_size_| does not change.
  DecreaseRecvWindowSize(static_cast<int32_t>(len));
  IncreaseRecvWindowSize(static_cast<int32_t>(len));

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end())
    return;
  it->second.stream->OnPaddingConsumed(len);
}

SpdyHeadersHandlerInterface* SpdySession::OnHeaderFrameStart(
    SpdyStreamId stream_id) {
  LOG(FATAL);
  return nullptr;
}

void SpdySession::OnHeaderFrameEnd(SpdyStreamId stream_id, bool end_headers) {
  LOG(FATAL);
}

void SpdySession::OnSettings(bool clear_persisted) {
  CHECK(in_io_loop_);

  if (clear_persisted)
    http_server_properties_->ClearSpdySettings(host_port_pair());

  if (net_log_.IsCapturing()) {
    net_log_.AddEvent(NetLog::TYPE_HTTP2_SESSION_RECV_SETTINGS,
                      base::Bind(&NetLogSpdySettingsCallback, host_port_pair(),
                                 clear_persisted));
  }

  if (GetProtocolVersion() >= HTTP2) {
    // Send an acknowledgment of the setting.
    SpdySettingsIR settings_ir;
    settings_ir.set_is_ack(true);
    EnqueueSessionWrite(
        HIGHEST,
        SETTINGS,
        scoped_ptr<SpdyFrame>(
            buffered_spdy_framer_->SerializeFrame(settings_ir)));
  }
}

void SpdySession::OnSetting(SpdySettingsIds id, uint8_t flags, uint32_t value) {
  CHECK(in_io_loop_);

  HandleSetting(id, value);
  http_server_properties_->SetSpdySetting(
      host_port_pair(),
      id,
      static_cast<SpdySettingsFlags>(flags),
      value);
  received_settings_ = true;

  // Log the setting.
  const SpdyMajorVersion protocol_version = GetProtocolVersion();
  net_log_.AddEvent(NetLog::TYPE_HTTP2_SESSION_RECV_SETTING,
                    base::Bind(&NetLogSpdySettingCallback, id, protocol_version,
                               static_cast<SpdySettingsFlags>(flags), value));
}

void SpdySession::OnSendCompressedFrame(
    SpdyStreamId stream_id,
    SpdyFrameType type,
    size_t payload_len,
    size_t frame_len) {
  if (type != SYN_STREAM && type != HEADERS)
    return;

  DCHECK(buffered_spdy_framer_.get());
  size_t compressed_len =
      frame_len - buffered_spdy_framer_->GetSynStreamMinimumSize();

  if (payload_len) {
    // Make sure we avoid early decimal truncation.
    int compression_pct = 100 - (100 * compressed_len) / payload_len;
    UMA_HISTOGRAM_PERCENTAGE("Net.SpdySynStreamCompressionPercentage",
                             compression_pct);
  }
}

void SpdySession::OnReceiveCompressedFrame(
    SpdyStreamId stream_id,
    SpdyFrameType type,
    size_t frame_len) {
  last_compressed_frame_len_ = frame_len;
}

int SpdySession::OnInitialResponseHeadersReceived(
    const SpdyHeaderBlock& response_headers,
    base::Time response_time,
    base::TimeTicks recv_first_byte_time,
    SpdyStream* stream) {
  CHECK(in_io_loop_);
  SpdyStreamId stream_id = stream->stream_id();

  if (stream->type() == SPDY_PUSH_STREAM) {
    DCHECK(stream->IsReservedRemote());
    if (max_concurrent_pushed_streams_ &&
        num_active_pushed_streams_ >= max_concurrent_pushed_streams_) {
      ResetStream(stream_id,
                  RST_STREAM_REFUSED_STREAM,
                  "Stream concurrency limit reached.");
      return STATUS_CODE_REFUSED_STREAM;
    }
  }

  if (stream->type() == SPDY_PUSH_STREAM) {
    // Will be balanced in DeleteStream.
    num_active_pushed_streams_++;
  }

  // May invalidate |stream|.
  int rv = stream->OnInitialResponseHeadersReceived(
      response_headers, response_time, recv_first_byte_time);
  if (rv < 0) {
    DCHECK_NE(rv, ERR_IO_PENDING);
    DCHECK(active_streams_.find(stream_id) == active_streams_.end());
  }

  return rv;
}

void SpdySession::OnSynStream(SpdyStreamId stream_id,
                              SpdyStreamId associated_stream_id,
                              SpdyPriority priority,
                              bool fin,
                              bool unidirectional,
                              const SpdyHeaderBlock& headers) {
  CHECK(in_io_loop_);

  DCHECK_LE(GetProtocolVersion(), SPDY3);

  base::Time response_time = base::Time::Now();
  base::TimeTicks recv_first_byte_time = time_func_();

  if (net_log_.IsCapturing()) {
    net_log_.AddEvent(
        NetLog::TYPE_HTTP2_SESSION_PUSHED_SYN_STREAM,
        base::Bind(&NetLogSpdySynStreamReceivedCallback, &headers, fin,
                   unidirectional, priority, stream_id, associated_stream_id));
  }

  // Split headers to simulate push promise and response.
  SpdyHeaderBlock request_headers;
  SpdyHeaderBlock response_headers;
  SplitPushedHeadersToRequestAndResponse(
      headers, GetProtocolVersion(), &request_headers, &response_headers);

  if (!TryCreatePushStream(
          stream_id, associated_stream_id, priority, request_headers))
    return;

  ActiveStreamMap::iterator active_it = active_streams_.find(stream_id);
  if (active_it == active_streams_.end()) {
    NOTREACHED();
    return;
  }

  OnInitialResponseHeadersReceived(response_headers, response_time,
                                   recv_first_byte_time,
                                   active_it->second.stream);
}

void SpdySession::DeleteExpiredPushedStreams() {
  if (unclaimed_pushed_streams_.empty())
    return;

  // Check that adequate time has elapsed since the last sweep.
  if (time_func_() < next_unclaimed_push_stream_sweep_time_)
    return;

  // Gather old streams to delete.
  base::TimeTicks minimum_freshness = time_func_() -
      base::TimeDelta::FromSeconds(kMinPushedStreamLifetimeSeconds);
  std::vector<SpdyStreamId> streams_to_close;
  for (PushedStreamMap::iterator it = unclaimed_pushed_streams_.begin();
       it != unclaimed_pushed_streams_.end(); ++it) {
    if (minimum_freshness > it->second.creation_time)
      streams_to_close.push_back(it->second.stream_id);
  }

  for (std::vector<SpdyStreamId>::const_iterator to_close_it =
           streams_to_close.begin();
       to_close_it != streams_to_close.end(); ++to_close_it) {
    ActiveStreamMap::iterator active_it = active_streams_.find(*to_close_it);
    if (active_it == active_streams_.end())
      continue;

    LogAbandonedActiveStream(active_it, ERR_INVALID_SPDY_STREAM);
    // CloseActiveStreamIterator() will remove the stream from
    // |unclaimed_pushed_streams_|.
    ResetStreamIterator(
        active_it, RST_STREAM_REFUSED_STREAM, "Stream not claimed.");
  }

  next_unclaimed_push_stream_sweep_time_ = time_func_() +
      base::TimeDelta::FromSeconds(kMinPushedStreamLifetimeSeconds);
}

void SpdySession::OnSynReply(SpdyStreamId stream_id,
                             bool fin,
                             const SpdyHeaderBlock& headers) {
  CHECK(in_io_loop_);

  base::Time response_time = base::Time::Now();
  base::TimeTicks recv_first_byte_time = time_func_();

  if (net_log().IsCapturing()) {
    net_log().AddEvent(NetLog::TYPE_HTTP2_SESSION_SYN_REPLY,
                       base::Bind(&NetLogSpdySynReplyOrHeadersReceivedCallback,
                                  &headers, fin, stream_id));
  }

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end()) {
    // NOTE:  it may just be that the stream was cancelled.
    return;
  }

  SpdyStream* stream = it->second.stream;
  CHECK_EQ(stream->stream_id(), stream_id);

  stream->AddRawReceivedBytes(last_compressed_frame_len_);
  last_compressed_frame_len_ = 0;

  if (GetProtocolVersion() >= HTTP2) {
    const std::string& error = "HTTP/2 wasn't expecting SYN_REPLY.";
    stream->LogStreamError(ERR_SPDY_PROTOCOL_ERROR, error);
    ResetStreamIterator(it, RST_STREAM_PROTOCOL_ERROR, error);
    return;
  }
  if (!it->second.waiting_for_syn_reply) {
    const std::string& error =
        "Received duplicate SYN_REPLY for stream.";
    stream->LogStreamError(ERR_SPDY_PROTOCOL_ERROR, error);
    ResetStreamIterator(it, RST_STREAM_PROTOCOL_ERROR, error);
    return;
  }
  it->second.waiting_for_syn_reply = false;

  ignore_result(OnInitialResponseHeadersReceived(
      headers, response_time, recv_first_byte_time, stream));
}

void SpdySession::OnHeaders(SpdyStreamId stream_id,
                            bool has_priority,
                            SpdyPriority priority,
                            SpdyStreamId parent_stream_id,
                            bool exclusive,
                            bool fin,
                            const SpdyHeaderBlock& headers) {
  CHECK(in_io_loop_);

  if (net_log().IsCapturing()) {
    net_log().AddEvent(NetLog::TYPE_HTTP2_SESSION_RECV_HEADERS,
                       base::Bind(&NetLogSpdySynReplyOrHeadersReceivedCallback,
                                  &headers, fin, stream_id));
  }

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end()) {
    // NOTE:  it may just be that the stream was cancelled.
    LOG(WARNING) << "Received HEADERS for invalid stream " << stream_id;
    return;
  }

  SpdyStream* stream = it->second.stream;
  CHECK_EQ(stream->stream_id(), stream_id);

  stream->AddRawReceivedBytes(last_compressed_frame_len_);
  last_compressed_frame_len_ = 0;

  base::Time response_time = base::Time::Now();
  base::TimeTicks recv_first_byte_time = time_func_();

  if (it->second.waiting_for_syn_reply) {
    if (GetProtocolVersion() < HTTP2) {
      const std::string& error =
          "Was expecting SYN_REPLY, not HEADERS.";
      stream->LogStreamError(ERR_SPDY_PROTOCOL_ERROR, error);
      ResetStreamIterator(it, RST_STREAM_PROTOCOL_ERROR, error);
      return;
    }

    it->second.waiting_for_syn_reply = false;
    ignore_result(OnInitialResponseHeadersReceived(
        headers, response_time, recv_first_byte_time, stream));
  } else if (it->second.stream->IsReservedRemote()) {
    ignore_result(OnInitialResponseHeadersReceived(
        headers, response_time, recv_first_byte_time, stream));
  } else {
    int rv = stream->OnAdditionalResponseHeadersReceived(headers);
    if (rv < 0) {
      DCHECK_NE(rv, ERR_IO_PENDING);
      DCHECK(active_streams_.find(stream_id) == active_streams_.end());
    }
  }
}

bool SpdySession::OnUnknownFrame(SpdyStreamId stream_id, int frame_type) {
  // Validate stream id.
  // Was the frame sent on a stream id that has not been used in this session?
  if (stream_id % 2 == 1 && stream_id > stream_hi_water_mark_)
    return false;

  if (stream_id % 2 == 0 && stream_id > last_accepted_push_stream_id_)
    return false;

  return true;
}

void SpdySession::OnRstStream(SpdyStreamId stream_id,
                              SpdyRstStreamStatus status) {
  CHECK(in_io_loop_);

  std::string description;
  net_log().AddEvent(
      NetLog::TYPE_HTTP2_SESSION_RST_STREAM,
      base::Bind(&NetLogSpdyRstCallback, stream_id, status, &description));

  ActiveStreamMap::iterator it = active_streams_.find(stream_id);
  if (it == active_streams_.end()) {
    // NOTE:  it may just be that the stream was cancelled.
    LOG(WARNING) << "Received RST for invalid stream" << stream_id;
    return;
  }

  CHECK_EQ(it->second.stream->stream_id(), stream_id);

  if (status == 0) {
    it->second.stream->OnDataReceived(scoped_ptr<SpdyBuffer>());
  } else if (status == RST_STREAM_REFUSED_STREAM) {
    CloseActiveStreamIterator(it, ERR_SPDY_SERVER_REFUSED_STREAM);
  } else if (status == RST_STREAM_HTTP_1_1_REQUIRED) {
    // TODO(bnc): Record histogram with number of open streams capped at 50.
    it->second.stream->LogStreamError(
        ERR_HTTP_1_1_REQUIRED,
        base::StringPrintf(
            "SPDY session closed because of stream with status: %d", status));
    DoDrainSession(ERR_HTTP_1_1_REQUIRED, "HTTP_1_1_REQUIRED for stream.");
  } else {
    RecordProtocolErrorHistogram(
        PROTOCOL_ERROR_RST_STREAM_FOR_NON_ACTIVE_STREAM);
    it->second.stream->LogStreamError(
        ERR_SPDY_PROTOCOL_ERROR,
        base::StringPrintf("SPDY stream closed with status: %d", status));
    // TODO(mbelshe): Map from Spdy-protocol errors to something sensical.
    //                For now, it doesn't matter much - it is a protocol error.
    CloseActiveStreamIterator(it, ERR_SPDY_PROTOCOL_ERROR);
  }
}

void SpdySession::OnGoAway(SpdyStreamId last_accepted_stream_id,
                           SpdyGoAwayStatus status,
                           StringPiece debug_data) {
  CHECK(in_io_loop_);

  // TODO(jgraettinger): UMA histogram on |status|.

  net_log_.AddEvent(
      NetLog::TYPE_HTTP2_SESSION_GOAWAY,
      base::Bind(&NetLogSpdyGoAwayCallback, last_accepted_stream_id,
                 active_streams_.size(), unclaimed_pushed_streams_.size(),
                 status, debug_data));
  MakeUnavailable();
  if (status == GOAWAY_HTTP_1_1_REQUIRED) {
    // TODO(bnc): Record histogram with number of open streams capped at 50.
    DoDrainSession(ERR_HTTP_1_1_REQUIRED, "HTTP_1_1_REQUIRED for stream.");
  } else {
    StartGoingAway(last_accepted_stream_id, ERR_ABORTED);
  }
  // This is to handle the case when we already don't have any active
  // streams (i.e., StartGoingAway() did nothing). Otherwise, we have
  // active streams and so the last one being closed will finish the
  // going away process (see DeleteStream()).
  MaybeFinishGoingAway();
}

void SpdySession::OnPing(SpdyPingId unique_id, bool is_ack) {
  CHECK(in_io_loop_);

  net_log_.AddEvent(
      NetLog::TYPE_HTTP2_SESSION_PING,
      base::Bind(&NetLogSpdyPingCallback, unique_id, is_ack, "received"));

  // Send response to a PING from server.
  if ((protocol_ == kProtoHTTP2 && !is_ack) ||
      (protocol_ < kProtoHTTP2 && unique_id % 2 == 0)) {
    WritePingFrame(unique_id, true);
    return;
  }

  --pings_in_flight_;
  if (pings_in_flight_ < 0) {
    RecordProtocolErrorHistogram(PROTOCOL_ERROR_UNEXPECTED_PING);
    DoDrainSession(ERR_SPDY_PROTOCOL_ERROR, "pings_in_flight_ is < 0.");
    pings_in_flight_ = 0;
    return;
  }

  if (pings_in_flight_ > 0)
    return;

  // We will record RTT in histogram when there are no more client sent
  // pings_in_flight_.
  RecordPingRTTHistogram(time_func_() - last_ping_sent_time_);
}

void SpdySession::OnWindowUpdate(SpdyStreamId stream_id,
                                 int delta_window_size) {
  CHECK(in_io_loop_);

  net_log_.AddEvent(NetLog::TYPE_HTTP2_SESSION_RECEIVED_WINDOW_UPDATE_FRAME,
                    base::Bind(&NetLogSpdyWindowUpdateFrameCallback, stream_id,
                               delta_window_size));

  if (stream_id == kSessionFlowControlStreamId) {
    // WINDOW_UPDATE for the session.
    if (delta_window_size < 1) {
      RecordProtocolErrorHistogram(PROTOCOL_ERROR_INVALID_WINDOW_UPDATE_SIZE);
      DoDrainSession(
          ERR_SPDY_PROTOCOL_ERROR,
          "Received WINDOW_UPDATE with an invalid delta_window_size " +
              base::IntToString(delta_window_size));
      return;
    }

    IncreaseSendWindowSize(delta_window_size);
  } else {
    // WINDOW_UPDATE for a stream.
    ActiveStreamMap::iterator it = active_streams_.find(stream_id);

    if (it == active_streams_.end()) {
      // NOTE:  it may just be that the stream was cancelled.
      LOG(WARNING) << "Received WINDOW_UPDATE for invalid stream " << stream_id;
      return;
    }

    SpdyStream* stream = it->second.stream;
    CHECK_EQ(stream->stream_id(), stream_id);

    if (delta_window_size < 1) {
      ResetStreamIterator(it, RST_STREAM_FLOW_CONTROL_ERROR,
                          base::StringPrintf(
                              "Received WINDOW_UPDATE with an invalid "
                              "delta_window_size %d",
                              delta_window_size));
      return;
    }

    CHECK_EQ(it->second.stream->stream_id(), stream_id);
    it->second.stream->IncreaseSendWindowSize(delta_window_size);
  }
}

bool SpdySession::TryCreatePushStream(SpdyStreamId stream_id,
                                      SpdyStreamId associated_stream_id,
                                      SpdyPriority priority,
                                      const SpdyHeaderBlock& headers) {
  // Server-initiated streams should have even sequence numbers.
  if ((stream_id & 0x1) != 0) {
    LOG(WARNING) << "Received invalid push stream id " << stream_id;
    CloseSessionOnError(ERR_SPDY_PROTOCOL_ERROR, "Odd push stream id.");
    return false;
  }

  if (stream_id <= last_accepted_push_stream_id_) {
    LOG(WARNING) << "Received push stream id lesser or equal to the last "
                 << "accepted before " << stream_id;
    CloseSessionOnError(
        ERR_SPDY_PROTOCOL_ERROR,
        "New push stream id must be greater than the last accepted.");
    return false;
  }

  if (IsStreamActive(stream_id)) {
    // For SPDY3 and higher we should not get here, we'll start going away
    // earlier on |last_seen_push_stream_id_| check.
    CHECK_GT(SPDY3, GetProtocolVersion());
    LOG(WARNING) << "Received push for active stream " << stream_id;
    return false;
  }

  last_accepted_push_stream_id_ = stream_id;

  RequestPriority request_priority =
      ConvertSpdyPriorityToRequestPriority(priority, GetProtocolVersion());

  if (availability_state_ == STATE_GOING_AWAY) {
    // TODO(akalin): This behavior isn't in the SPDY spec, although it
    // probably should be.
    EnqueueResetStreamFrame(stream_id,
                            request_priority,
                            RST_STREAM_REFUSED_STREAM,
                            "push stream request received when going away");
    return false;
  }

  if (associated_stream_id == 0) {
    // In HTTP/2 0 stream id in PUSH_PROMISE frame leads to framer error and
    // session going away. We should never get here.
    CHECK_GT(HTTP2, GetProtocolVersion());
    std::string description = base::StringPrintf(
        "Received invalid associated stream id %d for pushed stream %d",
        associated_stream_id,
        stream_id);
    EnqueueResetStreamFrame(
        stream_id, request_priority, RST_STREAM_REFUSED_STREAM, description);
    return false;
  }

  streams_pushed_count_++;

  // TODO(mbelshe): DCHECK that this is a GET method?

  // Verify that the response had a URL for us.
  GURL gurl = GetUrlFromHeaderBlock(headers, GetProtocolVersion());
  if (!gurl.is_valid()) {
    EnqueueResetStreamFrame(stream_id,
                            request_priority,
                            RST_STREAM_PROTOCOL_ERROR,
                            "Pushed stream url was invalid: " + gurl.spec());
    return false;
  }

  // Verify we have a valid stream association.
  ActiveStreamMap::iterator associated_it =
      active_streams_.find(associated_stream_id);
  if (associated_it == active_streams_.end()) {
    EnqueueResetStreamFrame(
        stream_id,
        request_priority,
        RST_STREAM_INVALID_STREAM,
        base::StringPrintf("Received push for inactive associated stream %d",
                           associated_stream_id));
    return false;
  }

  DCHECK(gurl.is_valid());

  // Check that the pushed stream advertises the same origin as its associated
  // stream. Bypass this check if and only if this session is with a SPDY proxy
  // that is trusted explicitly as determined by the |proxy_delegate_| or if the
  // proxy is pushing same-origin resources.
  if (!HostPortPair::FromURL(gurl).Equals(host_port_pair())) {
    if (proxy_delegate_ &&
        proxy_delegate_->IsTrustedSpdyProxy(
            ProxyServer(ProxyServer::SCHEME_HTTPS, host_port_pair()))) {
      // Disallow pushing of HTTPS content.
      if (gurl.SchemeIs("https")) {
        EnqueueResetStreamFrame(
            stream_id, request_priority, RST_STREAM_REFUSED_STREAM,
            base::StringPrintf("Rejected push of Cross Origin HTTPS content %d",
                               associated_stream_id));
        return false;
      }
    } else {
      GURL associated_url(associated_it->second.stream->GetUrlFromHeaders());
      if (associated_url.GetOrigin() != gurl.GetOrigin()) {
        EnqueueResetStreamFrame(
            stream_id, request_priority, RST_STREAM_REFUSED_STREAM,
            base::StringPrintf("Rejected Cross Origin Push Stream %d",
                               associated_stream_id));
        return false;
      }
    }
  }

  // There should not be an existing pushed stream with the same path.
  PushedStreamMap::iterator pushed_it =
      unclaimed_pushed_streams_.lower_bound(gurl);
  if (pushed_it != unclaimed_pushed_streams_.end() &&
      pushed_it->first == gurl) {
    EnqueueResetStreamFrame(
        stream_id,
        request_priority,
        RST_STREAM_PROTOCOL_ERROR,
        "Received duplicate pushed stream with url: " + gurl.spec());
    return false;
  }

  scoped_ptr<SpdyStream> stream(
      new SpdyStream(SPDY_PUSH_STREAM, GetWeakPtr(), gurl, request_priority,
                     stream_initial_send_window_size_,
                     stream_max_recv_window_size_, net_log_));
  stream->set_stream_id(stream_id);

  // In spdy4/http2 PUSH_PROMISE arrives on associated stream.
  if (associated_it != active_streams_.end() && GetProtocolVersion() >= HTTP2) {
    associated_it->second.stream->AddRawReceivedBytes(
        last_compressed_frame_len_);
  } else {
    stream->AddRawReceivedBytes(last_compressed_frame_len_);
  }

  last_compressed_frame_len_ = 0;

  PushedStreamMap::iterator inserted_pushed_it =
      unclaimed_pushed_streams_.insert(
          pushed_it,
          std::make_pair(gurl, PushedStreamInfo(stream_id, time_func_())));
  DCHECK(inserted_pushed_it != pushed_it);
  DeleteExpiredPushedStreams();

  InsertActivatedStream(std::move(stream));

  ActiveStreamMap::iterator active_it = active_streams_.find(stream_id);
  if (active_it == active_streams_.end()) {
    NOTREACHED();
    return false;
  }

  active_it->second.stream->OnPushPromiseHeadersReceived(headers);
  DCHECK(active_it->second.stream->IsReservedRemote());
  num_pushed_streams_++;
  return true;
}

void SpdySession::OnPushPromise(SpdyStreamId stream_id,
                                SpdyStreamId promised_stream_id,
                                const SpdyHeaderBlock& headers) {
  CHECK(in_io_loop_);

  if (net_log_.IsCapturing()) {
    net_log_.AddEvent(NetLog::TYPE_HTTP2_SESSION_RECV_PUSH_PROMISE,
                      base::Bind(&NetLogSpdyPushPromiseReceivedCallback,
                                 &headers, stream_id, promised_stream_id));
  }

  // Any priority will do.
  // TODO(baranovich): pass parent stream id priority?
  if (!TryCreatePushStream(promised_stream_id, stream_id, 0, headers))
    return;
}

void SpdySession::SendStreamWindowUpdate(SpdyStreamId stream_id,
                                         uint32_t delta_window_size) {
  ActiveStreamMap::const_iterator it = active_streams_.find(stream_id);
  CHECK(it != active_streams_.end());
  CHECK_EQ(it->second.stream->stream_id(), stream_id);
  SendWindowUpdateFrame(
      stream_id, delta_window_size, it->second.stream->priority());
}

void SpdySession::SendInitialData() {
  DCHECK(enable_sending_initial_data_);

  if (send_connection_header_prefix_) {
    DCHECK_EQ(protocol_, kProtoHTTP2);
    scoped_ptr<SpdyFrame> connection_header_prefix_frame(
        new SpdyFrame(const_cast<char*>(kHttp2ConnectionHeaderPrefix),
                      kHttp2ConnectionHeaderPrefixSize,
                      false /* take_ownership */));
    // Count the prefix as part of the subsequent SETTINGS frame.
    EnqueueSessionWrite(HIGHEST, SETTINGS,
                        std::move(connection_header_prefix_frame));
  }

  // First, notify the server about the settings they should use when
  // communicating with us.
  SettingsMap settings_map;
  // Create a new settings frame notifying the server of our
  // max concurrent streams and initial window size.
  settings_map[SETTINGS_MAX_CONCURRENT_STREAMS] =
      SettingsFlagsAndValue(SETTINGS_FLAG_NONE, kMaxConcurrentPushedStreams);
  if (stream_max_recv_window_size_ != GetDefaultInitialWindowSize(protocol_)) {
    settings_map[SETTINGS_INITIAL_WINDOW_SIZE] =
        SettingsFlagsAndValue(SETTINGS_FLAG_NONE, stream_max_recv_window_size_);
  }
  SendSettings(settings_map);

  // Next, notify the server about our initial recv window size.
  // Bump up the receive window size to the real initial value. This
  // has to go here since the WINDOW_UPDATE frame sent by
  // IncreaseRecvWindowSize() call uses |buffered_spdy_framer_|.
  // This condition implies that |session_max_recv_window_size_| -
  // |session_recv_window_size_| doesn't overflow.
  DCHECK_GE(session_max_recv_window_size_, session_recv_window_size_);
  DCHECK_GE(session_recv_window_size_, 0);
  if (session_max_recv_window_size_ > session_recv_window_size_) {
    IncreaseRecvWindowSize(session_max_recv_window_size_ -
                           session_recv_window_size_);
  }

  if (protocol_ == kProtoSPDY31) {
    // Finally, notify the server about the settings they have
    // previously told us to use when communicating with them (after
    // applying them).
    const SettingsMap& server_settings_map =
        http_server_properties_->GetSpdySettings(host_port_pair());
    if (server_settings_map.empty())
      return;

    SettingsMap::const_iterator it =
        server_settings_map.find(SETTINGS_CURRENT_CWND);
    uint32_t cwnd = (it != server_settings_map.end()) ? it->second.second : 0;
    UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsCwndSent", cwnd, 1, 200, 100);

    for (SettingsMap::const_iterator it = server_settings_map.begin();
         it != server_settings_map.end(); ++it) {
      const SpdySettingsIds new_id = it->first;
      const uint32_t new_val = it->second.second;
      HandleSetting(new_id, new_val);
    }

    SendSettings(server_settings_map);
  }
}


void SpdySession::SendSettings(const SettingsMap& settings) {
  const SpdyMajorVersion protocol_version = GetProtocolVersion();
  net_log_.AddEvent(
      NetLog::TYPE_HTTP2_SESSION_SEND_SETTINGS,
      base::Bind(&NetLogSpdySendSettingsCallback, &settings, protocol_version));
  // Create the SETTINGS frame and send it.
  DCHECK(buffered_spdy_framer_.get());
  scoped_ptr<SpdyFrame> settings_frame(
      buffered_spdy_framer_->CreateSettings(settings));
  sent_settings_ = true;
  EnqueueSessionWrite(HIGHEST, SETTINGS, std::move(settings_frame));
}

void SpdySession::HandleSetting(uint32_t id, uint32_t value) {
  switch (id) {
    case SETTINGS_MAX_CONCURRENT_STREAMS:
      max_concurrent_streams_ = std::min(static_cast<size_t>(value),
                                         kMaxConcurrentStreamLimit);
      ProcessPendingStreamRequests();
      break;
    case SETTINGS_INITIAL_WINDOW_SIZE: {
      if (value > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        net_log().AddEvent(
            NetLog::TYPE_HTTP2_SESSION_INITIAL_WINDOW_SIZE_OUT_OF_RANGE,
            NetLog::IntCallback("initial_window_size", value));
        return;
      }

      // SETTINGS_INITIAL_WINDOW_SIZE updates initial_send_window_size_ only.
      int32_t delta_window_size =
          static_cast<int32_t>(value) - stream_initial_send_window_size_;
      stream_initial_send_window_size_ = static_cast<int32_t>(value);
      UpdateStreamsSendWindowSize(delta_window_size);
      net_log().AddEvent(
          NetLog::TYPE_HTTP2_SESSION_UPDATE_STREAMS_SEND_WINDOW_SIZE,
          NetLog::IntCallback("delta_window_size", delta_window_size));
      break;
    }
  }
}

void SpdySession::UpdateStreamsSendWindowSize(int32_t delta_window_size) {
  for (ActiveStreamMap::iterator it = active_streams_.begin();
       it != active_streams_.end(); ++it) {
    it->second.stream->AdjustSendWindowSize(delta_window_size);
  }

  for (CreatedStreamSet::const_iterator it = created_streams_.begin();
       it != created_streams_.end(); it++) {
    (*it)->AdjustSendWindowSize(delta_window_size);
  }
}

void SpdySession::SendPrefacePingIfNoneInFlight() {
  if (pings_in_flight_ || !enable_ping_based_connection_checking_)
    return;

  base::TimeTicks now = time_func_();
  // If there is no activity in the session, then send a preface-PING.
  if ((now - last_activity_time_) > connection_at_risk_of_loss_time_)
    SendPrefacePing();
}

void SpdySession::SendPrefacePing() {
  WritePingFrame(next_ping_id_, false);
}

void SpdySession::SendWindowUpdateFrame(SpdyStreamId stream_id,
                                        uint32_t delta_window_size,
                                        RequestPriority priority) {
  ActiveStreamMap::const_iterator it = active_streams_.find(stream_id);
  if (it != active_streams_.end()) {
    CHECK_EQ(it->second.stream->stream_id(), stream_id);
  } else {
    CHECK_EQ(stream_id, kSessionFlowControlStreamId);
  }

  net_log_.AddEvent(NetLog::TYPE_HTTP2_SESSION_SENT_WINDOW_UPDATE_FRAME,
                    base::Bind(&NetLogSpdyWindowUpdateFrameCallback, stream_id,
                               delta_window_size));

  DCHECK(buffered_spdy_framer_.get());
  scoped_ptr<SpdyFrame> window_update_frame(
      buffered_spdy_framer_->CreateWindowUpdate(stream_id, delta_window_size));
  EnqueueSessionWrite(priority, WINDOW_UPDATE, std::move(window_update_frame));
}

void SpdySession::WritePingFrame(SpdyPingId unique_id, bool is_ack) {
  DCHECK(buffered_spdy_framer_.get());
  scoped_ptr<SpdyFrame> ping_frame(
      buffered_spdy_framer_->CreatePingFrame(unique_id, is_ack));
  EnqueueSessionWrite(HIGHEST, PING, std::move(ping_frame));

  if (net_log().IsCapturing()) {
    net_log().AddEvent(
        NetLog::TYPE_HTTP2_SESSION_PING,
        base::Bind(&NetLogSpdyPingCallback, unique_id, is_ack, "sent"));
  }
  if (!is_ack) {
    next_ping_id_ += 2;
    ++pings_in_flight_;
    PlanToCheckPingStatus();
    last_ping_sent_time_ = time_func_();
  }
}

void SpdySession::PlanToCheckPingStatus() {
  if (check_ping_status_pending_)
    return;

  check_ping_status_pending_ = true;
  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE, base::Bind(&SpdySession::CheckPingStatus,
                            weak_factory_.GetWeakPtr(), time_func_()),
      hung_interval_);
}

void SpdySession::CheckPingStatus(base::TimeTicks last_check_time) {
  CHECK(!in_io_loop_);

  // Check if we got a response back for all PINGs we had sent.
  if (pings_in_flight_ == 0) {
    check_ping_status_pending_ = false;
    return;
  }

  DCHECK(check_ping_status_pending_);

  base::TimeTicks now = time_func_();
  base::TimeDelta delay = hung_interval_ - (now - last_activity_time_);

  if (delay.InMilliseconds() < 0 || last_activity_time_ < last_check_time) {
    DoDrainSession(ERR_SPDY_PING_FAILED, "Failed ping.");
    return;
  }

  // Check the status of connection after a delay.
  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE, base::Bind(&SpdySession::CheckPingStatus,
                            weak_factory_.GetWeakPtr(), now),
      delay);
}

void SpdySession::RecordPingRTTHistogram(base::TimeDelta duration) {
  UMA_HISTOGRAM_CUSTOM_TIMES("Net.SpdyPing.RTT", duration,
                             base::TimeDelta::FromMilliseconds(1),
                             base::TimeDelta::FromMinutes(10), 100);
}

void SpdySession::RecordProtocolErrorHistogram(
    SpdyProtocolErrorDetails details) {
  UMA_HISTOGRAM_ENUMERATION("Net.SpdySessionErrorDetails2", details,
                            NUM_SPDY_PROTOCOL_ERROR_DETAILS);
  if (base::EndsWith(host_port_pair().host(), "google.com",
                     base::CompareCase::INSENSITIVE_ASCII)) {
    UMA_HISTOGRAM_ENUMERATION("Net.SpdySessionErrorDetails_Google2", details,
                              NUM_SPDY_PROTOCOL_ERROR_DETAILS);
  }
}

void SpdySession::RecordHistograms() {
  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdyStreamsPerSession",
                              streams_initiated_count_,
                              0, 300, 50);
  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdyStreamsPushedPerSession",
                              streams_pushed_count_,
                              0, 300, 50);
  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdyStreamsPushedAndClaimedPerSession",
                              streams_pushed_and_claimed_count_,
                              0, 300, 50);
  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdyStreamsAbandonedPerSession",
                              streams_abandoned_count_,
                              0, 300, 50);
  UMA_HISTOGRAM_ENUMERATION("Net.SpdySettingsSent",
                            sent_settings_ ? 1 : 0, 2);
  UMA_HISTOGRAM_ENUMERATION("Net.SpdySettingsReceived",
                            received_settings_ ? 1 : 0, 2);
  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdyStreamStallsPerSession",
                              stalled_streams_,
                              0, 300, 50);
  UMA_HISTOGRAM_ENUMERATION("Net.SpdySessionsWithStalls",
                            stalled_streams_ > 0 ? 1 : 0, 2);

  if (received_settings_) {
    // Enumerate the saved settings, and set histograms for it.
    const SettingsMap& settings_map =
        http_server_properties_->GetSpdySettings(host_port_pair());

    SettingsMap::const_iterator it;
    for (it = settings_map.begin(); it != settings_map.end(); ++it) {
      const SpdySettingsIds id = it->first;
      const uint32_t val = it->second.second;
      switch (id) {
        case SETTINGS_CURRENT_CWND:
          // Record several different histograms to see if cwnd converges
          // for larger volumes of data being sent.
          UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsCwnd",
                                      val, 1, 200, 100);
          if (total_bytes_received_ > 10 * 1024) {
            UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsCwnd10K",
                                        val, 1, 200, 100);
            if (total_bytes_received_ > 25 * 1024) {
              UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsCwnd25K",
                                          val, 1, 200, 100);
              if (total_bytes_received_ > 50 * 1024) {
                UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsCwnd50K",
                                            val, 1, 200, 100);
                if (total_bytes_received_ > 100 * 1024) {
                  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsCwnd100K",
                                              val, 1, 200, 100);
                }
              }
            }
          }
          break;
        case SETTINGS_ROUND_TRIP_TIME:
          UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsRTT",
                                      val, 1, 1200, 100);
          break;
        case SETTINGS_DOWNLOAD_RETRANS_RATE:
          UMA_HISTOGRAM_CUSTOM_COUNTS("Net.SpdySettingsRetransRate",
                                      val, 1, 100, 50);
          break;
        default:
          break;
      }
    }
  }
}

void SpdySession::CompleteStreamRequest(
    const base::WeakPtr<SpdyStreamRequest>& pending_request) {
  // Abort if the request has already been cancelled.
  if (!pending_request)
    return;

  base::WeakPtr<SpdyStream> stream;
  int rv = TryCreateStream(pending_request, &stream);

  if (rv == OK) {
    DCHECK(stream);
    pending_request->OnRequestCompleteSuccess(stream);
    return;
  }
  DCHECK(!stream);

  if (rv != ERR_IO_PENDING) {
    pending_request->OnRequestCompleteFailure(rv);
  }
}

void SpdySession::OnWriteBufferConsumed(
    size_t frame_payload_size,
    size_t consume_size,
    SpdyBuffer::ConsumeSource consume_source) {
  // We can be called with |in_io_loop_| set if a write SpdyBuffer is
  // deleted (e.g., a stream is closed due to incoming data).
  if (consume_source == SpdyBuffer::DISCARD) {
    // If we're discarding a frame or part of it, increase the send
    // window by the number of discarded bytes. (Although if we're
    // discarding part of a frame, it's probably because of a write
    // error and we'll be tearing down the session soon.)
    int remaining_payload_bytes = std::min(consume_size, frame_payload_size);
    DCHECK_GT(remaining_payload_bytes, 0);
    IncreaseSendWindowSize(remaining_payload_bytes);
  }
  // For consumed bytes, the send window is increased when we receive
  // a WINDOW_UPDATE frame.
}

void SpdySession::IncreaseSendWindowSize(int delta_window_size) {
  // We can be called with |in_io_loop_| set if a SpdyBuffer is
  // deleted (e.g., a stream is closed due to incoming data).
  DCHECK_GE(delta_window_size, 1);

  // Check for overflow.
  int32_t max_delta_window_size =
      std::numeric_limits<int32_t>::max() - session_send_window_size_;
  if (delta_window_size > max_delta_window_size) {
    RecordProtocolErrorHistogram(PROTOCOL_ERROR_INVALID_WINDOW_UPDATE_SIZE);
    DoDrainSession(
        ERR_SPDY_PROTOCOL_ERROR,
        "Received WINDOW_UPDATE [delta: " +
            base::IntToString(delta_window_size) +
            "] for session overflows session_send_window_size_ [current: " +
            base::IntToString(session_send_window_size_) + "]");
    return;
  }

  session_send_window_size_ += delta_window_size;

  net_log_.AddEvent(NetLog::TYPE_HTTP2_SESSION_UPDATE_SEND_WINDOW,
                    base::Bind(&NetLogSpdySessionWindowUpdateCallback,
                               delta_window_size, session_send_window_size_));

  DCHECK(!IsSendStalled());
  ResumeSendStalledStreams();
}

void SpdySession::DecreaseSendWindowSize(int32_t delta_window_size) {
  // We only call this method when sending a frame. Therefore,
  // |delta_window_size| should be within the valid frame size range.
  DCHECK_GE(delta_window_size, 1);
  DCHECK_LE(delta_window_size, kMaxSpdyFrameChunkSize);

  // |send_window_size_| should have been at least |delta_window_size| for
  // this call to happen.
  DCHECK_GE(session_send_window_size_, delta_window_size);

  session_send_window_size_ -= delta_window_size;

  net_log_.AddEvent(NetLog::TYPE_HTTP2_SESSION_UPDATE_SEND_WINDOW,
                    base::Bind(&NetLogSpdySessionWindowUpdateCallback,
                               -delta_window_size, session_send_window_size_));
}

void SpdySession::OnReadBufferConsumed(
    size_t consume_size,
    SpdyBuffer::ConsumeSource consume_source) {
  // We can be called with |in_io_loop_| set if a read SpdyBuffer is
  // deleted (e.g., discarded by a SpdyReadQueue).
  DCHECK_GE(consume_size, 1u);
  DCHECK_LE(consume_size,
            static_cast<size_t>(std::numeric_limits<int32_t>::max()));

  IncreaseRecvWindowSize(static_cast<int32_t>(consume_size));
}

void SpdySession::IncreaseRecvWindowSize(int32_t delta_window_size) {
  DCHECK_GE(session_unacked_recv_window_bytes_, 0);
  DCHECK_GE(session_recv_window_size_, session_unacked_recv_window_bytes_);
  DCHECK_GE(delta_window_size, 1);
  // Check for overflow.
  DCHECK_LE(delta_window_size,
            std::numeric_limits<int32_t>::max() - session_recv_window_size_);

  session_recv_window_size_ += delta_window_size;
  net_log_.AddEvent(NetLog::TYPE_HTTP2_STREAM_UPDATE_RECV_WINDOW,
                    base::Bind(&NetLogSpdySessionWindowUpdateCallback,
                               delta_window_size, session_recv_window_size_));

  session_unacked_recv_window_bytes_ += delta_window_size;
  if (session_unacked_recv_window_bytes_ > session_max_recv_window_size_ / 2) {
    SendWindowUpdateFrame(kSessionFlowControlStreamId,
                          session_unacked_recv_window_bytes_,
                          HIGHEST);
    session_unacked_recv_window_bytes_ = 0;
  }
}

void SpdySession::DecreaseRecvWindowSize(int32_t delta_window_size) {
  CHECK(in_io_loop_);
  DCHECK_GE(delta_window_size, 1);

  // The receiving window size as the peer knows it is
  // |session_recv_window_size_ - session_unacked_recv_window_bytes_|, if more
  // data are sent by the peer, that means that the receive window is not being
  // respected.
  if (delta_window_size >
      session_recv_window_size_ - session_unacked_recv_window_bytes_) {
    RecordProtocolErrorHistogram(PROTOCOL_ERROR_RECEIVE_WINDOW_VIOLATION);
    DoDrainSession(
        ERR_SPDY_FLOW_CONTROL_ERROR,
        "delta_window_size is " + base::IntToString(delta_window_size) +
            " in DecreaseRecvWindowSize, which is larger than the receive " +
            "window size of " + base::IntToString(session_recv_window_size_));
    return;
  }

  session_recv_window_size_ -= delta_window_size;
  net_log_.AddEvent(NetLog::TYPE_HTTP2_SESSION_UPDATE_RECV_WINDOW,
                    base::Bind(&NetLogSpdySessionWindowUpdateCallback,
                               -delta_window_size, session_recv_window_size_));
}

void SpdySession::QueueSendStalledStream(const SpdyStream& stream) {
  DCHECK(stream.send_stalled_by_flow_control());
  RequestPriority priority = stream.priority();
  CHECK_GE(priority, MINIMUM_PRIORITY);
  CHECK_LE(priority, MAXIMUM_PRIORITY);
  stream_send_unstall_queue_[priority].push_back(stream.stream_id());
}

void SpdySession::ResumeSendStalledStreams() {
  // We don't have to worry about new streams being queued, since
  // doing so would cause IsSendStalled() to return true. But we do
  // have to worry about streams being closed, as well as ourselves
  // being closed.

  while (!IsSendStalled()) {
    size_t old_size = 0;
#if DCHECK_IS_ON()
    old_size = GetTotalSize(stream_send_unstall_queue_);
#endif

    SpdyStreamId stream_id = PopStreamToPossiblyResume();
    if (stream_id == 0)
      break;
    ActiveStreamMap::const_iterator it = active_streams_.find(stream_id);
    // The stream may actually still be send-stalled after this (due
    // to its own send window) but that's okay -- it'll then be
    // resumed once its send window increases.
    if (it != active_streams_.end())
      it->second.stream->PossiblyResumeIfSendStalled();

    // The size should decrease unless we got send-stalled again.
    if (!IsSendStalled())
      DCHECK_LT(GetTotalSize(stream_send_unstall_queue_), old_size);
  }
}

SpdyStreamId SpdySession::PopStreamToPossiblyResume() {
  for (int i = MAXIMUM_PRIORITY; i >= MINIMUM_PRIORITY; --i) {
    std::deque<SpdyStreamId>* queue = &stream_send_unstall_queue_[i];
    if (!queue->empty()) {
      SpdyStreamId stream_id = queue->front();
      queue->pop_front();
      return stream_id;
    }
  }
  return 0;
}

}  // namespace net
