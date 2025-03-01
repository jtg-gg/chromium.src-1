// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/quic/quic_chromium_client_session.h"

#include <utility>

#include "base/callback_helpers.h"
#include "base/location.h"
#include "base/metrics/histogram_macros.h"
#include "base/metrics/sparse_histogram.h"
#include "base/single_thread_task_runner.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/thread_task_runner_handle.h"
#include "base/values.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/network_activity_monitor.h"
#include "net/http/transport_security_state.h"
#include "net/quic/crypto/proof_verifier_chromium.h"
#include "net/quic/crypto/quic_server_info.h"
#include "net/quic/quic_chromium_connection_helper.h"
#include "net/quic/quic_crypto_client_stream_factory.h"
#include "net/quic/quic_protocol.h"
#include "net/quic/quic_server_id.h"
#include "net/quic/quic_stream_factory.h"
#include "net/spdy/spdy_session.h"
#include "net/ssl/channel_id_service.h"
#include "net/ssl/ssl_connection_status_flags.h"
#include "net/ssl/ssl_info.h"
#include "net/udp/datagram_client_socket.h"

namespace net {

namespace {

// The length of time to wait for a 0-RTT handshake to complete
// before allowing the requests to possibly proceed over TCP.
const int k0RttHandshakeTimeoutMs = 300;

// IPv6 packets have an additional 20 bytes of overhead than IPv4 packets.
const size_t kAdditionalOverheadForIPv6 = 20;

// Maximum number of Readers that are created for any session due to
// connection migration. A new Reader is created every time this endpoint's
// IP address changes.
const size_t kMaxReadersPerQuicSession = 5;

// Histograms for tracking down the crashes from http://crbug.com/354669
// Note: these values must be kept in sync with the corresponding values in:
// tools/metrics/histograms/histograms.xml
enum Location {
  DESTRUCTOR = 0,
  ADD_OBSERVER = 1,
  TRY_CREATE_STREAM = 2,
  CREATE_OUTGOING_RELIABLE_STREAM = 3,
  NOTIFY_FACTORY_OF_SESSION_CLOSED_LATER = 4,
  NOTIFY_FACTORY_OF_SESSION_CLOSED = 5,
  NUM_LOCATIONS = 6,
};

void RecordUnexpectedOpenStreams(Location location) {
  UMA_HISTOGRAM_ENUMERATION("Net.QuicSession.UnexpectedOpenStreams", location,
                            NUM_LOCATIONS);
}

void RecordUnexpectedObservers(Location location) {
  UMA_HISTOGRAM_ENUMERATION("Net.QuicSession.UnexpectedObservers", location,
                            NUM_LOCATIONS);
}

void RecordUnexpectedNotGoingAway(Location location) {
  UMA_HISTOGRAM_ENUMERATION("Net.QuicSession.UnexpectedNotGoingAway", location,
                            NUM_LOCATIONS);
}

// Histogram for recording the different reasons that a QUIC session is unable
// to complete the handshake.
enum HandshakeFailureReason {
  HANDSHAKE_FAILURE_UNKNOWN = 0,
  HANDSHAKE_FAILURE_BLACK_HOLE = 1,
  HANDSHAKE_FAILURE_PUBLIC_RESET = 2,
  NUM_HANDSHAKE_FAILURE_REASONS = 3,
};

void RecordHandshakeFailureReason(HandshakeFailureReason reason) {
  UMA_HISTOGRAM_ENUMERATION(
      "Net.QuicSession.ConnectionClose.HandshakeNotConfirmed.Reason", reason,
      NUM_HANDSHAKE_FAILURE_REASONS);
}

// Note: these values must be kept in sync with the corresponding values in:
// tools/metrics/histograms/histograms.xml
enum HandshakeState {
  STATE_STARTED = 0,
  STATE_ENCRYPTION_ESTABLISHED = 1,
  STATE_HANDSHAKE_CONFIRMED = 2,
  STATE_FAILED = 3,
  NUM_HANDSHAKE_STATES = 4
};

void RecordHandshakeState(HandshakeState state) {
  UMA_HISTOGRAM_ENUMERATION("Net.QuicHandshakeState", state,
                            NUM_HANDSHAKE_STATES);
}

scoped_ptr<base::Value> NetLogQuicClientSessionCallback(
    const QuicServerId* server_id,
    int cert_verify_flags,
    bool require_confirmation,
    NetLogCaptureMode /* capture_mode */) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->SetString("host", server_id->host());
  dict->SetInteger("port", server_id->port());
  dict->SetBoolean("privacy_mode",
                   server_id->privacy_mode() == PRIVACY_MODE_ENABLED);
  dict->SetBoolean("require_confirmation", require_confirmation);
  dict->SetInteger("cert_verify_flags", cert_verify_flags);
  return std::move(dict);
}

}  // namespace

QuicChromiumClientSession::StreamRequest::StreamRequest() : stream_(nullptr) {}

QuicChromiumClientSession::StreamRequest::~StreamRequest() {
  CancelRequest();
}

int QuicChromiumClientSession::StreamRequest::StartRequest(
    const base::WeakPtr<QuicChromiumClientSession>& session,
    QuicChromiumClientStream** stream,
    const CompletionCallback& callback) {
  session_ = session;
  stream_ = stream;
  int rv = session_->TryCreateStream(this, stream_);
  if (rv == ERR_IO_PENDING) {
    callback_ = callback;
  }

  return rv;
}

