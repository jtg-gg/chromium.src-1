// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/socket/ssl_server_socket_openssl.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <utility>

#include "base/callback_helpers.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "crypto/openssl_util.h"
#include "crypto/rsa_private_key.h"
#include "crypto/scoped_openssl_types.h"
#include "net/base/net_errors.h"
#include "net/cert/cert_verify_result.h"
#include "net/cert/client_cert_verifier.h"
#include "net/cert/x509_util_openssl.h"
#include "net/ssl/openssl_ssl_util.h"
#include "net/ssl/scoped_openssl_types.h"
#include "net/ssl/ssl_connection_status_flags.h"
#include "net/ssl/ssl_info.h"

#define GotoState(s) next_handshake_state_ = s

namespace net {

namespace {

// Creates an X509Certificate out of the concatenation of |cert|, if non-null,
// with |chain|.
scoped_refptr<X509Certificate> CreateX509Certificate(X509* cert,
                                                     STACK_OF(X509) * chain) {
  std::vector<base::StringPiece> der_chain;
  base::StringPiece der_cert;
  scoped_refptr<X509Certificate> client_cert;
  if (cert) {
    if (!x509_util::GetDER(cert, &der_cert))
      return nullptr;
    der_chain.push_back(der_cert);
  }

  for (size_t i = 0; i < sk_X509_num(chain); ++i) {
    X509* x = sk_X509_value(chain, i);
    if (!x509_util::GetDER(x, &der_cert))
      return nullptr;
    der_chain.push_back(der_cert);
  }

  return X509Certificate::CreateFromDERCertChain(der_chain);
}

}  // namespace

void EnableSSLServerSockets() {
  // No-op because CreateSSLServerSocket() calls crypto::EnsureOpenSSLInit().
}

scoped_ptr<SSLServerSocket> CreateSSLServerSocket(
    scoped_ptr<StreamSocket> socket,
    X509Certificate* certificate,
    const crypto::RSAPrivateKey& key,
    const SSLServerConfig& ssl_server_config) {
  crypto::EnsureOpenSSLInit();
  return scoped_ptr<SSLServerSocket>(new SSLServerSocketOpenSSL(
      std::move(socket), certificate, key, ssl_server_config));
}

SSLServerSocketOpenSSL::SSLServerSocketOpenSSL(
    scoped_ptr<StreamSocket> transport_socket,
    scoped_refptr<X509Certificate> certificate,
    const crypto::RSAPrivateKey& key,
    const SSLServerConfig& ssl_server_config)
    : transport_send_busy_(false),
      transport_recv_busy_(false),
      transport_recv_eof_(false),
      user_read_buf_len_(0),
      user_write_buf_len_(0),
      transport_write_error_(OK),
      ssl_(NULL),
      transport_bio_(NULL),
      transport_socket_(std::move(transport_socket)),
      ssl_server_config_(ssl_server_config),
      cert_(certificate),
      key_(key.Copy()),
      next_handshake_state_(STATE_NONE),
      completed_handshake_(false) {
  CHECK(key_);
}

SSLServerSocketOpenSSL::~SSLServerSocketOpenSSL() {
  if (ssl_) {
    // Calling SSL_shutdown prevents the session from being marked as
    // unresumable.
    SSL_shutdown(ssl_);
    SSL_free(ssl_);
    ssl_ = NULL;
  }
  if (transport_bio_) {
    BIO_free_all(transport_bio_);
    transport_bio_ = NULL;
  }
}

int SSLServerSocketOpenSSL::Handshake(const CompletionCallback& callback) {
  net_log_.BeginEvent(NetLog::TYPE_SSL_SERVER_HANDSHAKE);

  // Set up new ssl object.
  int rv = Init();
  if (rv != OK) {
    LOG(ERROR) << "Failed to initialize OpenSSL: rv=" << rv;
    net_log_.EndEventWithNetErrorCode(NetLog::TYPE_SSL_SERVER_HANDSHAKE, rv);
    return rv;
  }

  // Set SSL to server mode. Handshake happens in the loop below.
  SSL_set_accept_state(ssl_);

  GotoState(STATE_HANDSHAKE);
  rv = DoHandshakeLoop(OK);
  if (rv == ERR_IO_PENDING) {
    user_handshake_callback_ = callback;
  } else {
    net_log_.EndEventWithNetErrorCode(NetLog::TYPE_SSL_SERVER_HANDSHAKE, rv);
  }

  return rv > OK ? OK : rv;
}

int SSLServerSocketOpenSSL::ExportKeyingMaterial(
    const base::StringPiece& label,
    bool has_context,
    const base::StringPiece& context,
    unsigned char* out,
    unsigned int outlen) {
  if (!IsConnected())
    return ERR_SOCKET_NOT_CONNECTED;

  crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);

