/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef InspectorResourceAgent_h
#define InspectorResourceAgent_h

#include "bindings/core/v8/ScriptString.h"
#include "core/CoreExport.h"
#include "core/inspector/InspectorBaseAgent.h"
#include "core/inspector/InspectorPageAgent.h"
#include "platform/Timer.h"
#include "platform/heap/Handle.h"
#include "platform/network/ResourceRequest.h"
#include "wtf/PassOwnPtr.h"
#include "wtf/text/WTFString.h"

namespace blink {

class Resource;
struct FetchInitiatorInfo;
class Document;
class DocumentLoader;
class EncodedFormData;
class ExecutionContext;
class LocalFrame;
class HTTPHeaderMap;
class InspectedFrames;
class InspectorFrontend;
class KURL;
class NetworkResourcesData;
class ResourceError;
class ResourceLoader;
class ResourceResponse;
class ThreadableLoaderClient;
class XHRReplayData;
class XMLHttpRequest;

class WebSocketHandshakeRequest;
class WebSocketHandshakeResponse;

namespace protocol {
class DictionaryValue;
}

typedef String ErrorString;

class CORE_EXPORT InspectorResourceAgent final : public InspectorBaseAgent<InspectorResourceAgent, protocol::Frontend::Network>, public protocol::Dispatcher::NetworkCommandHandler {
public:
    static PassOwnPtrWillBeRawPtr<InspectorResourceAgent> create(InspectedFrames* inspectedFrames)
    {
        return adoptPtrWillBeNoop(new InspectorResourceAgent(inspectedFrames));
    }

    void restore() override;

    ~InspectorResourceAgent() override;
    DECLARE_VIRTUAL_TRACE();

    // Called from instrumentation.
    void didBlockRequest(LocalFrame*, const ResourceRequest&, DocumentLoader*, const FetchInitiatorInfo&, ResourceRequestBlockedReason);
    void willSendRequest(LocalFrame*, unsigned long identifier, DocumentLoader*, ResourceRequest&, const ResourceResponse& redirectResponse, const FetchInitiatorInfo&);
    void markResourceAsCached(unsigned long identifier);
    void didReceiveResourceResponse(LocalFrame*, unsigned long identifier, DocumentLoader*, const ResourceResponse&, ResourceLoader*);
    void didReceiveData(LocalFrame*, unsigned long identifier, const char* data, int dataLength, int encodedDataLength);
    void didFinishLoading(unsigned long identifier, double monotonicFinishTime, int64_t encodedDataLength);
    void didReceiveCORSRedirectResponse(LocalFrame*, unsigned long identifier, DocumentLoader*, const ResourceResponse&, ResourceLoader*);
    void didFailLoading(unsigned long identifier, const ResourceError&);
    void didCommitLoad(LocalFrame*, DocumentLoader*);
    void scriptImported(unsigned long identifier, const String& sourceString);
    void didReceiveScriptResponse(unsigned long identifier);
    bool shouldForceCORSPreflight();

    void documentThreadableLoaderStartedLoadingForClient(unsigned long identifier, ThreadableLoaderClient*);
    void willLoadXHR(XMLHttpRequest*, ThreadableLoaderClient*, const AtomicString& method, const KURL&, bool async, PassRefPtr<EncodedFormData> body, const HTTPHeaderMap& headers, bool includeCrendentials);
    void didFailXHRLoading(ExecutionContext*, XMLHttpRequest*, ThreadableLoaderClient*, const AtomicString&, const String&);
    void didFinishXHRLoading(ExecutionContext*, XMLHttpRequest*, ThreadableLoaderClient*, const AtomicString&, const String&);

    void willStartFetch(ThreadableLoaderClient*);
    void didFailFetch(ThreadableLoaderClient*);
    void didFinishFetch(ExecutionContext*, ThreadableLoaderClient*, const AtomicString& method, const String& url);

    void willSendEventSourceRequest(ThreadableLoaderClient*);
    void willDispatchEventSourceEvent(ThreadableLoaderClient*, const AtomicString& eventName, const AtomicString& eventId, const String& data);
    void didFinishEventSourceRequest(ThreadableLoaderClient*);

    void removedResourceFromMemoryCache(Resource*);

    void applyUserAgentOverride(String* userAgent);

    // FIXME: InspectorResourceAgent should not be aware of style recalculation.
    void willRecalculateStyle(Document*);
    void didRecalculateStyle(int);
    void didScheduleStyleRecalculation(Document*);

    void frameScheduledNavigation(LocalFrame*, double);
    void frameClearedScheduledNavigation(LocalFrame*);

