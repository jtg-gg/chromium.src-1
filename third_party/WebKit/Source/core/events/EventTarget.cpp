/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2001 Dirk Mueller (mueller@kde.org)
 * Copyright (C) 2004, 2005, 2006, 2007 Apple Inc. All rights reserved.
 * Copyright (C) 2006 Alexey Proskuryakov (ap@webkit.org)
 *           (C) 2007, 2008 Nikolas Zimmermann <zimmermann@kde.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "core/events/EventTarget.h"

#include "bindings/core/v8/ExceptionState.h"
#include "bindings/core/v8/V8DOMActivityLogger.h"
#include "core/dom/ExceptionCode.h"
#include "core/editing/Editor.h"
#include "core/events/Event.h"
#include "core/inspector/InspectorInstrumentation.h"
#include "core/frame/LocalDOMWindow.h"
#include "core/frame/UseCounter.h"
#include "platform/EventDispatchForbiddenScope.h"
#include "wtf/StdLibExtras.h"
#include "wtf/Threading.h"
#include "wtf/Vector.h"

using namespace WTF;

namespace blink {
namespace {

void setDefaultEventListenerOptionsLegacy(EventListenerOptions& options, bool useCapture)
{
    options.setCapture(useCapture);
    options.setPassive(false);
}

void setDefaultEventListenerOptions(EventListenerOptions& options)
{
    // The default for capture is based on whether the eventListenerOptions
    // runtime setting is enabled. That is
    // addEventListener('type', function(e) {}, {});
    // behaves differently under the setting. With the setting off
    // capture is true; with the setting on capture is false.
    if (!options.hasCapture())
        options.setCapture(!RuntimeEnabledFeatures::eventListenerOptionsEnabled());
    if (!options.hasPassive())
        options.setPassive(false);
}

} // namespace

EventTargetData::EventTargetData()
{
}

EventTargetData::~EventTargetData()
{
}

DEFINE_TRACE(EventTargetData)
{
    visitor->trace(eventListenerMap);
}

EventTarget::EventTarget()
{
}

EventTarget::~EventTarget()
{
}

Node* EventTarget::toNode()
{
    return nullptr;
}

const LocalDOMWindow* EventTarget::toDOMWindow() const
{
    return nullptr;
}

LocalDOMWindow* EventTarget::toDOMWindow()
{
    return nullptr;
}

MessagePort* EventTarget::toMessagePort()
{
    return nullptr;
}

inline LocalDOMWindow* EventTarget::executingWindow()
{
    if (ExecutionContext* context = executionContext())
        return context->executingWindow();
    return nullptr;
}

bool EventTarget::addEventListener(const AtomicString& eventType, PassRefPtrWillBeRawPtr<EventListener> listener, bool useCapture)
{
    EventListenerOptions options;
    setDefaultEventListenerOptionsLegacy(options, useCapture);
    return addEventListenerInternal(eventType, listener, options);
}

bool EventTarget::addEventListener(const AtomicString& eventType, PassRefPtrWillBeRawPtr<EventListener> listener, const EventListenerOptionsOrBoolean& optionsUnion)
{
    if (optionsUnion.isBoolean())
        return addEventListener(eventType, listener, optionsUnion.getAsBoolean());
    if (optionsUnion.isEventListenerOptions()) {
        EventListenerOptions options = optionsUnion.getAsEventListenerOptions();
        return addEventListener(eventType, listener, options);
    }
    return addEventListener(eventType, listener);
}

bool EventTarget::addEventListener(const AtomicString& eventType, PassRefPtrWillBeRawPtr<EventListener> listener, EventListenerOptions& options)
{
    setDefaultEventListenerOptions(options);
    return addEventListenerInternal(eventType, listener, options);
}

bool EventTarget::addEventListenerInternal(const AtomicString& eventType, PassRefPtrWillBeRawPtr<EventListener> listener, const EventListenerOptions& options)
{
    if (!listener)
        return false;

    V8DOMActivityLogger* activityLogger = V8DOMActivityLogger::currentActivityLoggerIfIsolatedWorld();
    if (activityLogger) {
        Vector<String> argv;
        argv.append(toNode() ? toNode()->nodeName() : interfaceName());
        argv.append(eventType);
        activityLogger->logEvent("blinkAddEventListener", argv.size(), argv.data());
    }

    return ensureEventTargetData().eventListenerMap.add(eventType, listener, options);
}

bool EventTarget::removeEventListener(const AtomicString& eventType, PassRefPtrWillBeRawPtr<EventListener> listener, bool useCapture)
{
    EventListenerOptions options;
    setDefaultEventListenerOptionsLegacy(options, useCapture);
    return removeEventListenerInternal(eventType, listener, options);
}

bool EventTarget::removeEventListener(const AtomicString& eventType, PassRefPtrWillBeRawPtr<EventListener> listener, const EventListenerOptionsOrBoolean& optionsUnion)
{
    if (optionsUnion.isBoolean())
        return removeEventListener(eventType, listener, optionsUnion.getAsBoolean());
    if (optionsUnion.isEventListenerOptions()) {
        EventListenerOptions options = optionsUnion.getAsEventListenerOptions();
        return removeEventListener(eventType, listener, options);
    }
    return removeEventListener(eventType, listener);
}

bool EventTarget::removeEventListener(const AtomicString& eventType, PassRefPtrWillBeRawPtr<EventListener> listener, EventListenerOptions& options)
{
    setDefaultEventListenerOptions(options);
    return removeEventListenerInternal(eventType, listener, options);
}

bool EventTarget::removeEventListenerInternal(const AtomicString& eventType, PassRefPtrWillBeRawPtr<EventListener> listener, const EventListenerOptions& options)
{
    if (!listener)
        return false;

    EventTargetData* d = eventTargetData();
    if (!d)
        return false;

    size_t indexOfRemovedListener;

    if (!d->eventListenerMap.remove(eventType, listener.get(), options, indexOfRemovedListener))
        return false;

    // Notify firing events planning to invoke the listener at 'index' that
    // they have one less listener to invoke.
    if (!d->firingEventIterators)
        return true;
    for (size_t i = 0; i < d->firingEventIterators->size(); ++i) {
        FiringEventIterator& firingIterator = d->firingEventIterators->at(i);
        if (eventType != firingIterator.eventType)
            continue;

        if (indexOfRemovedListener >= firingIterator.end)
            continue;

        --firingIterator.end;
        // Note that when firing an event listener,
        // firingIterator.iterator indicates the next event listener
        // that would fire, not the currently firing event
        // listener. See EventTarget::fireEventListeners.
        if (indexOfRemovedListener < firingIterator.iterator)
            --firingIterator.iterator;
    }

    return true;
}

bool EventTarget::setAttributeEventListener(const AtomicString& eventType, PassRefPtrWillBeRawPtr<EventListener> listener)
{
    clearAttributeEventListener(eventType);
    if (!listener)
        return false;
    return addEventListener(eventType, listener, false);
}

EventListener* EventTarget::getAttributeEventListener(const AtomicString& eventType)
{
    EventListenerVector* listenerVector = getEventListeners(eventType);
    if (!listenerVector)
        return nullptr;
    for (const auto& eventListener : *listenerVector) {
        EventListener* listener = eventListener.listener.get();
        if (listener->isAttribute() && listener->belongsToTheCurrentWorld())
            return listener;
    }
    return nullptr;
}

bool EventTarget::clearAttributeEventListener(const AtomicString& eventType)
{
    EventListener* listener = getAttributeEventListener(eventType);
    if (!listener)
        return false;
    return removeEventListener(eventType, listener, false);
}

bool EventTarget::dispatchEventForBindings(PassRefPtrWillBeRawPtr<Event> event, ExceptionState& exceptionState)
{
    if (event->type().isEmpty()) {
        exceptionState.throwDOMException(InvalidStateError, "The event provided is uninitialized.");
        return false;
    }
    if (event->isBeingDispatched()) {
        exceptionState.throwDOMException(InvalidStateError, "The event is already being dispatched.");
        return false;
    }

    if (!executionContext())
        return false;

    event->setTrusted(false);

    // Return whether the event was cancelled or not to JS not that it
    // might have actually been default handled; so check only against
    // CanceledByEventHandler.
    return dispatchEventInternal(event) != DispatchEventResult::CanceledByEventHandler;
}

DispatchEventResult EventTarget::dispatchEvent(PassRefPtrWillBeRawPtr<Event> event)
{
    event->setTrusted(true);
    return dispatchEventInternal(event);
}

DispatchEventResult EventTarget::dispatchEventInternal(PassRefPtrWillBeRawPtr<Event> event)
{
    event->setTarget(this);
    event->setCurrentTarget(this);
    event->setEventPhase(Event::AT_TARGET);
    DispatchEventResult dispatchResult = fireEventListeners(event.get());
    event->setEventPhase(0);
    return dispatchResult;
}

void EventTarget::uncaughtExceptionInEventHandler()
{
}

static const AtomicString& legacyType(const Event* event)
{
    if (event->type() == EventTypeNames::transitionend)
        return EventTypeNames::webkitTransitionEnd;

    if (event->type() == EventTypeNames::animationstart)
        return EventTypeNames::webkitAnimationStart;

    if (event->type() == EventTypeNames::animationend)
        return EventTypeNames::webkitAnimationEnd;

    if (event->type() == EventTypeNames::animationiteration)
        return EventTypeNames::webkitAnimationIteration;

    if (event->type() == EventTypeNames::wheel)
        return EventTypeNames::mousewheel;

    return emptyAtom;
}

void EventTarget::countLegacyEvents(const AtomicString& legacyTypeName, EventListenerVector* listenersVector, EventListenerVector* legacyListenersVector)
{
    UseCounter::Feature unprefixedFeature;
    UseCounter::Feature prefixedFeature;
    UseCounter::Feature prefixedAndUnprefixedFeature;
    if (legacyTypeName == EventTypeNames::webkitTransitionEnd) {
        prefixedFeature = UseCounter::PrefixedTransitionEndEvent;
        unprefixedFeature = UseCounter::UnprefixedTransitionEndEvent;
        prefixedAndUnprefixedFeature = UseCounter::PrefixedAndUnprefixedTransitionEndEvent;
    } else if (legacyTypeName == EventTypeNames::webkitAnimationEnd) {
        prefixedFeature = UseCounter::PrefixedAnimationEndEvent;
        unprefixedFeature = UseCounter::UnprefixedAnimationEndEvent;
        prefixedAndUnprefixedFeature = UseCounter::PrefixedAndUnprefixedAnimationEndEvent;
    } else if (legacyTypeName == EventTypeNames::webkitAnimationStart) {
        prefixedFeature = UseCounter::PrefixedAnimationStartEvent;
        unprefixedFeature = UseCounter::UnprefixedAnimationStartEvent;
        prefixedAndUnprefixedFeature = UseCounter::PrefixedAndUnprefixedAnimationStartEvent;
    } else if (legacyTypeName == EventTypeNames::webkitAnimationIteration) {
        prefixedFeature = UseCounter::PrefixedAnimationIterationEvent;
        unprefixedFeature = UseCounter::UnprefixedAnimationIterationEvent;
        prefixedAndUnprefixedFeature = UseCounter::PrefixedAndUnprefixedAnimationIterationEvent;
    } else if (legacyTypeName == EventTypeNames::mousewheel) {
        prefixedFeature = UseCounter::MouseWheelEvent;
        unprefixedFeature = UseCounter::WheelEvent;
        prefixedAndUnprefixedFeature = UseCounter::MouseWheelAndWheelEvent;
    } else {
        return;
    }

    if (LocalDOMWindow* executingWindow = this->executingWindow()) {
        if (legacyListenersVector) {
            if (listenersVector)
                UseCounter::count(executingWindow->document(), prefixedAndUnprefixedFeature);
            else
                UseCounter::count(executingWindow->document(), prefixedFeature);
        } else if (listenersVector) {
            UseCounter::count(executingWindow->document(), unprefixedFeature);
        }
    }
}

DispatchEventResult EventTarget::fireEventListeners(Event* event)
{
    ASSERT(!EventDispatchForbiddenScope::isEventDispatchForbidden());
    ASSERT(event && !event->type().isEmpty());

    EventTargetData* d = eventTargetData();
    if (!d)
        return DispatchEventResult::NotCanceled;

    EventListenerVector* legacyListenersVector = nullptr;
    AtomicString legacyTypeName = legacyType(event);
    if (!legacyTypeName.isEmpty())
        legacyListenersVector = d->eventListenerMap.find(legacyTypeName);

    EventListenerVector* listenersVector = d->eventListenerMap.find(event->type());

    if (listenersVector) {
        fireEventListeners(event, d, *listenersVector);
    } else if (legacyListenersVector) {
        AtomicString unprefixedTypeName = event->type();
        event->setType(legacyTypeName);
        fireEventListeners(event, d, *legacyListenersVector);
        event->setType(unprefixedTypeName);
    }

    Editor::countEvent(executionContext(), event);
    countLegacyEvents(legacyTypeName, listenersVector, legacyListenersVector);
    return dispatchEventResult(*event);
}

void EventTarget::fireEventListeners(Event* event, EventTargetData* d, EventListenerVector& entry)
{
    RefPtrWillBeRawPtr<EventTarget> protect(this);

    // Fire all listeners registered for this event. Don't fire listeners removed
    // during event dispatch. Also, don't fire event listeners added during event
    // dispatch. Conveniently, all new event listeners will be added after or at
    // index |size|, so iterating up to (but not including) |size| naturally excludes
    // new event listeners.

    if (event->type() == EventTypeNames::beforeunload) {
        if (LocalDOMWindow* executingWindow = this->executingWindow()) {
            if (executingWindow->top())
                UseCounter::count(executingWindow->document(), UseCounter::SubFrameBeforeUnloadFired);
            UseCounter::count(executingWindow->document(), UseCounter::DocumentBeforeUnloadFired);
        }
    } else if (event->type() == EventTypeNames::unload) {
        if (LocalDOMWindow* executingWindow = this->executingWindow())
            UseCounter::count(executingWindow->document(), UseCounter::DocumentUnloadFired);
    } else if (event->type() == EventTypeNames::DOMFocusIn || event->type() == EventTypeNames::DOMFocusOut) {
        if (LocalDOMWindow* executingWindow = this->executingWindow())
            UseCounter::count(executingWindow->document(), UseCounter::DOMFocusInOutEvent);
    } else if (event->type() == EventTypeNames::focusin || event->type() == EventTypeNames::focusout) {
        if (LocalDOMWindow* executingWindow = this->executingWindow())
            UseCounter::count(executingWindow->document(), UseCounter::FocusInOutEvent);
    } else if (event->type() == EventTypeNames::textInput) {
        if (LocalDOMWindow* executingWindow = this->executingWindow())
            UseCounter::count(executingWindow->document(), UseCounter::TextInputFired);
    }

    size_t i = 0;
    size_t size = entry.size();
    if (!d->firingEventIterators)
        d->firingEventIterators = adoptPtr(new FiringEventIteratorVector);
    d->firingEventIterators->append(FiringEventIterator(event->type(), i, size));
    while (i < size) {
        RegisteredEventListener& registeredListener = entry[i];

        // Move the iterator past this event listener. This must match
        // the handling of the FiringEventIterator::iterator in
        // EventTarget::removeEventListener.
        ++i;

        if (event->eventPhase() == Event::CAPTURING_PHASE && !registeredListener.useCapture)
            continue;
        if (event->eventPhase() == Event::BUBBLING_PHASE && registeredListener.useCapture)
            continue;

        // If stopImmediatePropagation has been called, we just break out immediately, without
        // handling any more events on this target.
        if (event->immediatePropagationStopped())
            break;

        ExecutionContext* context = executionContext();
        if (!context)
            break;

        event->setHandlingPassive(registeredListener.passive);

        InspectorInstrumentationCookie cookie = InspectorInstrumentation::willHandleEvent(this, event, registeredListener.listener.get(), registeredListener.useCapture);

        // To match Mozilla, the AT_TARGET phase fires both capturing and bubbling
        // event listeners, even though that violates some versions of the DOM spec.
        registeredListener.listener->handleEvent(context, event);
        event->setHandlingPassive(false);

        RELEASE_ASSERT(i <= size);

        InspectorInstrumentation::didHandleEvent(cookie);
    }
    d->firingEventIterators->removeLast();
}

DispatchEventResult EventTarget::dispatchEventResult(const Event& event)
{
    if (event.defaultPrevented())
        return DispatchEventResult::CanceledByEventHandler;
    if (event.defaultHandled())
        return DispatchEventResult::CanceledByDefaultEventHandler;
    return DispatchEventResult::NotCanceled;
}

EventListenerVector* EventTarget::getEventListeners(const AtomicString& eventType)
{
    EventTargetData* data = eventTargetData();
    if (!data)
        return nullptr;
    return data->eventListenerMap.find(eventType);
}

Vector<AtomicString> EventTarget::eventTypes()
{
    EventTargetData* d = eventTargetData();
    return d ? d->eventListenerMap.eventTypes() : Vector<AtomicString>();
}

void EventTarget::removeAllEventListeners()
{
    EventTargetData* d = eventTargetData();
    if (!d)
        return;
    d->eventListenerMap.clear();

    // Notify firing events planning to invoke the listener at 'index' that
    // they have one less listener to invoke.
    if (d->firingEventIterators) {
        for (size_t i = 0; i < d->firingEventIterators->size(); ++i) {
            d->firingEventIterators->at(i).iterator = 0;
            d->firingEventIterators->at(i).end = 0;
        }
    }
}

} // namespace blink