void QuicChromiumClientSession::StreamRequest::CancelRequest() {
  if (session_)
    session_->CancelRequest(this);
  session_.reset();
  callback_.Reset();
}

void QuicChromiumClientSession::StreamRequest::OnRequestCompleteSuccess(
    QuicChromiumClientStream* stream) {
  session_.reset();
  *stream_ = stream;
  base::ResetAndReturn(&callback_).Run(OK);
}

void QuicChromiumClientSession::StreamRequest::OnRequestCompleteFailure(
    int rv) {
  session_.reset();
  base::ResetAndReturn(&callback_).Run(rv);
}

QuicChromiumClientSession::QuicChromiumClientSession(
    QuicConnection* connection,
    scoped_ptr<DatagramClientSocket> socket,
    QuicStreamFactory* stream_factory,
    QuicCryptoClientStreamFactory* crypto_client_stream_factory,
    QuicClock* clock,
    TransportSecurityState* transport_security_state,
    scoped_ptr<QuicServerInfo> server_info,
    const QuicServerId& server_id,
    int yield_after_packets,
    QuicTime::Delta yield_after_duration,
    int cert_verify_flags,
    const QuicConfig& config,
    QuicCryptoClientConfig* crypto_config,
    const char* const connection_description,
    base::TimeTicks dns_resolution_end_time,
    QuicClientPushPromiseIndex* push_promise_index,
    base::TaskRunner* task_runner,
    scoped_ptr<SocketPerformanceWatcher> socket_performance_watcher,
    NetLog* net_log)
    : QuicClientSessionBase(connection, push_promise_index, config),
      server_id_(server_id),
      require_confirmation_(false),
      stream_factory_(stream_factory),
      transport_security_state_(transport_security_state),
      server_info_(std::move(server_info)),
      num_total_streams_(0),
      task_runner_(task_runner),
      net_log_(BoundNetLog::Make(net_log, NetLog::SOURCE_QUIC_SESSION)),
      dns_resolution_end_time_(dns_resolution_end_time),
      logger_(new QuicConnectionLogger(this,
                                       connection_description,
                                       std::move(socket_performance_watcher),
                                       net_log_)),
      going_away_(false),
      disabled_reason_(QUIC_DISABLED_NOT),
      weak_factory_(this) {
  sockets_.push_back(std::move(socket));
  packet_readers_.push_back(make_scoped_ptr(new QuicChromiumPacketReader(
      sockets_.back().get(), clock, this, yield_after_packets,
      yield_after_duration, net_log_)));
  crypto_stream_.reset(
      crypto_client_stream_factory->CreateQuicCryptoClientStream(
          server_id, this, make_scoped_ptr(new ProofVerifyContextChromium(
                               cert_verify_flags, net_log_)),
          crypto_config));
  connection->set_debug_visitor(logger_.get());
  connection->set_creator_debug_delegate(logger_.get());
  net_log_.BeginEvent(NetLog::TYPE_QUIC_SESSION,
                      base::Bind(NetLogQuicClientSessionCallback, &server_id,
                                 cert_verify_flags, require_confirmation_));
  IPEndPoint address;
  if (socket && socket->GetLocalAddress(&address) == OK &&
      address.GetFamily() == ADDRESS_FAMILY_IPV6) {
    connection->SetMaxPacketLength(connection->max_packet_length() -
                                   kAdditionalOverheadForIPv6);
  }
}