  int rv = SSL_export_keying_material(
      ssl_, out, outlen, label.data(), label.size(),
      reinterpret_cast<const unsigned char*>(context.data()),
      context.length(), context.length() > 0);

  if (rv != 1) {
    int ssl_error = SSL_get_error(ssl_, rv);
    LOG(ERROR) << "Failed to export keying material;"
               << " returned " << rv
               << ", SSL error code " << ssl_error;
    return MapOpenSSLError(ssl_error, err_tracer);
  }
  return OK;
}

int SSLServerSocketOpenSSL::GetTLSUniqueChannelBinding(std::string* out) {
  NOTIMPLEMENTED();
  return ERR_NOT_IMPLEMENTED;
}

int SSLServerSocketOpenSSL::Read(IOBuffer* buf, int buf_len,
                                 const CompletionCallback& callback) {
  DCHECK(user_read_callback_.is_null());
  DCHECK(user_handshake_callback_.is_null());
  DCHECK(!user_read_buf_);
  DCHECK(!callback.is_null());

  user_read_buf_ = buf;
  user_read_buf_len_ = buf_len;

  DCHECK(completed_handshake_);

  int rv = DoReadLoop(OK);

  if (rv == ERR_IO_PENDING) {
    user_read_callback_ = callback;
  } else {
    user_read_buf_ = NULL;
    user_read_buf_len_ = 0;
  }

  return rv;
}

int SSLServerSocketOpenSSL::Write(IOBuffer* buf, int buf_len,
                                  const CompletionCallback& callback) {
  DCHECK(user_write_callback_.is_null());
  DCHECK(!user_write_buf_);
  DCHECK(!callback.is_null());

  user_write_buf_ = buf;
  user_write_buf_len_ = buf_len;

  int rv = DoWriteLoop(OK);

  if (rv == ERR_IO_PENDING) {
    user_write_callback_ = callback;
  } else {
    user_write_buf_ = NULL;
    user_write_buf_len_ = 0;
  }
  return rv;
}

int SSLServerSocketOpenSSL::SetReceiveBufferSize(int32_t size) {
  return transport_socket_->SetReceiveBufferSize(size);
}

int SSLServerSocketOpenSSL::SetSendBufferSize(int32_t size) {
  return transport_socket_->SetSendBufferSize(size);
}

int SSLServerSocketOpenSSL::Connect(const CompletionCallback& callback) {
  NOTIMPLEMENTED();
  return ERR_NOT_IMPLEMENTED;
}

void SSLServerSocketOpenSSL::Disconnect() {
  transport_socket_->Disconnect();
}

bool SSLServerSocketOpenSSL::IsConnected() const {
  // TODO(wtc): Find out if we should check transport_socket_->IsConnected()
  // as well.
  return completed_handshake_;
}

bool SSLServerSocketOpenSSL::IsConnectedAndIdle() const {
  return completed_handshake_ && transport_socket_->IsConnectedAndIdle();
}

int SSLServerSocketOpenSSL::GetPeerAddress(IPEndPoint* address) const {
  if (!IsConnected())
    return ERR_SOCKET_NOT_CONNECTED;
  return transport_socket_->GetPeerAddress(address);
}

