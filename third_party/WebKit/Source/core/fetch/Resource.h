/*
    Copyright (C) 1998 Lars Knoll (knoll@mpi-hd.mpg.de)
    Copyright (C) 2001 Dirk Mueller <mueller@kde.org>
    Copyright (C) 2006 Samuel Weinig (sam.weinig@gmail.com)
    Copyright (C) 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011 Apple Inc. All rights reserved.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#ifndef Resource_h
#define Resource_h

#include "core/CoreExport.h"
#include "core/fetch/CachedMetadataHandler.h"
#include "core/fetch/ResourceLoaderOptions.h"
#include "platform/Timer.h"
#include "platform/network/ResourceError.h"
#include "platform/network/ResourceLoadPriority.h"
#include "platform/network/ResourceRequest.h"
#include "platform/network/ResourceResponse.h"
#include "platform/scheduler/CancellableTaskFactory.h"
#include "public/platform/WebDataConsumerHandle.h"
#include "public/platform/WebMemoryDumpProvider.h"
#include "wtf/Allocator.h"
#include "wtf/HashCountedSet.h"
#include "wtf/HashSet.h"
#include "wtf/OwnPtr.h"
#include "wtf/text/WTFString.h"

// FIXME(crbug.com/352043): This is temporarily enabled even on RELEASE to diagnose a wild crash.
#define ENABLE_RESOURCE_IS_DELETED_CHECK

namespace blink {

struct FetchInitiatorInfo;
class CachedMetadata;
class FetchRequest;
class ResourceClient;
class ResourceFetcher;
class ResourceTimingInfo;
class InspectorResource;
class ResourceLoader;
class SecurityOrigin;
class SharedBuffer;

// A resource that is held in the cache. Classes who want to use this object should derive
// from ResourceClient, to get the function calls in case the requested data has arrived.
// This class also does the actual communication with the loader to obtain the resource from the network.
class CORE_EXPORT Resource : public RefCountedWillBeGarbageCollectedFinalized<Resource> {
    WTF_MAKE_NONCOPYABLE(Resource);
    USING_FAST_MALLOC_WITH_TYPE_NAME_WILL_BE_REMOVED(blink::Resource);
    friend class InspectorResource;

public:
    enum Type {
        MainResource,
        Image,
        CSSStyleSheet,
        Script,
        Font,
        Raw,
        SVGDocument,
        XSLStyleSheet,
        LinkPrefetch,
        LinkPreload,
        TextTrack,
        ImportResource,
        Media, // Audio or video file requested by a HTML5 media element
        Manifest
    };

    enum Status {
        Unknown, // let cache decide what to do with it
        Pending, // only partially loaded
        Cached, // regular case
        LoadError,
        DecodeError
    };

    // Exposed for testing.
    static PassRefPtrWillBeRawPtr<Resource> create(const ResourceRequest& request, Type type)
    {
        return adoptRefWillBeNoop(new Resource(request, type));
    }
    virtual ~Resource();

    virtual void removedFromMemoryCache();
    DECLARE_VIRTUAL_TRACE();

    virtual void load(ResourceFetcher*, const ResourceLoaderOptions&);

    virtual void setEncoding(const String&) { }
    virtual String encoding() const { return String(); }
    virtual void appendData(const char*, size_t);
    virtual void error(Resource::Status);
    virtual void setCORSFailed() { }

    void setNeedsSynchronousCacheHit(bool needsSynchronousCacheHit) { m_needsSynchronousCacheHit = needsSynchronousCacheHit; }

    void setLinkPreload(bool isLinkPreload) { m_linkPreload = isLinkPreload; }
    bool isLinkPreload() const { return m_linkPreload; }

    void setResourceError(const ResourceError& error) { m_error = error; }
    const ResourceError& resourceError() const { return m_error; }

    void setIdentifier(unsigned long identifier) { m_identifier = identifier; }
    unsigned long identifier() const { return m_identifier; }

    virtual bool shouldIgnoreHTTPStatusCodeErrors() const { return false; }

    const ResourceRequest& resourceRequest() const { return m_resourceRequest; }
    const ResourceRequest& lastResourceRequest() const;

    void setRevalidatingRequest(const ResourceRequest& request) { m_revalidatingRequest = request; }

    const KURL& url() const { return m_resourceRequest.url();}
    Type getType() const { return static_cast<Type>(m_type); }
    const ResourceLoaderOptions& options() const { return m_options; }
    void setOptions(const ResourceLoaderOptions& options) { m_options = options; }

    void didChangePriority(ResourceLoadPriority, int intraPriorityValue);
    ResourcePriority priorityFromClients();

    void addClient(ResourceClient*);
    void removeClient(ResourceClient*);
    bool hasClients() const { return !m_clients.isEmpty() || !m_clientsAwaitingCallback.isEmpty() || !m_finishedClients.isEmpty(); }

    enum PreloadResult {
        PreloadNotReferenced,
        PreloadReferenced,
        PreloadReferencedWhileLoading,
        PreloadReferencedWhileComplete
    };
    PreloadResult getPreloadResult() const { return static_cast<PreloadResult>(m_preloadResult); }

    virtual void didAddClient(ResourceClient*);
    virtual void didRemoveClient(ResourceClient*) { }
    virtual void allClientsRemoved();

    unsigned count() const { return m_clients.size(); }

    Status getStatus() const { return static_cast<Status>(m_status); }
    void setStatus(Status status) { m_status = status; }

    size_t size() const { return encodedSize() + decodedSize() + overheadSize(); }
    size_t encodedSize() const { return m_encodedSize; }
    size_t decodedSize() const { return m_decodedSize; }
    size_t overheadSize() const;

    bool isLoaded() const { return !m_loading; } // FIXME. Method name is inaccurate. Loading might not have started yet.

    bool isLoading() const { return m_loading; }
    void setLoading(bool b) { m_loading = b; }
    virtual bool stillNeedsLoad() const { return false; }

    ResourceLoader* loader() const { return m_loader.get(); }

    virtual bool isImage() const { return false; }
    bool shouldBlockLoadEvent() const;
    bool isLoadEventBlockingResourceType() const;

    // Computes the status of an object after loading.
    // Updates the expire date on the cache entry file
    void setLoadFinishTime(double finishTime) { m_loadFinishTime = finishTime; }
    void finish();

    // FIXME: Remove the stringless variant once all the callsites' error messages are updated.
    bool passesAccessControlCheck(SecurityOrigin*) const;
    bool passesAccessControlCheck(SecurityOrigin*, String& errorDescription) const;

    bool isEligibleForIntegrityCheck(SecurityOrigin*) const;

    void clearLoader();

    SharedBuffer* resourceBuffer() const { return m_data.get(); }
    void setResourceBuffer(PassRefPtr<SharedBuffer>);

    virtual void willFollowRedirect(ResourceRequest&, const ResourceResponse&);

    virtual void updateRequest(const ResourceRequest&) { }
    virtual void responseReceived(const ResourceResponse&, PassOwnPtr<WebDataConsumerHandle>);
    void setResponse(const ResourceResponse& response) { m_response = response; }
    const ResourceResponse& response() const { return m_response; }

    virtual void reportResourceTimingToClients(const ResourceTimingInfo&) { }

    // Sets the serialized metadata retrieved from the platform's cache.
    virtual void setSerializedCachedMetadata(const char*, size_t);

    // This may return nullptr when the resource isn't cacheable.
    CachedMetadataHandler* cacheHandler();

    String reasonNotDeletable() const;

    // List of acceptable MIME types separated by ",".
    // A MIME type may contain a wildcard, e.g. "text/*".
    AtomicString accept() const { return m_accept; }
    void setAccept(const AtomicString& accept) { m_accept = accept; }

    AtomicString httpContentType() const;

    bool wasCanceled() const { return m_error.isCancellation(); }
    bool errorOccurred() const { return m_status == LoadError || m_status == DecodeError; }
    bool loadFailedOrCanceled() { return !m_error.isNull(); }

    DataBufferingPolicy getDataBufferingPolicy() const { return m_options.dataBufferingPolicy; }
    void setDataBufferingPolicy(DataBufferingPolicy);

    bool isUnusedPreload() const { return isPreloaded() && getPreloadResult() == PreloadNotReferenced; }
    bool isPreloaded() const { return m_preloadCount; }
    void increasePreloadCount() { ++m_preloadCount; }
    void decreasePreloadCount() { ASSERT(m_preloadCount); --m_preloadCount; }

    bool canReuseRedirectChain();
    bool mustRevalidateDueToCacheHeaders();
    bool canUseCacheValidator();
    bool isCacheValidator() const { return !m_revalidatingRequest.isNull(); }
    bool hasCacheControlNoStoreHeader();
    bool hasVaryHeader() const;
    virtual bool mustRefetchDueToIntegrityMetadata(const FetchRequest& request) const { return false; }

    double currentAge() const;
    double freshnessLifetime();
    double stalenessLifetime();

    bool isPurgeable() const;
    bool lock();

    void setCacheIdentifier(const String& cacheIdentifier) { m_cacheIdentifier = cacheIdentifier; }
    String cacheIdentifier() const { return m_cacheIdentifier; }

    virtual void didSendData(unsigned long long /* bytesSent */, unsigned long long /* totalBytesToBeSent */) { }
    virtual void didDownloadData(int) { }

    double loadFinishTime() const { return m_loadFinishTime; }

    virtual bool canReuse(const ResourceRequest&) const { return true; }

    // Used by the MemoryCache to reduce the memory consumption of the entry.
    void prune();

    virtual void onMemoryDump(WebMemoryDumpLevelOfDetail, WebProcessMemoryDump*) const;

    static const char* resourceTypeToString(Type, const FetchInitiatorInfo&);
    static const char* resourceTypeName(Type);

    // TODO(japhet): Remove once oilpan ships, it doesn't need the WeakPtr.
    WeakPtrWillBeRawPtr<Resource> asWeakPtr();