QuicChromiumClientSession::~QuicChromiumClientSession() {
  if (!dynamic_streams().empty())
    RecordUnexpectedOpenStreams(DESTRUCTOR);
  if (!observers_.empty())
    RecordUnexpectedObservers(DESTRUCTOR);
  if (!going_away_)
    RecordUnexpectedNotGoingAway(DESTRUCTOR);

  while (!dynamic_streams().empty() || !observers_.empty() ||
         !stream_requests_.empty()) {
    // The session must be closed before it is destroyed.
    DCHECK(dynamic_streams().empty());
    CloseAllStreams(ERR_UNEXPECTED);
    DCHECK(observers_.empty());
    CloseAllObservers(ERR_UNEXPECTED);

    connection()->set_debug_visitor(nullptr);
    net_log_.EndEvent(NetLog::TYPE_QUIC_SESSION);

    while (!stream_requests_.empty()) {
      StreamRequest* request = stream_requests_.front();
      stream_requests_.pop_front();
      request->OnRequestCompleteFailure(ERR_ABORTED);
    }
  }

  if (connection()->connected()) {
    // Ensure that the connection is closed by the time the session is
    // destroyed.
    connection()->CloseConnection(QUIC_INTERNAL_ERROR,
                                  ConnectionCloseSource::FROM_SELF);
  }

  if (IsEncryptionEstablished())
    RecordHandshakeState(STATE_ENCRYPTION_ESTABLISHED);
  if (IsCryptoHandshakeConfirmed())
    RecordHandshakeState(STATE_HANDSHAKE_CONFIRMED);
  else
    RecordHandshakeState(STATE_FAILED);

  UMA_HISTOGRAM_COUNTS("Net.QuicSession.NumTotalStreams", num_total_streams_);
  UMA_HISTOGRAM_COUNTS("Net.QuicNumSentClientHellos",
                       crypto_stream_->num_sent_client_hellos());
  if (!IsCryptoHandshakeConfirmed())
    return;

  // Sending one client_hello means we had zero handshake-round-trips.
  int round_trip_handshakes = crypto_stream_->num_sent_client_hellos() - 1;

  // Don't bother with these histogram during tests, which mock out
  // num_sent_client_hellos().
  if (round_trip_handshakes < 0 || !stream_factory_)
    return;

  bool port_selected = stream_factory_->enable_port_selection();
  SSLInfo ssl_info;
  // QUIC supports only secure urls.
  if (GetSSLInfo(&ssl_info) && ssl_info.cert.get()) {
    if (port_selected) {
      UMA_HISTOGRAM_CUSTOM_COUNTS("Net.QuicSession.ConnectSelectPortForHTTPS",
                                  round_trip_handshakes, 0, 3, 4);
    } else {
      UMA_HISTOGRAM_CUSTOM_COUNTS("Net.QuicSession.ConnectRandomPortForHTTPS",
                                  round_trip_handshakes, 0, 3, 4);
      if (require_confirmation_) {
        UMA_HISTOGRAM_CUSTOM_COUNTS(
            "Net.QuicSession.ConnectRandomPortRequiringConfirmationForHTTPS",
            round_trip_handshakes, 0, 3, 4);
      }
    }
  }
  const QuicConnectionStats stats = connection()->GetStats();
  if (server_info_ && stats.min_rtt_us > 0) {
    base::TimeTicks wait_for_data_start_time =
        server_info_->wait_for_data_start_time();
    base::TimeTicks wait_for_data_end_time =
        server_info_->wait_for_data_end_time();
    if (!wait_for_data_start_time.is_null() &&
        !wait_for_data_end_time.is_null()) {
      base::TimeDelta wait_time =
          wait_for_data_end_time - wait_for_data_start_time;
      const base::HistogramBase::Sample kMaxWaitToRtt = 1000;
      base::HistogramBase::Sample wait_to_rtt =
          static_cast<base::HistogramBase::Sample>(
              100 * wait_time.InMicroseconds() / stats.min_rtt_us);
      UMA_HISTOGRAM_CUSTOM_COUNTS("Net.QuicServerInfo.WaitForDataReadyToRtt",
                                  wait_to_rtt, 0, kMaxWaitToRtt, 50);
    }
  }

  // The MTU used by QUIC is limited to a fairly small set of predefined values
  // (initial values and MTU discovery values), but does not fare well when
  // bucketed.  Because of that, a sparse histogram is used here.
  UMA_HISTOGRAM_SPARSE_SLOWLY("Net.QuicSession.ClientSideMtu",
                              connection()->max_packet_length());
  UMA_HISTOGRAM_SPARSE_SLOWLY("Net.QuicSession.ServerSideMtu",
                              stats.max_received_packet_size);

  UMA_HISTOGRAM_COUNTS("Net.QuicSession.MtuProbesSent",
                       connection()->mtu_probe_count());

  if (stats.max_sequence_reordering == 0)
    return;
  const base::HistogramBase::Sample kMaxReordering = 100;
  base::HistogramBase::Sample reordering = kMaxReordering;
  if (stats.min_rtt_us > 0) {
    reordering = static_cast<base::HistogramBase::Sample>(
        100 * stats.max_time_reordering_us / stats.min_rtt_us);
  }
  UMA_HISTOGRAM_CUSTOM_COUNTS("Net.QuicSession.MaxReorderingTime", reordering,
                              0, kMaxReordering, 50);
  if (stats.min_rtt_us > 100 * 1000) {
    UMA_HISTOGRAM_CUSTOM_COUNTS("Net.QuicSession.MaxReorderingTimeLongRtt",
                                reordering, 0, kMaxReordering, 50);
  }
  UMA_HISTOGRAM_COUNTS(
      "Net.QuicSession.MaxReordering",
      static_cast<base::HistogramBase::Sample>(stats.max_sequence_reordering));
}

void QuicChromiumClientSession::OnHeadersHeadOfLineBlocking(
    QuicTime::Delta delta) {
  UMA_HISTOGRAM_TIMES(
      "Net.QuicSession.HeadersHOLBlockedTime",
      base::TimeDelta::FromMicroseconds(delta.ToMicroseconds()));
}

void QuicChromiumClientSession::OnStreamFrame(const QuicStreamFrame& frame) {
  // Record total number of stream frames.
  UMA_HISTOGRAM_COUNTS("Net.QuicNumStreamFramesInPacket", 1);

  // Record number of frames per stream in packet.
  UMA_HISTOGRAM_COUNTS("Net.QuicNumStreamFramesPerStreamInPacket", 1);

  return QuicSpdySession::OnStreamFrame(frame);
}

void QuicChromiumClientSession::AddObserver(Observer* observer) {
  if (going_away_) {
    RecordUnexpectedObservers(ADD_OBSERVER);
    observer->OnSessionClosed(ERR_UNEXPECTED);
    return;
  }

  DCHECK(!ContainsKey(observers_, observer));
  observers_.insert(observer);
}

void QuicChromiumClientSession::RemoveObserver(Observer* observer) {
  DCHECK(ContainsKey(observers_, observer));
  observers_.erase(observer);
}