int SSLServerSocketOpenSSL::GetLocalAddress(IPEndPoint* address) const {
  if (!IsConnected())
    return ERR_SOCKET_NOT_CONNECTED;
  return transport_socket_->GetLocalAddress(address);
}

const BoundNetLog& SSLServerSocketOpenSSL::NetLog() const {
  return net_log_;
}

void SSLServerSocketOpenSSL::SetSubresourceSpeculation() {
  transport_socket_->SetSubresourceSpeculation();
}

void SSLServerSocketOpenSSL::SetOmniboxSpeculation() {
  transport_socket_->SetOmniboxSpeculation();
}

bool SSLServerSocketOpenSSL::WasEverUsed() const {
  return transport_socket_->WasEverUsed();
}

bool SSLServerSocketOpenSSL::UsingTCPFastOpen() const {
  return transport_socket_->UsingTCPFastOpen();
}

bool SSLServerSocketOpenSSL::WasNpnNegotiated() const {
  NOTIMPLEMENTED();
  return false;
}

NextProto SSLServerSocketOpenSSL::GetNegotiatedProtocol() const {
  // NPN is not supported by this class.
  return kProtoUnknown;
}

bool SSLServerSocketOpenSSL::GetSSLInfo(SSLInfo* ssl_info) {
  ssl_info->Reset();
  if (!completed_handshake_)
    return false;

  ssl_info->cert = client_cert_;

  const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl_);
  CHECK(cipher);
  ssl_info->security_bits = SSL_CIPHER_get_bits(cipher, NULL);

  SSLConnectionStatusSetCipherSuite(
      static_cast<uint16_t>(SSL_CIPHER_get_id(cipher)),
      &ssl_info->connection_status);
  SSLConnectionStatusSetVersion(GetNetSSLVersion(ssl_),
                                &ssl_info->connection_status);

  if (!SSL_get_secure_renegotiation_support(ssl_))
    ssl_info->connection_status |= SSL_CONNECTION_NO_RENEGOTIATION_EXTENSION;

  ssl_info->handshake_type = SSL_session_reused(ssl_)
                                 ? SSLInfo::HANDSHAKE_RESUME
                                 : SSLInfo::HANDSHAKE_FULL;

  return true;
}

void SSLServerSocketOpenSSL::GetConnectionAttempts(
    ConnectionAttempts* out) const {
  out->clear();
}

int64_t SSLServerSocketOpenSSL::GetTotalReceivedBytes() const {
  return transport_socket_->GetTotalReceivedBytes();
}

void SSLServerSocketOpenSSL::OnSendComplete(int result) {
  if (next_handshake_state_ == STATE_HANDSHAKE) {
    // In handshake phase.
    OnHandshakeIOComplete(result);
    return;
  }

  // TODO(byungchul): This state machine is not correct. Copy the state machine
  // of SSLClientSocketOpenSSL::OnSendComplete() which handles it better.
  if (!completed_handshake_)
    return;

  if (user_write_buf_) {
    int rv = DoWriteLoop(result);
    if (rv != ERR_IO_PENDING)
      DoWriteCallback(rv);
  } else {
    // Ensure that any queued ciphertext is flushed.
    DoTransportIO();
  }
}

void SSLServerSocketOpenSSL::OnRecvComplete(int result) {
  if (next_handshake_state_ == STATE_HANDSHAKE) {
    // In handshake phase.
    OnHandshakeIOComplete(result);
    return;
  }

  // Network layer received some data, check if client requested to read
  // decrypted data.
  if (!user_read_buf_ || !completed_handshake_)
    return;

  int rv = DoReadLoop(result);
  if (rv != ERR_IO_PENDING)
    DoReadCallback(rv);
}

void SSLServerSocketOpenSSL::OnHandshakeIOComplete(int result) {
  int rv = DoHandshakeLoop(result);
  if (rv == ERR_IO_PENDING)
    return;

  net_log_.EndEventWithNetErrorCode(NetLog::TYPE_SSL_SERVER_HANDSHAKE, rv);
  if (!user_handshake_callback_.is_null())
    DoHandshakeCallback(rv);
}

