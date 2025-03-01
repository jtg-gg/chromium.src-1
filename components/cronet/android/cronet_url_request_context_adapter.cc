// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/cronet/android/cronet_url_request_context_adapter.h"

#include <limits.h>
#include <stddef.h>
#include <map>
#include <utility>

#include "base/android/jni_android.h"
#include "base/android/jni_array.h"
#include "base/android/jni_string.h"
#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/scoped_vector.h"
#include "base/message_loop/message_loop.h"
#include "base/metrics/statistics_recorder.h"
#include "base/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "components/cronet/histogram_manager.h"
#include "components/cronet/url_request_context_config.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_filter.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_service_factory.h"
#include "jni/CronetUrlRequestContext_jni.h"
#include "net/base/external_estimate_provider.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/base/network_delegate_impl.h"
#include "net/base/url_util.h"
#include "net/cert/cert_verifier.h"
#include "net/http/http_auth_handler_factory.h"
#include "net/http/http_server_properties_manager.h"
#include "net/log/write_to_file_net_log_observer.h"
#include "net/proxy/proxy_config_service_android.h"
#include "net/proxy/proxy_service.h"
#include "net/sdch/sdch_owner.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "net/url_request/url_request_interceptor.h"

#if defined(DATA_REDUCTION_PROXY_SUPPORT)
#include "components/cronet/android/cronet_data_reduction_proxy.h"
#endif

namespace {

const char kHttpServerProperties[] = "net.http_server_properties";

// Connects the HttpServerPropertiesManager's storage to the prefs.
class PrefServiceAdapter
    : public net::HttpServerPropertiesManager::PrefDelegate {
 public:
  explicit PrefServiceAdapter(PrefService* pref_service)
      : pref_service_(pref_service), path_(kHttpServerProperties) {
    pref_change_registrar_.Init(pref_service_);
  }

  ~PrefServiceAdapter() override {}

  // PrefDelegate implementation.
  bool HasServerProperties() override {
    return pref_service_->HasPrefPath(path_);
  }
  const base::DictionaryValue& GetServerProperties() const override {
    // Guaranteed not to return null when the pref is registered
    // (RegisterProfilePrefs was called).
    return *pref_service_->GetDictionary(path_);
  }
  void SetServerProperties(const base::DictionaryValue& value) override {
    return pref_service_->Set(path_, value);
  }
  void StartListeningForUpdates(const base::Closure& callback) override {
    pref_change_registrar_.Add(path_, callback);
  }
  void StopListeningForUpdates() override {
    pref_change_registrar_.RemoveAll();
  }

 private:
  PrefService* pref_service_;
  std::string path_;
  PrefChangeRegistrar pref_change_registrar_;

  DISALLOW_COPY_AND_ASSIGN(PrefServiceAdapter);
};

// Connects the SdchOwner's storage to the prefs.
class SdchOwnerPrefStorage : public net::SdchOwner::PrefStorage,
                             public PrefStore::Observer {
 public:
  explicit SdchOwnerPrefStorage(PersistentPrefStore* storage)
      : storage_(storage), storage_key_("SDCH"), init_observer_(nullptr) {}
  ~SdchOwnerPrefStorage() override {
    if (init_observer_)
      storage_->RemoveObserver(this);
  }

  ReadError GetReadError() const override {
    PersistentPrefStore::PrefReadError error = storage_->GetReadError();

    DCHECK_NE(
        error,
        PersistentPrefStore::PREF_READ_ERROR_ASYNCHRONOUS_TASK_INCOMPLETE);
    DCHECK_NE(error, PersistentPrefStore::PREF_READ_ERROR_MAX_ENUM);

    switch (error) {
      case PersistentPrefStore::PREF_READ_ERROR_NONE:
        return PERSISTENCE_FAILURE_NONE;

      case PersistentPrefStore::PREF_READ_ERROR_NO_FILE:
        return PERSISTENCE_FAILURE_REASON_NO_FILE;

      case PersistentPrefStore::PREF_READ_ERROR_JSON_PARSE:
      case PersistentPrefStore::PREF_READ_ERROR_JSON_TYPE:
      case PersistentPrefStore::PREF_READ_ERROR_FILE_OTHER:
      case PersistentPrefStore::PREF_READ_ERROR_FILE_LOCKED:
      case PersistentPrefStore::PREF_READ_ERROR_JSON_REPEAT:
        return PERSISTENCE_FAILURE_REASON_READ_FAILED;

      case PersistentPrefStore::PREF_READ_ERROR_ACCESS_DENIED:
      case PersistentPrefStore::PREF_READ_ERROR_FILE_NOT_SPECIFIED:
      case PersistentPrefStore::PREF_READ_ERROR_ASYNCHRONOUS_TASK_INCOMPLETE:
      case PersistentPrefStore::PREF_READ_ERROR_MAX_ENUM:
      default:
        // We don't expect these other failures given our usage of prefs.
        NOTREACHED();
        return PERSISTENCE_FAILURE_REASON_OTHER;
    }
  }

  bool GetValue(const base::DictionaryValue** result) const override {
    const base::Value* result_value = nullptr;
    if (!storage_->GetValue(storage_key_, &result_value))
      return false;
    return result_value->GetAsDictionary(result);
  }

  bool GetMutableValue(base::DictionaryValue** result) override {
    base::Value* result_value = nullptr;
    if (!storage_->GetMutableValue(storage_key_, &result_value))
      return false;
    return result_value->GetAsDictionary(result);
  }

  void SetValue(scoped_ptr<base::DictionaryValue> value) override {
    storage_->SetValue(storage_key_, std::move(value),
                       WriteablePrefStore::DEFAULT_PREF_WRITE_FLAGS);
  }

  void ReportValueChanged() override {
    storage_->ReportValueChanged(storage_key_,
                                 WriteablePrefStore::DEFAULT_PREF_WRITE_FLAGS);
  }

  bool IsInitializationComplete() override {
    return storage_->IsInitializationComplete();
  }

  void StartObservingInit(net::SdchOwner* observer) override {
    DCHECK(!init_observer_);
    init_observer_ = observer;
    storage_->AddObserver(this);
  }

  void StopObservingInit() override {
    DCHECK(init_observer_);
    init_observer_ = nullptr;
    storage_->RemoveObserver(this);
  }

 private:
  // PrefStore::Observer implementation.
  void OnPrefValueChanged(const std::string& key) override {}
  void OnInitializationCompleted(bool succeeded) override {
    init_observer_->OnPrefStorageInitializationComplete(succeeded);
  }

  PersistentPrefStore* storage_;  // Non-owning.
  const std::string storage_key_;

  net::SdchOwner* init_observer_;  // Non-owning.

  DISALLOW_COPY_AND_ASSIGN(SdchOwnerPrefStorage);
};

class BasicNetworkDelegate : public net::NetworkDelegateImpl {
 public:
  BasicNetworkDelegate() {}
  ~BasicNetworkDelegate() override {}

