// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ScriptPromiseResolver_h
#define ScriptPromiseResolver_h

#include "bindings/core/v8/ScopedPersistent.h"
#include "bindings/core/v8/ScriptPromise.h"
#include "bindings/core/v8/ScriptState.h"
#include "bindings/core/v8/ToV8.h"
#include "core/CoreExport.h"
#include "core/dom/ActiveDOMObject.h"
#include "core/dom/ExecutionContext.h"
#include "platform/Timer.h"
#include "wtf/RefCounted.h"
#include <v8.h>

namespace blink {

// This class wraps v8::Promise::Resolver and provides the following
// functionalities.
//  - A ScriptPromiseResolver retains a ScriptState. A caller
//    can call resolve or reject from outside of a V8 context.
//  - This class is an ActiveDOMObject and keeps track of the associated
//    ExecutionContext state. When the ExecutionContext is suspended,
//    resolve or reject will be delayed. When it is stopped, resolve or reject
//    will be ignored.
class CORE_EXPORT ScriptPromiseResolver : public GarbageCollectedFinalized<ScriptPromiseResolver>, public ActiveDOMObject {
    WILL_BE_USING_GARBAGE_COLLECTED_MIXIN(ScriptPromiseResolver);
    WTF_MAKE_NONCOPYABLE(ScriptPromiseResolver);
public:
    static ScriptPromiseResolver* create(ScriptState* scriptState)
    {
        ScriptPromiseResolver* resolver = new ScriptPromiseResolver(scriptState);
        resolver->suspendIfNeeded();
        return resolver;
    }

#if ENABLE(ASSERT)
    // Eagerly finalized so as to ensure valid access to executionContext()
    // from the destructor's assert.
    EAGERLY_FINALIZE();

    ~ScriptPromiseResolver() override
    {
        // This assertion fails if:
        //  - promise() is called at least once and
        //  - this resolver is destructed before it is resolved, rejected,
        //    detached, the V8 isolate is terminated or the associated
        //    ExecutionContext is stopped.
        ASSERT(m_state == Detached || !m_isPromiseCalled || !scriptState()->contextIsValid() || !executionContext() || executionContext()->activeDOMObjectsAreStopped());
    }
#endif

    // Anything that can be passed to toV8 can be passed to this function.
    template<typename T>
    void resolve(T value)
    {
        resolveOrReject(value, Resolving);
    }

    // Anything that can be passed to toV8 can be passed to this function.
    template<typename T>
    void reject(T value)
    {
        resolveOrReject(value, Rejecting);
    }

    void resolve() { resolve(ToV8UndefinedGenerator()); }
    void reject() { reject(ToV8UndefinedGenerator()); }

    ScriptState* scriptState() { return m_scriptState.get(); }

    // Note that an empty ScriptPromise will be returned after resolve or
    // reject is called.
    ScriptPromise promise()
    {
#if ENABLE(ASSERT)
        m_isPromiseCalled = true;
#endif
        return m_resolver.promise();
    }

    ScriptState* scriptState() const { return m_scriptState.get(); }

    // ActiveDOMObject implementation.
    void suspend() override;
    void resume() override;
    void stop() override { detach(); }

    // Calling this function makes the resolver release its internal resources.
    // That means the associated promise will never be resolved or rejected
    // unless it's already been resolved or rejected.
    // Do not call this function unless you truly need the behavior.
    void detach();

    // Once this function is called this resolver stays alive while the
    // promise is pending and the associated ExecutionContext isn't stopped.
    void keepAliveWhilePending();

    DECLARE_VIRTUAL_TRACE();

protected:
    // You need to call suspendIfNeeded after the construction because
    // this is an ActiveDOMObject.
    explicit ScriptPromiseResolver(ScriptState*);

private:
    typedef ScriptPromise::InternalResolver Resolver;
    enum ResolutionState {
        Pending,
        Resolving,
        Rejecting,
        Detached,
    };

    template<typename T>
    void resolveOrReject(T value, ResolutionState newState)
    {
        if (m_state != Pending || !scriptState()->contextIsValid() || !executionContext() || executionContext()->activeDOMObjectsAreStopped())
            return;
        ASSERT(newState == Resolving || newState == Rejecting);
        m_state = newState;

        ScriptState::Scope scope(m_scriptState.get());
        m_value.set(
            m_scriptState->isolate(),
            toV8(value, m_scriptState->context()->Global(), m_scriptState->isolate()));

        if (executionContext()->activeDOMObjectsAreSuspended()) {
            // Retain this object until it is actually resolved or rejected.
            keepAliveWhilePending();
            return;
        }
        resolveOrRejectImmediately();
    }

    void resolveOrRejectImmediately();
    void onTimerFired(Timer<ScriptPromiseResolver>*);

    ResolutionState m_state;
    const RefPtr<ScriptState> m_scriptState;
    Timer<ScriptPromiseResolver> m_timer;
    Resolver m_resolver;
    ScopedPersistent<v8::Value> m_value;

    // To support keepAliveWhilePending(), this object needs to keep itself
    // alive while in that state.
    SelfKeepAlive<ScriptPromiseResolver> m_keepAlive;

#if ENABLE(ASSERT)
    // True if promise() is called.
    bool m_isPromiseCalled;
#endif
};

} // namespace blink

#endif // ScriptPromiseResolver_h