// Return 0 for EOF,
// > 0 for bytes transferred immediately,
// < 0 for error (or the non-error ERR_IO_PENDING).
int SSLServerSocketOpenSSL::BufferSend() {
  if (transport_send_busy_)
    return ERR_IO_PENDING;

  if (!send_buffer_) {
    // Get a fresh send buffer out of the send BIO.
    size_t max_read = BIO_pending(transport_bio_);
    if (!max_read)
      return 0;  // Nothing pending in the OpenSSL write BIO.
    send_buffer_ = new DrainableIOBuffer(new IOBuffer(max_read), max_read);
    int read_bytes = BIO_read(transport_bio_, send_buffer_->data(), max_read);
    DCHECK_GT(read_bytes, 0);
    CHECK_EQ(static_cast<int>(max_read), read_bytes);
  }

  int rv = transport_socket_->Write(
      send_buffer_.get(),
      send_buffer_->BytesRemaining(),
      base::Bind(&SSLServerSocketOpenSSL::BufferSendComplete,
                 base::Unretained(this)));
  if (rv == ERR_IO_PENDING) {
    transport_send_busy_ = true;
  } else {
    TransportWriteComplete(rv);
  }
  return rv;
}

void SSLServerSocketOpenSSL::BufferSendComplete(int result) {
  transport_send_busy_ = false;
  TransportWriteComplete(result);
  OnSendComplete(result);
}

void SSLServerSocketOpenSSL::TransportWriteComplete(int result) {
  DCHECK(ERR_IO_PENDING != result);
  if (result < 0) {
    // Got a socket write error; close the BIO to indicate this upward.
    //
    // TODO(davidben): The value of |result| gets lost. Feed the error back into
    // the BIO so it gets (re-)detected in OnSendComplete. Perhaps with
    // BIO_set_callback.
    DVLOG(1) << "TransportWriteComplete error " << result;
    (void)BIO_shutdown_wr(SSL_get_wbio(ssl_));

    // Match the fix for http://crbug.com/249848 in NSS by erroring future reads
    // from the socket after a write error.
    //
    // TODO(davidben): Avoid having read and write ends interact this way.
    transport_write_error_ = result;
    (void)BIO_shutdown_wr(transport_bio_);
    send_buffer_ = NULL;
  } else {
    DCHECK(send_buffer_);
    send_buffer_->DidConsume(result);
    DCHECK_GE(send_buffer_->BytesRemaining(), 0);
    if (send_buffer_->BytesRemaining() <= 0)
      send_buffer_ = NULL;
  }
}

int SSLServerSocketOpenSSL::BufferRecv() {
  if (transport_recv_busy_)
    return ERR_IO_PENDING;

  // Determine how much was requested from |transport_bio_| that was not
  // actually available.
  size_t requested = BIO_ctrl_get_read_request(transport_bio_);
  if (requested == 0) {
    // This is not a perfect match of error codes, as no operation is
    // actually pending. However, returning 0 would be interpreted as
    // a possible sign of EOF, which is also an inappropriate match.
    return ERR_IO_PENDING;
  }

  // Known Issue: While only reading |requested| data is the more correct
  // implementation, it has the downside of resulting in frequent reads:
  // One read for the SSL record header (~5 bytes) and one read for the SSL
  // record body. Rather than issuing these reads to the underlying socket
  // (and constantly allocating new IOBuffers), a single Read() request to
  // fill |transport_bio_| is issued. As long as an SSL client socket cannot
  // be gracefully shutdown (via SSL close alerts) and re-used for non-SSL
  // traffic, this over-subscribed Read()ing will not cause issues.
  size_t max_write = BIO_ctrl_get_write_guarantee(transport_bio_);
  if (!max_write)
    return ERR_IO_PENDING;

  recv_buffer_ = new IOBuffer(max_write);
  int rv = transport_socket_->Read(
      recv_buffer_.get(),
      max_write,
      base::Bind(&SSLServerSocketOpenSSL::BufferRecvComplete,
                 base::Unretained(this)));
  if (rv == ERR_IO_PENDING) {
    transport_recv_busy_ = true;
  } else {
    rv = TransportReadComplete(rv);
  }
  return rv;
}

