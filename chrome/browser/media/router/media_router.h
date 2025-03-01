// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MEDIA_ROUTER_MEDIA_ROUTER_H_
#define CHROME_BROWSER_MEDIA_ROUTER_MEDIA_ROUTER_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/callback.h"
#include "base/callback_list.h"
#include "base/memory/scoped_vector.h"
#include "base/time/time.h"
#include "chrome/browser/media/router/issue.h"
#include "chrome/browser/media/router/media_route.h"
#include "chrome/browser/media/router/media_sink.h"
#include "chrome/browser/media/router/media_source.h"
#include "components/keyed_service/core/keyed_service.h"
#include "content/public/browser/presentation_service_delegate.h"
#include "content/public/browser/presentation_session_message.h"

namespace content {
class WebContents;
}

namespace media_router {

class IssuesObserver;
class MediaRoutesObserver;
class MediaSinksObserver;
class PresentationConnectionStateObserver;
class PresentationSessionMessagesObserver;
class RouteRequestResult;

// Type of callback used in |CreateRoute()|, |JoinRoute()|, and
// |ConnectRouteByRouteId()|. Callback is invoked when the route request either
// succeeded or failed.
using MediaRouteResponseCallback =
    base::Callback<void(const RouteRequestResult& result)>;

// Subscription object returned by calling
// |AddPresentationConnectionStateChangedCallback|. See the method comments for
// details.
using PresentationConnectionStateSubscription = base::CallbackList<void(
    const content::PresentationConnectionStateChangeInfo&)>::Subscription;

// An interface for handling resources related to media routing.
// Responsible for registering observers for receiving sink availability
// updates, handling route requests/responses, and operating on routes (e.g.
// posting messages or closing).
// TODO(imcheng): Reduce number of parameters by putting them into structs.
class MediaRouter : public KeyedService {
 public:
  using PresentationSessionMessageCallback = base::Callback<void(
      scoped_ptr<ScopedVector<content::PresentationSessionMessage>>)>;
  using SendRouteMessageCallback = base::Callback<void(bool sent)>;

  ~MediaRouter() override = default;

  // Creates a media route from |source_id| to |sink_id|.
  // |origin| is the URL of requestor's page.
  // |web_contents| is the WebContents of the tab in which the request was made.
  // |origin| and |web_contents| are used for enforcing same-origin and/or
  // same-tab scope for JoinRoute() requests. (e.g., if enforced, the page
  // requesting JoinRoute() must have the same origin as the page that requested
  // CreateRoute()).
  // The caller may pass in nullptr for |web_contents| if tab is not applicable.
  // Each callback in |callbacks| is invoked with a response indicating
  // success or failure, in the order they are listed.
  // If |timeout| is positive, then any un-invoked |callbacks| will be invoked
  // with a timeout error after the timeout expires.
  // If |off_the_record| is true, the request was made by an off the record
  // (incognito) profile.
  virtual void CreateRoute(
      const MediaSource::Id& source_id,
      const MediaSink::Id& sink_id,
      const GURL& origin,
      content::WebContents* web_contents,
      const std::vector<MediaRouteResponseCallback>& callbacks,
      base::TimeDelta timeout,
      bool off_the_record) = 0;

  // Creates a route and connects it to an existing route identified by
  // |route_id|. |route_id| must refer to a non-local route, unnassociated with
  // a Presentation ID, because a new Presentation ID will be created.
  // |source|: The source to route to the existing route.
  // |route_id|: Route ID of the existing route.
  // |origin|, |web_contents|: Origin and WebContents of the join route request.
  // Used for validation when enforcing same-origin and/or same-tab scope.
  // (See CreateRoute documentation).
  // Each callback in |callbacks| is invoked with a response indicating
  // success or failure, in the order they are listed.
  // If |timeout| is positive, then any un-invoked |callbacks| will be invoked
  // with a timeout error after the timeout expires.
  virtual void ConnectRouteByRouteId(
      const MediaSource::Id& source_id,
      const MediaRoute::Id& route_id,
      const GURL& origin,
      content::WebContents* web_contents,
      const std::vector<MediaRouteResponseCallback>& callbacks,
      base::TimeDelta timeout) = 0;

  // Joins an existing route identified by |presentation_id|.
  // |source|: The source to route to the existing route.
  // |presentation_id|: Presentation ID of the existing route.
  // |origin|, |web_contents|: Origin and WebContents of the join route request.
  // Used for validation when enforcing same-origin and/or same-tab scope.
  // (See CreateRoute documentation).
  // Each callback in |callbacks| is invoked with a response indicating
  // success or failure, in the order they are listed.
  // If |timeout| is positive, then any un-invoked |callbacks| will be invoked
  // with a timeout error after the timeout expires.
  // If |off_the_record| is true, the request was made by an off the record
  // (incognito) profile.
  virtual void JoinRoute(
      const MediaSource::Id& source,
      const std::string& presentation_id,
      const GURL& origin,
      content::WebContents* web_contents,
      const std::vector<MediaRouteResponseCallback>& callbacks,
      base::TimeDelta timeout,
      bool off_the_record) = 0;