 private:
  // net::NetworkDelegate implementation.
  int OnBeforeURLRequest(net::URLRequest* request,
                         const net::CompletionCallback& callback,
                         GURL* new_url) override {
    return net::OK;
  }

  int OnBeforeSendHeaders(net::URLRequest* request,
                          const net::CompletionCallback& callback,
                          net::HttpRequestHeaders* headers) override {
    return net::OK;
  }

  void OnSendHeaders(net::URLRequest* request,
                     const net::HttpRequestHeaders& headers) override {}

  int OnHeadersReceived(
      net::URLRequest* request,
      const net::CompletionCallback& callback,
      const net::HttpResponseHeaders* original_response_headers,
      scoped_refptr<net::HttpResponseHeaders>* _response_headers,
      GURL* allowed_unsafe_redirect_url) override {
    return net::OK;
  }

  void OnBeforeRedirect(net::URLRequest* request,
                        const GURL& new_location) override {}

  void OnResponseStarted(net::URLRequest* request) override {}

  void OnCompleted(net::URLRequest* request, bool started) override {}

  void OnURLRequestDestroyed(net::URLRequest* request) override {}

  void OnPACScriptError(int line_number, const base::string16& error) override {
  }

  NetworkDelegate::AuthRequiredResponse OnAuthRequired(
      net::URLRequest* request,
      const net::AuthChallengeInfo& auth_info,
      const AuthCallback& callback,
      net::AuthCredentials* credentials) override {
    return net::NetworkDelegate::AUTH_REQUIRED_RESPONSE_NO_ACTION;
  }