void SSLServerSocketOpenSSL::BufferRecvComplete(int result) {
  result = TransportReadComplete(result);
  OnRecvComplete(result);
}

int SSLServerSocketOpenSSL::TransportReadComplete(int result) {
  DCHECK(ERR_IO_PENDING != result);
  if (result <= 0) {
    DVLOG(1) << "TransportReadComplete result " << result;
    // Received 0 (end of file) or an error. Either way, bubble it up to the
    // SSL layer via the BIO. TODO(joth): consider stashing the error code, to
    // relay up to the SSL socket client (i.e. via DoReadCallback).
    if (result == 0)
      transport_recv_eof_ = true;
    (void)BIO_shutdown_wr(transport_bio_);
  } else if (transport_write_error_ < 0) {
    // Mirror transport write errors as read failures; transport_bio_ has been
    // shut down by TransportWriteComplete, so the BIO_write will fail, failing
    // the CHECK. http://crbug.com/335557.
    result = transport_write_error_;
  } else {
    DCHECK(recv_buffer_);
    int ret = BIO_write(transport_bio_, recv_buffer_->data(), result);
    // A write into a memory BIO should always succeed.
    DCHECK_EQ(result, ret);
  }
  recv_buffer_ = NULL;
  transport_recv_busy_ = false;
  return result;
}

// Do as much network I/O as possible between the buffer and the
// transport socket. Return true if some I/O performed, false
// otherwise (error or ERR_IO_PENDING).
bool SSLServerSocketOpenSSL::DoTransportIO() {
  bool network_moved = false;
  int rv;
  // Read and write as much data as possible. The loop is necessary because
  // Write() may return synchronously.
  do {
    rv = BufferSend();
    if (rv != ERR_IO_PENDING && rv != 0)
      network_moved = true;
  } while (rv > 0);
  if (!transport_recv_eof_ && BufferRecv() != ERR_IO_PENDING)
    network_moved = true;
  return network_moved;
}

int SSLServerSocketOpenSSL::DoPayloadRead() {
  DCHECK(user_read_buf_);
  DCHECK_GT(user_read_buf_len_, 0);
  crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
  int rv = SSL_read(ssl_, user_read_buf_->data(), user_read_buf_len_);
  if (rv >= 0)
    return rv;
  int ssl_error = SSL_get_error(ssl_, rv);
  OpenSSLErrorInfo error_info;
  int net_error = MapOpenSSLErrorWithDetails(ssl_error, err_tracer,
                                             &error_info);
  if (net_error != ERR_IO_PENDING) {
    net_log_.AddEvent(
        NetLog::TYPE_SSL_READ_ERROR,
        CreateNetLogOpenSSLErrorCallback(net_error, ssl_error, error_info));
  }
  return net_error;
}

int SSLServerSocketOpenSSL::DoPayloadWrite() {
  DCHECK(user_write_buf_);
  crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
  int rv = SSL_write(ssl_, user_write_buf_->data(), user_write_buf_len_);
  if (rv >= 0)
    return rv;
  int ssl_error = SSL_get_error(ssl_, rv);
  OpenSSLErrorInfo error_info;
  int net_error = MapOpenSSLErrorWithDetails(ssl_error, err_tracer,
                                             &error_info);
  if (net_error != ERR_IO_PENDING) {
    net_log_.AddEvent(
        NetLog::TYPE_SSL_WRITE_ERROR,
        CreateNetLogOpenSSLErrorCallback(net_error, ssl_error, error_info));
  }
  return net_error;
}