  // Terminates the media route specified by |route_id|.
  virtual void TerminateRoute(const MediaRoute::Id& route_id) = 0;

  // Detaches the media route specified by |route_id|. The request might come
  // from the page or from an event like navigation or garbage collection.
  virtual void DetachRoute(const MediaRoute::Id& route_id) = 0;

  // Posts |message| to a MediaSink connected via MediaRoute with |route_id|.
  virtual void SendRouteMessage(const MediaRoute::Id& route_id,
                                const std::string& message,
                                const SendRouteMessageCallback& callback) = 0;

  // Sends |data| to a MediaSink connected via MediaRoute with |route_id|.
  // This is called for Blob / ArrayBuffer / ArrayBufferView types.
  virtual void SendRouteBinaryMessage(
      const MediaRoute::Id& route_id,
      scoped_ptr<std::vector<uint8_t>> data,
      const SendRouteMessageCallback& callback) = 0;

  // Adds a new |issue|.
  virtual void AddIssue(const Issue& issue) = 0;

  // Clears the issue with the id |issue_id|.
  virtual void ClearIssue(const Issue::Id& issue_id) = 0;

  // Notifies the Media Router that the user has taken an action involving the
  // Media Router. This can be used to perform any initialization that is not
  // approriate to be done at construction.
  virtual void OnUserGesture() = 0;

  // Adds |callback| to listen for state changes for presentation connected to
  // |route_id|. The returned Subscription object is owned by the caller.
  // |callback| will be invoked whenever there are state changes, until the
  // caller destroys the Subscription object.
  virtual scoped_ptr<PresentationConnectionStateSubscription>
  AddPresentationConnectionStateChangedCallback(
      const MediaRoute::Id& route_id,
      const content::PresentationConnectionStateChangedCallback& callback) = 0;

 private:
  friend class IssuesObserver;
  friend class MediaSinksObserver;
  friend class MediaRoutesObserver;
  friend class PresentationConnectionStateObserver;
  friend class PresentationSessionMessagesObserver;

  // The following functions are called by friend Observer classes above.

  // Registers |observer| with this MediaRouter. |observer| specifies a media
  // source and will receive updates with media sinks that are compatible with
  // that source. The initial update may happen synchronously.
  // NOTE: This class does not assume ownership of |observer|. Callers must
  // manage |observer| and make sure |UnregisterObserver()| is called
  // before the observer is destroyed.
  // It is invalid to register the same observer more than once and will result
  // in undefined behavior.
  // If the MRPM Host is not available, the registration request will fail
  // immediately.
  // The implementation can reject the request to observe in which case it will
  // notify the caller by returning |false|.
  virtual bool RegisterMediaSinksObserver(MediaSinksObserver* observer) = 0;

  // Removes a previously added MediaSinksObserver. |observer| will stop
  // receiving further updates.
  virtual void UnregisterMediaSinksObserver(MediaSinksObserver* observer) = 0;

  // Adds a MediaRoutesObserver to listen for updates on MediaRoutes.
  // The initial update may happen synchronously.
  // MediaRouter does not own |observer|. |UnregisterMediaRoutesObserver| should
  // be called before |observer| is destroyed.
  // It is invalid to register the same observer more than once and will result
  // in undefined behavior.
  virtual void RegisterMediaRoutesObserver(MediaRoutesObserver* observer) = 0;

  // Removes a previously added MediaRoutesObserver. |observer| will stop
  // receiving further updates.
  virtual void UnregisterMediaRoutesObserver(MediaRoutesObserver* observer) = 0;

  // Adds the IssuesObserver |observer|.
  // It is invalid to register the same observer more than once and will result
  // in undefined behavior.
  virtual void RegisterIssuesObserver(IssuesObserver* observer) = 0;

  // Removes the IssuesObserver |observer|.
  virtual void UnregisterIssuesObserver(IssuesObserver* observer) = 0;

  // Registers |observer| with this MediaRouter. |observer| specifies a media
  // route corresponding to a presentation and will receive messages from the
  // MediaSink connected to the route. Note that MediaRouter does not own
  // |observer|. |observer| should be unregistered before it is destroyed.
  // Registering the same observer more than once will result in undefined
  // behavior.
  virtual void RegisterPresentationSessionMessagesObserver(
      PresentationSessionMessagesObserver* observer) = 0;

  // Unregisters a previously registered PresentationSessionMessagesObserver.
  // |observer| will stop receiving further updates.
  virtual void UnregisterPresentationSessionMessagesObserver(
      PresentationSessionMessagesObserver* observer) = 0;
};

}  // namespace media_router

#endif  // CHROME_BROWSER_MEDIA_ROUTER_MEDIA_ROUTER_H_