int QuicChromiumClientSession::TryCreateStream(
    StreamRequest* request,
    QuicChromiumClientStream** stream) {
  if (!crypto_stream_->encryption_established()) {
    DLOG(DFATAL) << "Encryption not established.";
    return ERR_CONNECTION_CLOSED;
  }

  if (goaway_received()) {
    DVLOG(1) << "Going away.";
    return ERR_CONNECTION_CLOSED;
  }

  if (!connection()->connected()) {
    DVLOG(1) << "Already closed.";
    return ERR_CONNECTION_CLOSED;
  }

  if (going_away_) {
    RecordUnexpectedOpenStreams(TRY_CREATE_STREAM);
    return ERR_CONNECTION_CLOSED;
  }

  if (GetNumOpenOutgoingStreams() < max_open_outgoing_streams()) {
    *stream = CreateOutgoingReliableStreamImpl();
    return OK;
  }

  stream_requests_.push_back(request);
  return ERR_IO_PENDING;
}

void QuicChromiumClientSession::CancelRequest(StreamRequest* request) {
  // Remove |request| from the queue while preserving the order of the
  // other elements.
  StreamRequestQueue::iterator it =
      std::find(stream_requests_.begin(), stream_requests_.end(), request);
  if (it != stream_requests_.end()) {
    it = stream_requests_.erase(it);
  }
}

bool QuicChromiumClientSession::ShouldCreateOutgoingDynamicStream() {
  if (!crypto_stream_->encryption_established()) {
    DVLOG(1) << "Encryption not active so no outgoing stream created.";
    return false;
  }
  if (GetNumOpenOutgoingStreams() >= max_open_outgoing_streams()) {
    DVLOG(1) << "Failed to create a new outgoing stream. "
             << "Already " << GetNumOpenOutgoingStreams() << " open.";
    return false;
  }
  if (goaway_received()) {
    DVLOG(1) << "Failed to create a new outgoing stream. "
             << "Already received goaway.";
    return false;
  }
  if (going_away_) {
    RecordUnexpectedOpenStreams(CREATE_OUTGOING_RELIABLE_STREAM);
    return false;
  }
  return true;
}

QuicChromiumClientStream*
QuicChromiumClientSession::CreateOutgoingDynamicStream(SpdyPriority priority) {
  if (!ShouldCreateOutgoingDynamicStream()) {
    return nullptr;
  }
  return CreateOutgoingReliableStreamImpl();
}

QuicChromiumClientStream*
QuicChromiumClientSession::CreateOutgoingReliableStreamImpl() {
  DCHECK(connection()->connected());
  QuicChromiumClientStream* stream =
      new QuicChromiumClientStream(GetNextOutgoingStreamId(), this, net_log_);
  ActivateStream(stream);
  ++num_total_streams_;
  UMA_HISTOGRAM_COUNTS("Net.QuicSession.NumOpenStreams",
                       GetNumOpenOutgoingStreams());
  // The previous histogram puts 100 in a bucket betweeen 86-113 which does
  // not shed light on if chrome ever things it has more than 100 streams open.
  UMA_HISTOGRAM_BOOLEAN("Net.QuicSession.TooManyOpenStreams",
                        GetNumOpenOutgoingStreams() > 100);
  return stream;
}

QuicCryptoClientStream* QuicChromiumClientSession::GetCryptoStream() {
  return crypto_stream_.get();
};

// TODO(rtenneti): Add unittests for GetSSLInfo which exercise the various ways
// we learn about SSL info (sync vs async vs cached).
bool QuicChromiumClientSession::GetSSLInfo(SSLInfo* ssl_info) const {
  ssl_info->Reset();
  if (!cert_verify_result_) {
    return false;
  }

  ssl_info->cert_status = cert_verify_result_->cert_status;
  ssl_info->cert = cert_verify_result_->verified_cert;

  // TODO(wtc): Define QUIC "cipher suites".
  // Report the TLS cipher suite that most closely resembles the crypto
  // parameters of the QUIC connection.
  QuicTag aead = crypto_stream_->crypto_negotiated_params().aead;
  uint16_t cipher_suite;
  int security_bits;
  switch (aead) {
    case kAESG:
      cipher_suite = 0xc02f;  // TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
      security_bits = 128;
      break;
    case kCC12:
      cipher_suite = 0xcc13;  // TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256
      security_bits = 256;
      break;
    case kCC20:
      cipher_suite = 0xcc13;  // TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256
      security_bits = 256;
      break;
    default:
      NOTREACHED();
      return false;
  }
  int ssl_connection_status = 0;
  ssl_connection_status |= cipher_suite;
  ssl_connection_status |=
      (SSL_CONNECTION_VERSION_QUIC & SSL_CONNECTION_VERSION_MASK)
      << SSL_CONNECTION_VERSION_SHIFT;

  ssl_info->public_key_hashes = cert_verify_result_->public_key_hashes;
  ssl_info->is_issued_by_known_root =
      cert_verify_result_->is_issued_by_known_root;

  ssl_info->connection_status = ssl_connection_status;
  ssl_info->client_cert_sent = false;
  ssl_info->channel_id_sent = crypto_stream_->WasChannelIDSent();
  ssl_info->security_bits = security_bits;
  ssl_info->handshake_type = SSLInfo::HANDSHAKE_FULL;
  ssl_info->pinning_failure_log = pinning_failure_log_;

  ssl_info->UpdateCertificateTransparencyInfo(*ct_verify_result_);

  return true;
}