int SSLServerSocketOpenSSL::DoHandshakeLoop(int last_io_result) {
  int rv = last_io_result;
  do {
    // Default to STATE_NONE for next state.
    // (This is a quirk carried over from the windows
    // implementation.  It makes reading the logs a bit harder.)
    // State handlers can and often do call GotoState just
    // to stay in the current state.
    State state = next_handshake_state_;
    GotoState(STATE_NONE);
    switch (state) {
      case STATE_HANDSHAKE:
        rv = DoHandshake();
        break;
      case STATE_NONE:
      default:
        rv = ERR_UNEXPECTED;
        LOG(DFATAL) << "unexpected state " << state;
        break;
    }

    // Do the actual network I/O
    bool network_moved = DoTransportIO();
    if (network_moved && next_handshake_state_ == STATE_HANDSHAKE) {
      // In general we exit the loop if rv is ERR_IO_PENDING.  In this
      // special case we keep looping even if rv is ERR_IO_PENDING because
      // the transport IO may allow DoHandshake to make progress.
      rv = OK;  // This causes us to stay in the loop.
    }
  } while (rv != ERR_IO_PENDING && next_handshake_state_ != STATE_NONE);
  return rv;
}

int SSLServerSocketOpenSSL::DoReadLoop(int result) {
  DCHECK(completed_handshake_);
  DCHECK(next_handshake_state_ == STATE_NONE);

  if (result < 0)
    return result;

  bool network_moved;
  int rv;
  do {
    rv = DoPayloadRead();
    network_moved = DoTransportIO();
  } while (rv == ERR_IO_PENDING && network_moved);
  return rv;
}

int SSLServerSocketOpenSSL::DoWriteLoop(int result) {
  DCHECK(completed_handshake_);
  DCHECK_EQ(next_handshake_state_, STATE_NONE);

  if (result < 0)
    return result;

  bool network_moved;
  int rv;
  do {
    rv = DoPayloadWrite();
    network_moved = DoTransportIO();
  } while (rv == ERR_IO_PENDING && network_moved);
  return rv;
}

int SSLServerSocketOpenSSL::DoHandshake() {
  crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
  int net_error = OK;
  int rv = SSL_do_handshake(ssl_);

  if (rv == 1) {
    completed_handshake_ = true;
    // The results of SSL_get_peer_certificate() must be explicitly freed.
    ScopedX509 cert(SSL_get_peer_certificate(ssl_));
    if (cert) {
      // The caller does not take ownership of SSL_get_peer_cert_chain's
      // results.
      STACK_OF(X509)* chain = SSL_get_peer_cert_chain(ssl_);
      client_cert_ = CreateX509Certificate(cert.get(), chain);
      if (!client_cert_.get())
        return ERR_SSL_CLIENT_AUTH_CERT_BAD_FORMAT;
    }
  } else {
    int ssl_error = SSL_get_error(ssl_, rv);
    OpenSSLErrorInfo error_info;
    net_error = MapOpenSSLErrorWithDetails(ssl_error, err_tracer, &error_info);

    // This hack is necessary because the mapping of SSL error codes to
    // net_errors assumes (correctly for client sockets, but erroneously for
    // server sockets) that peer cert verification failure can only occur if
    // the cert changed during a renego. crbug.com/570351
    if (net_error == ERR_SSL_SERVER_CERT_CHANGED)
      net_error = ERR_BAD_SSL_CLIENT_AUTH_CERT;

    // If not done, stay in this state
    if (net_error == ERR_IO_PENDING) {
      GotoState(STATE_HANDSHAKE);
    } else {
      LOG(ERROR) << "handshake failed; returned " << rv
                 << ", SSL error code " << ssl_error
                 << ", net_error " << net_error;
      net_log_.AddEvent(
          NetLog::TYPE_SSL_HANDSHAKE_ERROR,
          CreateNetLogOpenSSLErrorCallback(net_error, ssl_error, error_info));
    }
  }
  return net_error;
}