#ifdef ENABLE_RESOURCE_IS_DELETED_CHECK
    void assertAlive() const { RELEASE_ASSERT(!m_deleted); }
#else
    void assertAlive() const { }
#endif

protected:
    Resource(const ResourceRequest&, Type);

    virtual void checkNotify();
    virtual void finishOnePart();

    virtual void destroyDecodedDataForFailedRevalidation() { }

    void setEncodedSize(size_t);
    void setDecodedSize(size_t);
    void didAccessDecodedData();

    void finishPendingClients();

    HashCountedSet<ResourceClient*> m_clients;
    HashCountedSet<ResourceClient*> m_clientsAwaitingCallback;
    HashCountedSet<ResourceClient*> m_finishedClients;

    class ResourceCallback : public NoBaseWillBeGarbageCollectedFinalized<ResourceCallback> {
    public:
        static ResourceCallback* callbackHandler();
        DECLARE_TRACE();
        void schedule(Resource*);
        void cancel(Resource*);
        bool isScheduled(Resource*) const;
    private:
        ResourceCallback();
        void runTask();
        OwnPtr<CancellableTaskFactory> m_callbackTaskFactory;
        WillBeHeapHashSet<RefPtrWillBeMember<Resource>> m_resourcesWithPendingClients;
    };

    bool hasClient(ResourceClient* client) { return m_clients.contains(client) || m_clientsAwaitingCallback.contains(client) || m_finishedClients.contains(client); }

    struct RedirectPair {
        DISALLOW_NEW_EXCEPT_PLACEMENT_NEW();
    public:
        explicit RedirectPair(const ResourceRequest& request, const ResourceResponse& redirectResponse)
            : m_request(request)
            , m_redirectResponse(redirectResponse)
        {
        }

        ResourceRequest m_request;
        ResourceResponse m_redirectResponse;
    };
    const Vector<RedirectPair>& redirectChain() const { return m_redirectChain; }

    virtual bool isSafeToUnlock() const { return false; }
    virtual void destroyDecodedDataIfPossible() { }

    void markClientsFinished();

    // Returns the memory dump name used for tracing. See Resource::onMemoryDump.
    String getMemoryDumpName() const;

    ResourceRequest m_resourceRequest;
    ResourceRequest m_revalidatingRequest;
    AtomicString m_accept;
    PersistentWillBeMember<ResourceLoader> m_loader;
    ResourceLoaderOptions m_options;

    ResourceResponse m_response;
    double m_responseTimestamp;

    RefPtr<SharedBuffer> m_data;
    Timer<Resource> m_cancelTimer;