int QuicChromiumClientSession::CryptoConnect(
    bool require_confirmation,
    const CompletionCallback& callback) {
  require_confirmation_ = require_confirmation;
  handshake_start_ = base::TimeTicks::Now();
  RecordHandshakeState(STATE_STARTED);
  DCHECK(flow_controller());
  crypto_stream_->CryptoConnect();

  if (IsCryptoHandshakeConfirmed())
    return OK;

  // Unless we require handshake confirmation, activate the session if
  // we have established initial encryption.
  if (!require_confirmation_ && IsEncryptionEstablished()) {
    // To mitigate the effects of hanging 0-RTT connections, set up a timer to
    // cancel any requests, if the handshake takes too long.
    task_runner_->PostDelayedTask(
        FROM_HERE, base::Bind(&QuicChromiumClientSession::OnConnectTimeout,
                              weak_factory_.GetWeakPtr()),
        base::TimeDelta::FromMilliseconds(k0RttHandshakeTimeoutMs));
    return OK;
  }

  callback_ = callback;
  return ERR_IO_PENDING;
}

int QuicChromiumClientSession::ResumeCryptoConnect(
    const CompletionCallback& callback) {
  if (IsCryptoHandshakeConfirmed())
    return OK;

  if (!connection()->connected())
    return ERR_QUIC_HANDSHAKE_FAILED;

  callback_ = callback;
  return ERR_IO_PENDING;
}

int QuicChromiumClientSession::GetNumSentClientHellos() const {
  return crypto_stream_->num_sent_client_hellos();
}

bool QuicChromiumClientSession::CanPool(const std::string& hostname,
                                        PrivacyMode privacy_mode) const {
  DCHECK(connection()->connected());
  if (privacy_mode != server_id_.privacy_mode()) {
    // Privacy mode must always match.
    return false;
  }
  SSLInfo ssl_info;
  if (!GetSSLInfo(&ssl_info) || !ssl_info.cert.get()) {
    NOTREACHED() << "QUIC should always have certificates.";
    return false;
  }

  return SpdySession::CanPool(transport_security_state_, ssl_info,
                              server_id_.host(), hostname);
}

QuicSpdyStream* QuicChromiumClientSession::CreateIncomingDynamicStream(
    QuicStreamId id) {
  DLOG(ERROR) << "Server push not supported";
  return nullptr;
}

void QuicChromiumClientSession::CloseStream(QuicStreamId stream_id) {
  ReliableQuicStream* stream = GetStream(stream_id);
  if (stream) {
    logger_->UpdateReceivedFrameCounts(stream_id, stream->num_frames_received(),
                                       stream->num_duplicate_frames_received());
  }
  QuicSpdySession::CloseStream(stream_id);
  OnClosedStream();
}

void QuicChromiumClientSession::SendRstStream(QuicStreamId id,
                                              QuicRstStreamErrorCode error,
                                              QuicStreamOffset bytes_written) {
  QuicSpdySession::SendRstStream(id, error, bytes_written);
  OnClosedStream();
}

void QuicChromiumClientSession::OnClosedStream() {
  if (GetNumOpenOutgoingStreams() < max_open_outgoing_streams() &&
      !stream_requests_.empty() && crypto_stream_->encryption_established() &&
      !goaway_received() && !going_away_ && connection()->connected()) {
    StreamRequest* request = stream_requests_.front();
    stream_requests_.pop_front();
    request->OnRequestCompleteSuccess(CreateOutgoingReliableStreamImpl());
  }

  if (GetNumOpenOutgoingStreams() == 0) {
    stream_factory_->OnIdleSession(this);
  }
}

void QuicChromiumClientSession::OnCryptoHandshakeEvent(
    CryptoHandshakeEvent event) {
  if (stream_factory_ && event == HANDSHAKE_CONFIRMED &&
      (stream_factory_->OnHandshakeConfirmed(
          this, logger_->ReceivedPacketLossRate()))) {
    return;
  }

  if (!callback_.is_null() &&
      (!require_confirmation_ || event == HANDSHAKE_CONFIRMED ||
       event == ENCRYPTION_REESTABLISHED)) {
    // TODO(rtenneti): Currently for all CryptoHandshakeEvent events, callback_
    // could be called because there are no error events in CryptoHandshakeEvent
    // enum. If error events are added to CryptoHandshakeEvent, then the
    // following code needs to changed.
    base::ResetAndReturn(&callback_).Run(OK);
  }
  if (event == HANDSHAKE_CONFIRMED) {
    UMA_HISTOGRAM_TIMES("Net.QuicSession.HandshakeConfirmedTime",
                        base::TimeTicks::Now() - handshake_start_);
    if (server_info_) {
      // TODO(rtenneti): Should we delete this histogram?
      // Track how long it has taken to finish handshake once we start waiting
      // for reading of QUIC server information from disk cache. We could use
      // this data to compare total time taken if we were to cancel the disk
      // cache read vs waiting for the read to complete.
      base::TimeTicks wait_for_data_start_time =
          server_info_->wait_for_data_start_time();
      if (!wait_for_data_start_time.is_null()) {
        UMA_HISTOGRAM_TIMES(
            "Net.QuicServerInfo.WaitForDataReady.HandshakeConfirmedTime",
            base::TimeTicks::Now() - wait_for_data_start_time);
      }
    }
    // Track how long it has taken to finish handshake after we have finished
    // DNS host resolution.
    if (!dns_resolution_end_time_.is_null()) {
      UMA_HISTOGRAM_TIMES(
          "Net.QuicSession.HostResolution.HandshakeConfirmedTime",
          base::TimeTicks::Now() - dns_resolution_end_time_);
    }

    ObserverSet::iterator it = observers_.begin();
    while (it != observers_.end()) {
      Observer* observer = *it;
      ++it;
      observer->OnCryptoHandshakeConfirmed();
    }
    if (server_info_)
      server_info_->OnExternalCacheHit();
  }
  QuicSpdySession::OnCryptoHandshakeEvent(event);
}