void SSLServerSocketOpenSSL::DoHandshakeCallback(int rv) {
  DCHECK_NE(rv, ERR_IO_PENDING);
  base::ResetAndReturn(&user_handshake_callback_).Run(rv > OK ? OK : rv);
}

void SSLServerSocketOpenSSL::DoReadCallback(int rv) {
  DCHECK(rv != ERR_IO_PENDING);
  DCHECK(!user_read_callback_.is_null());

  user_read_buf_ = NULL;
  user_read_buf_len_ = 0;
  base::ResetAndReturn(&user_read_callback_).Run(rv);
}

void SSLServerSocketOpenSSL::DoWriteCallback(int rv) {
  DCHECK(rv != ERR_IO_PENDING);
  DCHECK(!user_write_callback_.is_null());

  user_write_buf_ = NULL;
  user_write_buf_len_ = 0;
  base::ResetAndReturn(&user_write_callback_).Run(rv);
}

int SSLServerSocketOpenSSL::Init() {
  DCHECK(!ssl_);
  DCHECK(!transport_bio_);

  crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);

  ScopedSSL_CTX ssl_ctx(SSL_CTX_new(TLS_method()));
  int verify_mode = 0;
  switch (ssl_server_config_.client_cert_type) {
    case SSLServerConfig::ClientCertType::REQUIRE_CLIENT_CERT:
      verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    // Fall-through
    case SSLServerConfig::ClientCertType::OPTIONAL_CLIENT_CERT:
      verify_mode |= SSL_VERIFY_PEER;
      SSL_CTX_set_verify(ssl_ctx.get(), verify_mode, nullptr);
      SSL_CTX_set_cert_verify_callback(ssl_ctx.get(), CertVerifyCallback,
                                       ssl_server_config_.client_cert_verifier);
      break;
    case SSLServerConfig::ClientCertType::NO_CLIENT_CERT:
      break;
  }
  ssl_ = SSL_new(ssl_ctx.get());
  if (!ssl_)
    return ERR_UNEXPECTED;

  BIO* ssl_bio = NULL;
  // 0 => use default buffer sizes.
  if (!BIO_new_bio_pair(&ssl_bio, 0, &transport_bio_, 0))
    return ERR_UNEXPECTED;
  DCHECK(ssl_bio);
  DCHECK(transport_bio_);

  SSL_set_bio(ssl_, ssl_bio, ssl_bio);

  // Set certificate and private key.
  DCHECK(cert_->os_cert_handle());
#if defined(USE_OPENSSL_CERTS)
  if (SSL_use_certificate(ssl_, cert_->os_cert_handle()) != 1) {
    LOG(ERROR) << "Cannot set certificate.";
    return ERR_UNEXPECTED;
  }
#else
  // Convert OSCertHandle to X509 structure.
  std::string der_string;
  if (!X509Certificate::GetDEREncoded(cert_->os_cert_handle(), &der_string))
    return ERR_UNEXPECTED;

  const unsigned char* der_string_array =
      reinterpret_cast<const unsigned char*>(der_string.data());

  ScopedX509 x509(d2i_X509(NULL, &der_string_array, der_string.length()));
  if (!x509)
    return ERR_UNEXPECTED;

  // On success, SSL_use_certificate acquires a reference to |x509|.
  if (SSL_use_certificate(ssl_, x509.get()) != 1) {
    LOG(ERROR) << "Cannot set certificate.";
    return ERR_UNEXPECTED;
  }