    PassOwnPtr<protocol::Network::Initiator> buildInitiatorObject(Document*, const FetchInitiatorInfo&);

    void didCreateWebSocket(Document*, unsigned long identifier, const KURL& requestURL, const String&);
    void willSendWebSocketHandshakeRequest(Document*, unsigned long identifier, const WebSocketHandshakeRequest*);
    void didReceiveWebSocketHandshakeResponse(Document*, unsigned long identifier, const WebSocketHandshakeRequest*, const WebSocketHandshakeResponse*);
    void didCloseWebSocket(Document*, unsigned long identifier);
    void didReceiveWebSocketFrame(unsigned long identifier, int opCode, bool masked, const char* payload, size_t payloadLength);
    void didSendWebSocketFrame(unsigned long identifier, int opCode, bool masked, const char* payload, size_t payloadLength);
    void didReceiveWebSocketFrameError(unsigned long identifier, const String&);

    // Called from frontend
    void enable(ErrorString*) override;
    void disable(ErrorString*) override;
    void setUserAgentOverride(ErrorString*, const String& userAgent) override;
    void setExtraHTTPHeaders(ErrorString*, PassOwnPtr<protocol::Network::Headers>) override;
    void getResponseBody(ErrorString*, const String& requestId, PassRefPtr<GetResponseBodyCallback>) override;
    void addBlockedURL(ErrorString*, const String& url) override;
    void removeBlockedURL(ErrorString*, const String& url) override;
    void replayXHR(ErrorString*, const String& requestId) override;
    void setMonitoringXHREnabled(ErrorString*, bool enabled) override;
    void canClearBrowserCache(ErrorString*, bool* result) override;
    void canClearBrowserCookies(ErrorString*, bool* result) override;
    void emulateNetworkConditions(ErrorString*, bool offline, double latency, double downloadThroughput, double uploadThroughput) override;
    void setCacheDisabled(ErrorString*, bool cacheDisabled) override;
    void setDataSizeLimitsForTest(ErrorString*, int maxTotalSize, int maxResourceSize) override;

    // Called from other agents.
    void setHostId(const String&);
    bool fetchResourceContent(Document*, const KURL&, String* content, bool* base64Encoded);
    bool shouldBlockRequest(const ResourceRequest&);

private:
    explicit InspectorResourceAgent(InspectedFrames*);

    void enable();
    void willSendRequestInternal(LocalFrame*, unsigned long identifier, DocumentLoader*, const ResourceRequest&, const ResourceResponse& redirectResponse, const FetchInitiatorInfo&);
    void delayedRemoveReplayXHR(XMLHttpRequest*);
    void removeFinishedReplayXHRFired(Timer<InspectorResourceAgent>*);
    void didFinishXHRInternal(ExecutionContext*, XMLHttpRequest*, ThreadableLoaderClient*, const AtomicString&, const String&, bool);

    bool getResponseBodyBlob(const String& requestId, PassRefPtr<GetResponseBodyCallback>);

    RawPtrWillBeMember<InspectedFrames> m_inspectedFrames;
    String m_userAgentOverride;
    String m_hostId;
    OwnPtrWillBeMember<NetworkResourcesData> m_resourcesData;

    typedef HashMap<ThreadableLoaderClient*, unsigned long> ThreadableLoaderClientRequestIdMap;

    // Stores the pending ThreadableLoaderClient till an identifier for
    // the load is generated by the loader and passed to the inspector
    // via the documentThreadableLoaderStartedLoadingForClient() method.
    ThreadableLoaderClient* m_pendingRequest;
    InspectorPageAgent::ResourceType m_pendingRequestType;
    ThreadableLoaderClientRequestIdMap m_knownRequestIdMap;

    RefPtrWillBeMember<XHRReplayData> m_pendingXHRReplayData;

    typedef HashMap<String, OwnPtr<protocol::Network::Initiator>> FrameNavigationInitiatorMap;
    FrameNavigationInitiatorMap m_frameNavigationInitiatorMap;

    // FIXME: InspectorResourceAgent should now be aware of style recalculation.
    OwnPtr<protocol::Network::Initiator> m_styleRecalculationInitiator;
    bool m_isRecalculatingStyle;

    PersistentHeapHashSetWillBeHeapHashSet<Member<XMLHttpRequest>> m_replayXHRs;
    PersistentHeapHashSetWillBeHeapHashSet<Member<XMLHttpRequest>> m_replayXHRsToBeDeleted;
    Timer<InspectorResourceAgent> m_removeFinishedReplayXHRTimer;
};

} // namespace blink


#endif // !defined(InspectorResourceAgent_h)