void QuicChromiumClientSession::OnCryptoHandshakeMessageSent(
    const CryptoHandshakeMessage& message) {
  logger_->OnCryptoHandshakeMessageSent(message);

  if (message.tag() == kREJ || message.tag() == kSREJ) {
    UMA_HISTOGRAM_CUSTOM_COUNTS("Net.QuicSession.RejectLength",
                                message.GetSerialized().length(), 1000, 10000,
                                50);
  }
}

void QuicChromiumClientSession::OnCryptoHandshakeMessageReceived(
    const CryptoHandshakeMessage& message) {
  logger_->OnCryptoHandshakeMessageReceived(message);
}

void QuicChromiumClientSession::OnGoAway(const QuicGoAwayFrame& frame) {
  QuicSession::OnGoAway(frame);
  NotifyFactoryOfSessionGoingAway();
}

void QuicChromiumClientSession::OnRstStream(const QuicRstStreamFrame& frame) {
  QuicSession::OnRstStream(frame);
  OnClosedStream();
}

void QuicChromiumClientSession::OnConnectionClosed(
    QuicErrorCode error,
    ConnectionCloseSource source) {
  DCHECK(!connection()->connected());
  logger_->OnConnectionClosed(error, source);
  if (source == ConnectionCloseSource::FROM_PEER) {
    if (IsCryptoHandshakeConfirmed()) {
      UMA_HISTOGRAM_SPARSE_SLOWLY(
          "Net.QuicSession.ConnectionCloseErrorCodeServer.HandshakeConfirmed",
          error);
    }
    UMA_HISTOGRAM_SPARSE_SLOWLY(
        "Net.QuicSession.ConnectionCloseErrorCodeServer", error);
  } else {
    if (IsCryptoHandshakeConfirmed()) {
      UMA_HISTOGRAM_SPARSE_SLOWLY(
          "Net.QuicSession.ConnectionCloseErrorCodeClient.HandshakeConfirmed",
          error);
    }
    UMA_HISTOGRAM_SPARSE_SLOWLY(
        "Net.QuicSession.ConnectionCloseErrorCodeClient", error);
  }

  if (error == QUIC_NETWORK_IDLE_TIMEOUT) {
    UMA_HISTOGRAM_COUNTS(
        "Net.QuicSession.ConnectionClose.NumOpenStreams.TimedOut",
        GetNumOpenOutgoingStreams());
    if (IsCryptoHandshakeConfirmed()) {
      if (GetNumOpenOutgoingStreams() > 0) {
        disabled_reason_ = QUIC_DISABLED_TIMEOUT_WITH_OPEN_STREAMS;
        UMA_HISTOGRAM_BOOLEAN(
            "Net.QuicSession.TimedOutWithOpenStreams.HasUnackedPackets",
            connection()->sent_packet_manager().HasUnackedPackets());
        UMA_HISTOGRAM_COUNTS(
            "Net.QuicSession.TimedOutWithOpenStreams.ConsecutiveRTOCount",
            connection()->sent_packet_manager().consecutive_rto_count());
        UMA_HISTOGRAM_COUNTS(
            "Net.QuicSession.TimedOutWithOpenStreams.ConsecutiveTLPCount",
            connection()->sent_packet_manager().consecutive_tlp_count());
      }
      if (connection()->sent_packet_manager().HasUnackedPackets()) {
        UMA_HISTOGRAM_TIMES(
            "Net.QuicSession.LocallyTimedOutWithOpenStreams."
            "TimeSinceLastReceived.UnackedPackets",
            NetworkActivityMonitor::GetInstance()->GetTimeSinceLastReceived());
      } else {
        UMA_HISTOGRAM_TIMES(
            "Net.QuicSession.LocallyTimedOutWithOpenStreams."
            "TimeSinceLastReceived.NoUnackedPackets",
            NetworkActivityMonitor::GetInstance()->GetTimeSinceLastReceived());
      }

    } else {
      UMA_HISTOGRAM_COUNTS(
          "Net.QuicSession.ConnectionClose.NumOpenStreams.HandshakeTimedOut",
          GetNumOpenOutgoingStreams());
      UMA_HISTOGRAM_COUNTS(
          "Net.QuicSession.ConnectionClose.NumTotalStreams.HandshakeTimedOut",
          num_total_streams_);
    }
  }

  if (!IsCryptoHandshakeConfirmed()) {
    if (error == QUIC_PUBLIC_RESET) {
      RecordHandshakeFailureReason(HANDSHAKE_FAILURE_PUBLIC_RESET);
    } else if (connection()->GetStats().packets_received == 0) {
      RecordHandshakeFailureReason(HANDSHAKE_FAILURE_BLACK_HOLE);
      UMA_HISTOGRAM_SPARSE_SLOWLY(
          "Net.QuicSession.ConnectionClose.HandshakeFailureBlackHole.QuicError",
          error);
    } else {
      RecordHandshakeFailureReason(HANDSHAKE_FAILURE_UNKNOWN);
      UMA_HISTOGRAM_SPARSE_SLOWLY(
          "Net.QuicSession.ConnectionClose.HandshakeFailureUnknown.QuicError",
          error);
    }
  } else if (error == QUIC_PUBLIC_RESET) {
    disabled_reason_ = QUIC_DISABLED_PUBLIC_RESET_POST_HANDSHAKE;
  }

  UMA_HISTOGRAM_SPARSE_SLOWLY("Net.QuicSession.QuicVersion",
                              connection()->version());
  NotifyFactoryOfSessionGoingAway();
  QuicSession::OnConnectionClosed(error, source);

  if (!callback_.is_null()) {
    base::ResetAndReturn(&callback_).Run(ERR_QUIC_PROTOCOL_ERROR);
  }

  for (auto& socket : sockets_) {
    socket->Close();
  }
  DCHECK(dynamic_streams().empty());
  CloseAllStreams(ERR_UNEXPECTED);
  CloseAllObservers(ERR_UNEXPECTED);
  NotifyFactoryOfSessionClosedLater();
}