private:
    class CacheHandler;
    void cancelTimerFired(Timer<Resource>*);

    void revalidationSucceeded(const ResourceResponse&);
    void revalidationFailed();

    bool unlock();

    void setCachedMetadata(unsigned dataTypeID, const char*, size_t, CachedMetadataHandler::CacheType);
    void clearCachedMetadata(CachedMetadataHandler::CacheType);
    CachedMetadata* cachedMetadata(unsigned dataTypeID) const;

    String m_fragmentIdentifierForRequest;

#if !ENABLE(OILPAN)
    WeakPtrFactory<Resource> m_weakPtrFactory;
#endif

    RefPtr<CachedMetadata> m_cachedMetadata;
    OwnPtrWillBeMember<CacheHandler> m_cacheHandler;

    ResourceError m_error;

    double m_loadFinishTime;

    unsigned long m_identifier;

    size_t m_encodedSize;
    size_t m_decodedSize;
    unsigned m_preloadCount;

    String m_cacheIdentifier;

    unsigned m_preloadResult : 2; // PreloadResult
    unsigned m_requestedFromNetworkingLayer : 1;

    unsigned m_loading : 1;

    unsigned m_type : 4; // Type
    unsigned m_status : 3; // Status

    unsigned m_needsSynchronousCacheHit : 1;
    unsigned m_linkPreload : 1;

#ifdef ENABLE_RESOURCE_IS_DELETED_CHECK
    bool m_deleted;
#endif

    // Ordered list of all redirects followed while fetching this resource.
    Vector<RedirectPair> m_redirectChain;
};

class ResourceFactory {
    STACK_ALLOCATED();
public:
    virtual PassRefPtrWillBeRawPtr<Resource> create(const ResourceRequest&, const String&) const = 0;
    Resource::Type type() const { return m_type; }

protected:
    ResourceFactory(Resource::Type type) : m_type(type) { }

    Resource::Type m_type;
};

#define DEFINE_RESOURCE_TYPE_CASTS(typeName) \
    DEFINE_TYPE_CASTS(typeName##Resource, Resource, resource, resource->getType() == Resource::typeName, resource.getType() == Resource::typeName); \
    inline typeName##Resource* to##typeName##Resource(const RefPtrWillBeRawPtr<Resource>& ptr) { return to##typeName##Resource(ptr.get()); }

} // namespace blink

#endif