#endif  // USE_OPENSSL_CERTS

  DCHECK(key_->key());
  if (SSL_use_PrivateKey(ssl_, key_->key()) != 1) {
    LOG(ERROR) << "Cannot set private key.";
    return ERR_UNEXPECTED;
  }

  DCHECK_LT(SSL3_VERSION, ssl_server_config_.version_min);
  DCHECK_LT(SSL3_VERSION, ssl_server_config_.version_max);
  SSL_set_min_version(ssl_, ssl_server_config_.version_min);
  SSL_set_max_version(ssl_, ssl_server_config_.version_max);

  // OpenSSL defaults some options to on, others to off. To avoid ambiguity,
  // set everything we care about to an absolute value.
  SslSetClearMask options;
  options.ConfigureFlag(SSL_OP_NO_COMPRESSION, true);

  SSL_set_options(ssl_, options.set_mask);
  SSL_clear_options(ssl_, options.clear_mask);

  // Same as above, this time for the SSL mode.
  SslSetClearMask mode;

  mode.ConfigureFlag(SSL_MODE_RELEASE_BUFFERS, true);

  SSL_set_mode(ssl_, mode.set_mask);
  SSL_clear_mode(ssl_, mode.clear_mask);

  // See SSLServerConfig::disabled_cipher_suites for description of the suites
  // disabled by default. Note that !SHA256 and !SHA384 only remove HMAC-SHA256
  // and HMAC-SHA384 cipher suites, not GCM cipher suites with SHA256 or SHA384
  // as the handshake hash.
  std::string command("DEFAULT:!SHA256:!SHA384:!AESGCM+AES256:!aPSK");

  if (ssl_server_config_.require_ecdhe)
    command.append(":!kRSA:!kDHE");

  // Remove any disabled ciphers.
  for (uint16_t id : ssl_server_config_.disabled_cipher_suites) {
    const SSL_CIPHER* cipher = SSL_get_cipher_by_value(id);
    if (cipher) {
      command.append(":!");
      command.append(SSL_CIPHER_get_name(cipher));
    }
  }

  int rv = SSL_set_cipher_list(ssl_, command.c_str());
  // If this fails (rv = 0) it means there are no ciphers enabled on this SSL.
  // This will almost certainly result in the socket failing to complete the
  // handshake at which point the appropriate error is bubbled up to the client.
  LOG_IF(WARNING, rv != 1) << "SSL_set_cipher_list('" << command
                           << "') returned " << rv;

  if (ssl_server_config_.client_cert_type !=
          SSLServerConfig::ClientCertType::NO_CLIENT_CERT &&
      !ssl_server_config_.cert_authorities_.empty()) {
    ScopedX509NameStack stack(sk_X509_NAME_new_null());
    for (const auto& authority : ssl_server_config_.cert_authorities_) {
      const uint8_t* name = reinterpret_cast<const uint8_t*>(authority.c_str());
      const uint8_t* name_start = name;
      ScopedX509_NAME subj(d2i_X509_NAME(nullptr, &name, authority.length()));
      if (!subj || name != name_start + authority.length())
        return ERR_UNEXPECTED;
      sk_X509_NAME_push(stack.get(), subj.release());
    }
    SSL_set_client_CA_list(ssl_, stack.release());
  }

  return OK;
}

// static
int SSLServerSocketOpenSSL::CertVerifyCallback(X509_STORE_CTX* store_ctx,
                                               void* arg) {
  ClientCertVerifier* verifier = reinterpret_cast<ClientCertVerifier*>(arg);
  // If a verifier was not supplied, all certificates are accepted.
  if (!verifier)
    return 1;
  STACK_OF(X509)* chain = store_ctx->untrusted;
  scoped_refptr<X509Certificate> client_cert(
      CreateX509Certificate(nullptr, chain));
  if (!client_cert.get()) {
    X509_STORE_CTX_set_error(store_ctx, X509_V_ERR_CERT_REJECTED);
    return 0;
  }
  // Asynchronous completion of Verify is currently not supported.
  // http://crbug.com/347402
  // The API for Verify supports the parts needed for async completion
  // but is currently expected to complete synchronously.
  scoped_ptr<ClientCertVerifier::Request> ignore_async;
  int res =
      verifier->Verify(client_cert.get(), CompletionCallback(), &ignore_async);
  DCHECK_NE(res, ERR_IO_PENDING);

  if (res != OK) {
    X509_STORE_CTX_set_error(store_ctx, X509_V_ERR_CERT_REJECTED);
    return 0;
  }
  return 1;
}

}  // namespace net