void QuicChromiumClientSession::OnSuccessfulVersionNegotiation(
    const QuicVersion& version) {
  logger_->OnSuccessfulVersionNegotiation(version);
  QuicSpdySession::OnSuccessfulVersionNegotiation(version);
}

void QuicChromiumClientSession::OnPathDegrading() {
  stream_factory_->MaybeMigrateSessionEarly(this);
}

void QuicChromiumClientSession::OnProofValid(
    const QuicCryptoClientConfig::CachedState& cached) {
  DCHECK(cached.proof_valid());

  if (!server_info_) {
    return;
  }

  QuicServerInfo::State* state = server_info_->mutable_state();

  state->server_config = cached.server_config();
  state->source_address_token = cached.source_address_token();
  state->server_config_sig = cached.signature();
  state->certs = cached.certs();

  server_info_->Persist();
}

void QuicChromiumClientSession::OnProofVerifyDetailsAvailable(
    const ProofVerifyDetails& verify_details) {
  const ProofVerifyDetailsChromium* verify_details_chromium =
      reinterpret_cast<const ProofVerifyDetailsChromium*>(&verify_details);
  cert_verify_result_.reset(new CertVerifyResult);
  cert_verify_result_->CopyFrom(verify_details_chromium->cert_verify_result);
  pinning_failure_log_ = verify_details_chromium->pinning_failure_log;
  scoped_ptr<ct::CTVerifyResult> ct_verify_result_copy(
      new ct::CTVerifyResult(verify_details_chromium->ct_verify_result));
  ct_verify_result_ = std::move(ct_verify_result_copy);
  logger_->OnCertificateVerified(*cert_verify_result_);
}

void QuicChromiumClientSession::StartReading() {
  for (auto& packet_reader : packet_readers_) {
    packet_reader->StartReading();
  }
}

void QuicChromiumClientSession::CloseSessionOnError(int error,
                                                    QuicErrorCode quic_error) {
  RecordAndCloseSessionOnError(error, quic_error);
  NotifyFactoryOfSessionClosed();
}

void QuicChromiumClientSession::CloseSessionOnErrorAndNotifyFactoryLater(
    int error,
    QuicErrorCode quic_error) {
  RecordAndCloseSessionOnError(error, quic_error);
  NotifyFactoryOfSessionClosedLater();
}

void QuicChromiumClientSession::RecordAndCloseSessionOnError(
    int error,
    QuicErrorCode quic_error) {
  UMA_HISTOGRAM_SPARSE_SLOWLY("Net.QuicSession.CloseSessionOnError", -error);
  CloseSessionOnErrorInner(error, quic_error);
}

void QuicChromiumClientSession::CloseSessionOnErrorInner(
    int net_error,
    QuicErrorCode quic_error) {
  if (!callback_.is_null()) {
    base::ResetAndReturn(&callback_).Run(net_error);
  }
  CloseAllStreams(net_error);
  CloseAllObservers(net_error);
  net_log_.AddEvent(NetLog::TYPE_QUIC_SESSION_CLOSE_ON_ERROR,
                    NetLog::IntCallback("net_error", net_error));

  if (connection()->connected())
    connection()->CloseConnection(quic_error, ConnectionCloseSource::FROM_SELF);
  DCHECK(!connection()->connected());
}

void QuicChromiumClientSession::CloseAllStreams(int net_error) {
  while (!dynamic_streams().empty()) {
    ReliableQuicStream* stream = dynamic_streams().begin()->second;
    QuicStreamId id = stream->id();
    static_cast<QuicChromiumClientStream*>(stream)->OnError(net_error);
    CloseStream(id);
  }
}

void QuicChromiumClientSession::CloseAllObservers(int net_error) {
  while (!observers_.empty()) {
    Observer* observer = *observers_.begin();
    observers_.erase(observer);
    observer->OnSessionClosed(net_error);
  }
}