  bool OnCanGetCookies(const net::URLRequest& request,
                       const net::CookieList& cookie_list) override {
    return false;
  }

  bool OnCanSetCookie(const net::URLRequest& request,
                      const std::string& cookie_line,
                      net::CookieOptions* options) override {
    return false;
  }

  bool OnCanAccessFile(const net::URLRequest& request,
                       const base::FilePath& path) const override {
    return false;
  }

  DISALLOW_COPY_AND_ASSIGN(BasicNetworkDelegate);
};

}  // namespace

namespace cronet {

// Explicitly register static JNI functions.
bool CronetUrlRequestContextAdapterRegisterJni(JNIEnv* env) {
  return RegisterNativesImpl(env);
}

CronetURLRequestContextAdapter::CronetURLRequestContextAdapter(
    scoped_ptr<URLRequestContextConfig> context_config)
    : network_thread_(new base::Thread("network")),
      http_server_properties_manager_(nullptr),
      context_config_(std::move(context_config)),
      is_context_initialized_(false),
      default_load_flags_(net::LOAD_NORMAL) {
  base::Thread::Options options;
  options.message_loop_type = base::MessageLoop::TYPE_IO;
  network_thread_->StartWithOptions(options);
}

CronetURLRequestContextAdapter::~CronetURLRequestContextAdapter() {
  DCHECK(GetNetworkTaskRunner()->BelongsToCurrentThread());

  if (http_server_properties_manager_)
    http_server_properties_manager_->ShutdownOnPrefThread();
  if (pref_service_)
    pref_service_->CommitPendingWrite();
  if (network_quality_estimator_) {
    network_quality_estimator_->RemoveRTTObserver(this);
    network_quality_estimator_->RemoveThroughputObserver(this);
  }
  StopNetLogOnNetworkThread();
}

void CronetURLRequestContextAdapter::InitRequestContextOnMainThread(
    JNIEnv* env,
    const JavaParamRef<jobject>& jcaller) {
  base::android::ScopedJavaGlobalRef<jobject> jcaller_ref;
  jcaller_ref.Reset(env, jcaller);
  proxy_config_service_ = net::ProxyService::CreateSystemProxyConfigService(
      GetNetworkTaskRunner(), nullptr /* Ignored on Android */);
  net::ProxyConfigServiceAndroid* android_proxy_config_service =
      static_cast<net::ProxyConfigServiceAndroid*>(proxy_config_service_.get());
  // If a PAC URL is present, ignore it and use the address and port of
  // Android system's local HTTP proxy server. See: crbug.com/432539.
  // TODO(csharrison) Architect the wrapper better so we don't need to cast for
  // android ProxyConfigServices.
  android_proxy_config_service->set_exclude_pac_url(true);
  GetNetworkTaskRunner()->PostTask(
      FROM_HERE,
      base::Bind(&CronetURLRequestContextAdapter::InitializeOnNetworkThread,
                 base::Unretained(this), base::Passed(&context_config_),
                 jcaller_ref));
}

void CronetURLRequestContextAdapter::
    EnableNetworkQualityEstimatorOnNetworkThread(bool use_local_host_requests,
                                                 bool use_smaller_responses) {
  DCHECK(GetNetworkTaskRunner()->BelongsToCurrentThread());
  DCHECK(!network_quality_estimator_);
  network_quality_estimator_.reset(new net::NetworkQualityEstimator(
      scoped_ptr<net::ExternalEstimateProvider>(),
      std::map<std::string, std::string>(), use_local_host_requests,
      use_smaller_responses));
  context_->set_network_quality_estimator(network_quality_estimator_.get());
}

void CronetURLRequestContextAdapter::EnableNetworkQualityEstimator(
    JNIEnv* env,
    const JavaParamRef<jobject>& jcaller,
    jboolean use_local_host_requests,
    jboolean use_smaller_responses) {
  PostTaskToNetworkThread(
      FROM_HERE, base::Bind(&CronetURLRequestContextAdapter::
                                EnableNetworkQualityEstimatorOnNetworkThread,
                            base::Unretained(this), use_local_host_requests,
                            use_smaller_responses));
}

void CronetURLRequestContextAdapter::ProvideRTTObservationsOnNetworkThread(
    bool should) {
  DCHECK(GetNetworkTaskRunner()->BelongsToCurrentThread());
  if (!network_quality_estimator_)
    return;
  if (should) {
    network_quality_estimator_->AddRTTObserver(this);
  } else {
    network_quality_estimator_->RemoveRTTObserver(this);
  }
}

void CronetURLRequestContextAdapter::ProvideRTTObservations(
    JNIEnv* env,
    const JavaParamRef<jobject>& jcaller,
    bool should) {
  PostTaskToNetworkThread(FROM_HERE,
                          base::Bind(&CronetURLRequestContextAdapter::
                                         ProvideRTTObservationsOnNetworkThread,
                                     base::Unretained(this), should));
}

void CronetURLRequestContextAdapter::
    ProvideThroughputObservationsOnNetworkThread(bool should) {
  DCHECK(GetNetworkTaskRunner()->BelongsToCurrentThread());
  if (!network_quality_estimator_)
    return;
  if (should) {
    network_quality_estimator_->AddThroughputObserver(this);
  } else {
    network_quality_estimator_->RemoveThroughputObserver(this);
  }
}

void CronetURLRequestContextAdapter::ProvideThroughputObservations(
    JNIEnv* env,
    const JavaParamRef<jobject>& jcaller,
    bool should) {
  PostTaskToNetworkThread(
      FROM_HERE, base::Bind(&CronetURLRequestContextAdapter::
                                ProvideThroughputObservationsOnNetworkThread,
                            base::Unretained(this), should));
}

void CronetURLRequestContextAdapter::InitializeOnNetworkThread(
    scoped_ptr<URLRequestContextConfig> config,
    const base::android::ScopedJavaGlobalRef<jobject>&
        jcronet_url_request_context) {
  DCHECK(GetNetworkTaskRunner()->BelongsToCurrentThread());
  DCHECK(!is_context_initialized_);
  DCHECK(proxy_config_service_);
  // TODO(mmenke):  Add method to have the builder enable SPDY.
  net::URLRequestContextBuilder context_builder;

  // TODO(mef): Remove this work around for crbug.com/543366 once it is fixed.
  net::URLRequestContextBuilder::HttpNetworkSessionParams
      custom_http_network_session_params;
  custom_http_network_session_params.parse_alternative_services = false;
  context_builder.set_http_network_session_params(
      custom_http_network_session_params);

  net_log_.reset(new net::NetLog);
  scoped_ptr<net::NetworkDelegate> network_delegate(new BasicNetworkDelegate());
#if defined(DATA_REDUCTION_PROXY_SUPPORT)
  DCHECK(!data_reduction_proxy_);
  // For now, the choice to enable the data reduction proxy happens once,
  // at initialization. It cannot be disabled thereafter.
  if (!config->data_reduction_proxy_key.empty()) {
    data_reduction_proxy_.reset(new CronetDataReductionProxy(
        config->data_reduction_proxy_key, config->data_reduction_primary_proxy,
        config->data_reduction_fallback_proxy,
        config->data_reduction_secure_proxy_check_url, config->user_agent,
        GetNetworkTaskRunner(), net_log_.get()));
    network_delegate = data_reduction_proxy_->CreateNetworkDelegate(
        std::move(network_delegate));
    std::vector<scoped_ptr<net::URLRequestInterceptor>> interceptors;
    interceptors.push_back(data_reduction_proxy_->CreateInterceptor());
    context_builder.SetInterceptors(std::move(interceptors));
  }
#endif  // defined(DATA_REDUCTION_PROXY_SUPPORT)
  context_builder.set_network_delegate(std::move(network_delegate));
  context_builder.set_net_log(net_log_.get());

  // Android provides a local HTTP proxy server that handles proxying when a PAC
  // URL is present. Create a proxy service without a resolver and rely on this
  // local HTTP proxy. See: crbug.com/432539.
  context_builder.set_proxy_service(
      net::ProxyService::CreateWithoutProxyResolver(
          std::move(proxy_config_service_), net_log_.get()));
  config->ConfigureURLRequestContextBuilder(&context_builder, net_log_.get());

  // Set up pref file if storage path is specified.
  if (!config->storage_path.empty()) {
    base::FilePath filepath(config->storage_path);
    filepath = filepath.Append(FILE_PATH_LITERAL("local_prefs.json"));
    json_pref_store_ = new JsonPrefStore(
        filepath, GetFileThread()->task_runner(), scoped_ptr<PrefFilter>());
    context_builder.SetFileTaskRunner(GetFileThread()->task_runner());

    // Set up HttpServerPropertiesManager.
    PrefServiceFactory factory;
    factory.set_user_prefs(json_pref_store_);
    scoped_refptr<PrefRegistrySimple> registry(new PrefRegistrySimple());
    registry->RegisterDictionaryPref(kHttpServerProperties,
                                     new base::DictionaryValue());
    pref_service_ = factory.Create(registry.get());

    scoped_ptr<net::HttpServerPropertiesManager> http_server_properties_manager(
        new net::HttpServerPropertiesManager(
            new PrefServiceAdapter(pref_service_.get()),
            GetNetworkTaskRunner()));
    http_server_properties_manager->InitializeOnNetworkThread();
    http_server_properties_manager_ = http_server_properties_manager.get();
    context_builder.SetHttpServerProperties(
        std::move(http_server_properties_manager));
  }

  // Explicitly disable the persister for Cronet to avoid persistence of dynamic
  // HPKP. This is a safety measure ensuring that nobody enables the persistence
  // of HPKP by specifying transport_security_persister_path in the future.
  context_builder.set_transport_security_persister_path(base::FilePath());

  context_ = context_builder.Build();

  default_load_flags_ = net::LOAD_DO_NOT_SAVE_COOKIES |
                        net::LOAD_DO_NOT_SEND_COOKIES;
  if (config->load_disable_cache)
    default_load_flags_ |= net::LOAD_DISABLE_CACHE;

  if (config->enable_sdch) {
    DCHECK(context_->sdch_manager());
    sdch_owner_.reset(
        new net::SdchOwner(context_->sdch_manager(), context_.get()));
    if (json_pref_store_) {
      sdch_owner_->EnablePersistentStorage(
          make_scoped_ptr(new SdchOwnerPrefStorage(json_pref_store_.get())));
    }
  }

  // Currently (circa M39) enabling QUIC requires setting probability threshold.
  if (config->enable_quic) {
    context_->http_server_properties()
        ->SetAlternativeServiceProbabilityThreshold(0.0f);
    for (auto hint = config->quic_hints.begin();
         hint != config->quic_hints.end(); ++hint) {
      const URLRequestContextConfig::QuicHint& quic_hint = **hint;
      if (quic_hint.host.empty()) {
        LOG(ERROR) << "Empty QUIC hint host: " << quic_hint.host;
        continue;
      }

      url::CanonHostInfo host_info;
      std::string canon_host(net::CanonicalizeHost(quic_hint.host, &host_info));
      if (!host_info.IsIPAddress() &&
          !net::IsCanonicalizedHostCompliant(canon_host)) {
        LOG(ERROR) << "Invalid QUIC hint host: " << quic_hint.host;
        continue;
      }

      if (quic_hint.port <= std::numeric_limits<uint16_t>::min() ||
          quic_hint.port > std::numeric_limits<uint16_t>::max()) {
        LOG(ERROR) << "Invalid QUIC hint port: "
                   << quic_hint.port;
        continue;
      }

      if (quic_hint.alternate_port <= std::numeric_limits<uint16_t>::min() ||
          quic_hint.alternate_port > std::numeric_limits<uint16_t>::max()) {
        LOG(ERROR) << "Invalid QUIC hint alternate port: "
                   << quic_hint.alternate_port;
        continue;
      }

      net::HostPortPair quic_hint_host_port_pair(canon_host,
                                                 quic_hint.port);
      net::AlternativeService alternative_service(
          net::AlternateProtocol::QUIC, "",
          static_cast<uint16_t>(quic_hint.alternate_port));
      context_->http_server_properties()->SetAlternativeService(
          quic_hint_host_port_pair, alternative_service, 1.0f,
          base::Time::Max());
    }
  }

  // Iterate through PKP configuration for every host.
  for (const auto& pkp : config->pkp_list) {
    // Add the host pinning.
    context_->transport_security_state()->AddHPKP(
        pkp->host, pkp->expiration_date, pkp->include_subdomains,
        pkp->pin_hashes, GURL::EmptyGURL());
  }

  JNIEnv* env = base::android::AttachCurrentThread();
  jcronet_url_request_context_.Reset(env, jcronet_url_request_context.obj());
  Java_CronetUrlRequestContext_initNetworkThread(
      env, jcronet_url_request_context.obj());

#if defined(DATA_REDUCTION_PROXY_SUPPORT)
  if (data_reduction_proxy_)
    data_reduction_proxy_->Init(true, GetURLRequestContext());
#endif
  is_context_initialized_ = true;
  while (!tasks_waiting_for_context_.empty()) {
    tasks_waiting_for_context_.front().Run();
    tasks_waiting_for_context_.pop();
  }
}

void CronetURLRequestContextAdapter::Destroy(
    JNIEnv* env,
    const JavaParamRef<jobject>& jcaller) {
  DCHECK(!GetNetworkTaskRunner()->BelongsToCurrentThread());
  // Stick network_thread_ in a local, as |this| may be destroyed from the
  // network thread before delete network_thread is called.
  base::Thread* network_thread = network_thread_;
  GetNetworkTaskRunner()->DeleteSoon(FROM_HERE, this);
  // Deleting thread stops it after all tasks are completed.
  delete network_thread;
}

net::URLRequestContext* CronetURLRequestContextAdapter::GetURLRequestContext() {
  if (!context_) {
    LOG(ERROR) << "URLRequestContext is not set up";
  }
  return context_.get();
}

void CronetURLRequestContextAdapter::PostTaskToNetworkThread(
    const tracked_objects::Location& posted_from,
    const base::Closure& callback) {
  GetNetworkTaskRunner()->PostTask(
      posted_from, base::Bind(&CronetURLRequestContextAdapter::
                                  RunTaskAfterContextInitOnNetworkThread,
                              base::Unretained(this), callback));
}

void CronetURLRequestContextAdapter::RunTaskAfterContextInitOnNetworkThread(
    const base::Closure& task_to_run_after_context_init) {
  DCHECK(GetNetworkTaskRunner()->BelongsToCurrentThread());
  if (is_context_initialized_) {
    DCHECK(tasks_waiting_for_context_.empty());
    task_to_run_after_context_init.Run();
    return;
  }
  tasks_waiting_for_context_.push(task_to_run_after_context_init);
}

bool CronetURLRequestContextAdapter::IsOnNetworkThread() const {
  return GetNetworkTaskRunner()->BelongsToCurrentThread();
}

scoped_refptr<base::SingleThreadTaskRunner>
CronetURLRequestContextAdapter::GetNetworkTaskRunner() const {
  return network_thread_->task_runner();
}

void CronetURLRequestContextAdapter::StartNetLogToFile(
    JNIEnv* env,
    const JavaParamRef<jobject>& jcaller,
    const JavaParamRef<jstring>& jfile_name,
    jboolean jlog_all) {
  PostTaskToNetworkThread(
      FROM_HERE,
      base::Bind(
          &CronetURLRequestContextAdapter::StartNetLogToFileOnNetworkThread,
          base::Unretained(this),
          base::android::ConvertJavaStringToUTF8(env, jfile_name), jlog_all));
}

void CronetURLRequestContextAdapter::StopNetLog(
    JNIEnv* env,
    const JavaParamRef<jobject>& jcaller) {
  PostTaskToNetworkThread(
      FROM_HERE,
      base::Bind(&CronetURLRequestContextAdapter::StopNetLogOnNetworkThread,
                 base::Unretained(this)));
}

void CronetURLRequestContextAdapter::StartNetLogToFileOnNetworkThread(
    const std::string& file_name, bool log_all) {
  DCHECK(GetNetworkTaskRunner()->BelongsToCurrentThread());
  DCHECK(is_context_initialized_);
  DCHECK(context_);
  // Do nothing if already logging to a file.
  if (write_to_file_observer_)
    return;
  base::FilePath file_path(file_name);
  base::ScopedFILE file(base::OpenFile(file_path, "w"));
  if (!file)
    return;

  write_to_file_observer_.reset(new net::WriteToFileNetLogObserver());
  if (log_all) {
    write_to_file_observer_->set_capture_mode(
        net::NetLogCaptureMode::IncludeSocketBytes());
  }
  write_to_file_observer_->StartObserving(context_->net_log(), std::move(file),
                                          nullptr, context_.get());
}

void CronetURLRequestContextAdapter::StopNetLogOnNetworkThread() {
  DCHECK(GetNetworkTaskRunner()->BelongsToCurrentThread());
  if (write_to_file_observer_) {
    write_to_file_observer_->StopObserving(context_.get());
    write_to_file_observer_.reset();
  }
}

base::Thread* CronetURLRequestContextAdapter::GetFileThread() {
  DCHECK(GetNetworkTaskRunner()->BelongsToCurrentThread());
  if (!file_thread_) {
    file_thread_.reset(new base::Thread("Network File Thread"));
    file_thread_->Start();
  }
  return file_thread_.get();
}

void CronetURLRequestContextAdapter::OnRTTObservation(
    int32_t rtt_ms,
    const base::TimeTicks& timestamp,
    net::NetworkQualityEstimator::ObservationSource source) {
  Java_CronetUrlRequestContext_onRttObservation(
      base::android::AttachCurrentThread(), jcronet_url_request_context_.obj(),
      rtt_ms, (timestamp - base::TimeTicks::UnixEpoch()).InMilliseconds(),
      source);
}

void CronetURLRequestContextAdapter::OnThroughputObservation(
    int32_t throughput_kbps,
    const base::TimeTicks& timestamp,
    net::NetworkQualityEstimator::ObservationSource source) {
  Java_CronetUrlRequestContext_onThroughputObservation(
      base::android::AttachCurrentThread(), jcronet_url_request_context_.obj(),
      throughput_kbps,
      (timestamp - base::TimeTicks::UnixEpoch()).InMilliseconds(), source);
}

// Create a URLRequestContextConfig from the given parameters.
static jlong CreateRequestContextConfig(
    JNIEnv* env,
    const JavaParamRef<jclass>& jcaller,
    const JavaParamRef<jstring>& juser_agent,
    const JavaParamRef<jstring>& jstorage_path,
    jboolean jquic_enabled,
    const JavaParamRef<jstring>& jquic_default_user_agent_id,
    jboolean jhttp2_enabled,
    jboolean jsdch_enabled,
    const JavaParamRef<jstring>& jdata_reduction_proxy_key,
    const JavaParamRef<jstring>& jdata_reduction_proxy_primary_proxy,
    const JavaParamRef<jstring>& jdata_reduction_proxy_fallback_proxy,
    const JavaParamRef<jstring>& jdata_reduction_proxy_secure_proxy_check_url,
    jboolean jdisable_cache,
    jint jhttp_cache_mode,
    jlong jhttp_cache_max_size,
    const JavaParamRef<jstring>& jexperimental_quic_connection_options,
    jlong jmock_cert_verifier) {
  return reinterpret_cast<jlong>(new URLRequestContextConfig(
      jquic_enabled,
      base::android::ConvertJavaStringToUTF8(env, jquic_default_user_agent_id),
      jhttp2_enabled, jsdch_enabled,
      static_cast<URLRequestContextConfig::HttpCacheType>(jhttp_cache_mode),
      jhttp_cache_max_size, jdisable_cache,
      base::android::ConvertJavaStringToUTF8(env, jstorage_path),
      base::android::ConvertJavaStringToUTF8(env, juser_agent),
      base::android::ConvertJavaStringToUTF8(
          env, jexperimental_quic_connection_options),
      base::android::ConvertJavaStringToUTF8(env, jdata_reduction_proxy_key),
      base::android::ConvertJavaStringToUTF8(
          env, jdata_reduction_proxy_primary_proxy),
      base::android::ConvertJavaStringToUTF8(
          env, jdata_reduction_proxy_fallback_proxy),
      base::android::ConvertJavaStringToUTF8(
          env, jdata_reduction_proxy_secure_proxy_check_url),
      make_scoped_ptr(
          reinterpret_cast<net::CertVerifier*>(jmock_cert_verifier))));
}

// Add a QUIC hint to a URLRequestContextConfig.
static void AddQuicHint(JNIEnv* env,
                        const JavaParamRef<jclass>& jcaller,
                        jlong jurl_request_context_config,
                        const JavaParamRef<jstring>& jhost,
                        jint jport,
                        jint jalternate_port) {
  URLRequestContextConfig* config =
      reinterpret_cast<URLRequestContextConfig*>(jurl_request_context_config);
  config->quic_hints.push_back(
      make_scoped_ptr(new URLRequestContextConfig::QuicHint(
          base::android::ConvertJavaStringToUTF8(env, jhost), jport,
          jalternate_port)));
}

// Add a public key pin to URLRequestContextConfig.
// |jhost| is the host to apply the pin to.
// |jhashes| is an array of jbyte[32] representing SHA256 key hashes.
// |jinclude_subdomains| indicates if pin should be applied to subdomains.
// |jexpiration_time| is the time that the pin expires, in milliseconds since
// Jan. 1, 1970, midnight GMT.
static void AddPkp(JNIEnv* env,
                   const JavaParamRef<jclass>& jcaller,
                   jlong jurl_request_context_config,
                   const JavaParamRef<jstring>& jhost,
                   const JavaParamRef<jobjectArray>& jhashes,
                   jboolean jinclude_subdomains,
                   jlong jexpiration_time) {
  URLRequestContextConfig* config =
      reinterpret_cast<URLRequestContextConfig*>(jurl_request_context_config);
  scoped_ptr<URLRequestContextConfig::Pkp> pkp(new URLRequestContextConfig::Pkp(
      base::android::ConvertJavaStringToUTF8(env, jhost), jinclude_subdomains,
      base::Time::UnixEpoch() +
          base::TimeDelta::FromMilliseconds(jexpiration_time)));
  size_t hash_count = env->GetArrayLength(jhashes);
  for (size_t i = 0; i < hash_count; ++i) {
    ScopedJavaLocalRef<jbyteArray> bytes_array(
        env, static_cast<jbyteArray>(env->GetObjectArrayElement(jhashes, i)));
    static_assert(std::is_pod<net::SHA256HashValue>::value,
                  "net::SHA256HashValue is not POD");
    static_assert(sizeof(net::SHA256HashValue) * CHAR_BIT == 256,
                  "net::SHA256HashValue contains overhead");
    if (env->GetArrayLength(bytes_array.obj()) !=
        sizeof(net::SHA256HashValue)) {
      LOG(ERROR) << "Unable to add public key hash value.";
      continue;
    }
    jbyte* bytes = env->GetByteArrayElements(bytes_array.obj(), nullptr);
    net::HashValue hash(*reinterpret_cast<net::SHA256HashValue*>(bytes));
    pkp->pin_hashes.push_back(hash);
    env->ReleaseByteArrayElements(bytes_array.obj(), bytes, JNI_ABORT);
  }
  config->pkp_list.push_back(std::move(pkp));
}

// Creates RequestContextAdater if config is valid URLRequestContextConfig,
// returns 0 otherwise.
static jlong CreateRequestContextAdapter(JNIEnv* env,
                                         const JavaParamRef<jclass>& jcaller,
                                         jlong jconfig) {
  scoped_ptr<URLRequestContextConfig> context_config(
      reinterpret_cast<URLRequestContextConfig*>(jconfig));

  CronetURLRequestContextAdapter* context_adapter =
      new CronetURLRequestContextAdapter(std::move(context_config));
  return reinterpret_cast<jlong>(context_adapter);
}

static jint SetMinLogLevel(JNIEnv* env,
                           const JavaParamRef<jclass>& jcaller,
                           jint jlog_level) {
  jint old_log_level = static_cast<jint>(logging::GetMinLogLevel());
  // MinLogLevel is global, shared by all URLRequestContexts.
  logging::SetMinLogLevel(static_cast<int>(jlog_level));
  return old_log_level;
}

static ScopedJavaLocalRef<jbyteArray> GetHistogramDeltas(
    JNIEnv* env,
    const JavaParamRef<jclass>& jcaller) {
  base::StatisticsRecorder::Initialize();
  std::vector<uint8_t> data;
  if (!HistogramManager::GetInstance()->GetDeltas(&data))
    return ScopedJavaLocalRef<jbyteArray>();
  return base::android::ToJavaByteArray(env, &data[0], data.size());
}

}  // namespace cronet