scoped_ptr<base::Value> QuicChromiumClientSession::GetInfoAsValue(
    const std::set<HostPortPair>& aliases) {
  scoped_ptr<base::DictionaryValue> dict(new base::DictionaryValue());
  dict->SetString("version", QuicVersionToString(connection()->version()));
  dict->SetInteger("open_streams", GetNumOpenOutgoingStreams());
  scoped_ptr<base::ListValue> stream_list(new base::ListValue());
  for (StreamMap::const_iterator it = dynamic_streams().begin();
       it != dynamic_streams().end(); ++it) {
    stream_list->Append(
        new base::StringValue(base::UintToString(it->second->id())));
  }
  dict->Set("active_streams", std::move(stream_list));

  dict->SetInteger("total_streams", num_total_streams_);
  dict->SetString("peer_address", peer_address().ToString());
  dict->SetString("connection_id", base::Uint64ToString(connection_id()));
  dict->SetBoolean("connected", connection()->connected());
  const QuicConnectionStats& stats = connection()->GetStats();
  dict->SetInteger("packets_sent", stats.packets_sent);
  dict->SetInteger("packets_received", stats.packets_received);
  dict->SetInteger("packets_lost", stats.packets_lost);
  SSLInfo ssl_info;
  dict->SetBoolean("secure", GetSSLInfo(&ssl_info) && ssl_info.cert.get());

  scoped_ptr<base::ListValue> alias_list(new base::ListValue());
  for (std::set<HostPortPair>::const_iterator it = aliases.begin();
       it != aliases.end(); it++) {
    alias_list->Append(new base::StringValue(it->ToString()));
  }
  dict->Set("aliases", std::move(alias_list));

  return std::move(dict);
}

base::WeakPtr<QuicChromiumClientSession>
QuicChromiumClientSession::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

void QuicChromiumClientSession::OnReadError(
    int result,
    const DatagramClientSocket* socket) {
  DCHECK(socket != nullptr);
  if (socket != GetDefaultSocket()) {
    // Ignore read errors from old sockets that are no longer active.
    // TODO(jri): Maybe clean up old sockets on error.
    return;
  }
  DVLOG(1) << "Closing session on read error: " << result;
  UMA_HISTOGRAM_SPARSE_SLOWLY("Net.QuicSession.ReadError", -result);
  NotifyFactoryOfSessionGoingAway();
  CloseSessionOnErrorInner(result, QUIC_PACKET_READ_ERROR);
  NotifyFactoryOfSessionClosedLater();
}

bool QuicChromiumClientSession::OnPacket(const QuicEncryptedPacket& packet,
                                         IPEndPoint local_address,
                                         IPEndPoint peer_address) {
  ProcessUdpPacket(local_address, peer_address, packet);
  if (!connection()->connected()) {
    NotifyFactoryOfSessionClosedLater();
    return false;
  }
  return true;
}

void QuicChromiumClientSession::NotifyFactoryOfSessionGoingAway() {
  going_away_ = true;
  if (stream_factory_)
    stream_factory_->OnSessionGoingAway(this);
}

void QuicChromiumClientSession::NotifyFactoryOfSessionClosedLater() {
  if (!dynamic_streams().empty())
    RecordUnexpectedOpenStreams(NOTIFY_FACTORY_OF_SESSION_CLOSED_LATER);

  if (!going_away_)
    RecordUnexpectedNotGoingAway(NOTIFY_FACTORY_OF_SESSION_CLOSED_LATER);

  going_away_ = true;
  DCHECK_EQ(0u, GetNumActiveStreams());
  DCHECK(!connection()->connected());
  base::ThreadTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::Bind(&QuicChromiumClientSession::NotifyFactoryOfSessionClosed,
                 weak_factory_.GetWeakPtr()));
}

void QuicChromiumClientSession::NotifyFactoryOfSessionClosed() {
  if (!dynamic_streams().empty())
    RecordUnexpectedOpenStreams(NOTIFY_FACTORY_OF_SESSION_CLOSED);

  if (!going_away_)
    RecordUnexpectedNotGoingAway(NOTIFY_FACTORY_OF_SESSION_CLOSED);

  going_away_ = true;
  DCHECK_EQ(0u, GetNumActiveStreams());
  // Will delete |this|.
  if (stream_factory_)
    stream_factory_->OnSessionClosed(this);
}

void QuicChromiumClientSession::OnConnectTimeout() {
  DCHECK(callback_.is_null());
  DCHECK(IsEncryptionEstablished());

  if (IsCryptoHandshakeConfirmed())
    return;

  // TODO(rch): re-enable this code once beta is cut.
  //  if (stream_factory_)
  //    stream_factory_->OnSessionConnectTimeout(this);
  //  CloseAllStreams(ERR_QUIC_HANDSHAKE_FAILED);
  //  DCHECK_EQ(0u, GetNumOpenOutgoingStreams());
}

bool QuicChromiumClientSession::MigrateToSocket(
    scoped_ptr<DatagramClientSocket> socket,
    scoped_ptr<QuicChromiumPacketReader> reader,
    scoped_ptr<QuicPacketWriter> writer) {
  DCHECK_EQ(sockets_.size(), packet_readers_.size());
  if (sockets_.size() >= kMaxReadersPerQuicSession) {
    return false;
  }
  // TODO(jri): Make SetQuicPacketWriter take a scoped_ptr.
  connection()->SetQuicPacketWriter(writer.release(), /*owns_writer=*/true);
  packet_readers_.push_back(std::move(reader));
  sockets_.push_back(std::move(socket));
  StartReading();
  connection()->SendPing();
  return true;
}

const DatagramClientSocket* QuicChromiumClientSession::GetDefaultSocket()
    const {
  DCHECK(sockets_.back().get() != nullptr);
  // The most recently added socket is the currently active one.
  return sockets_.back().get();
}

bool QuicChromiumClientSession::IsAuthorized(const std::string& hostname) {
  return CanPool(hostname, server_id_.privacy_mode());
}

}  // namespace net
