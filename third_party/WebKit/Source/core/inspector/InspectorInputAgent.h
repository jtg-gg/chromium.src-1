/*
 * Copyright (C) 2012 Google Inc. All rights reserved.
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

#ifndef InspectorInputAgent_h
#define InspectorInputAgent_h

#include "core/CoreExport.h"
#include "core/inspector/InspectorBaseAgent.h"
#include "wtf/Noncopyable.h"
#include "wtf/PassOwnPtr.h"
#include "wtf/text/WTFString.h"

namespace blink {
class InspectedFrames;
class PlatformKeyboardEvent;
class PlatformMouseEvent;

typedef String ErrorString;

class CORE_EXPORT InspectorInputAgent final : public InspectorBaseAgent<InspectorInputAgent, protocol::Frontend::Input>, public protocol::Dispatcher::InputCommandHandler {
    WTF_MAKE_NONCOPYABLE(InspectorInputAgent);
public:
    static PassOwnPtrWillBeRawPtr<InspectorInputAgent> create(InspectedFrames* inspectedFrames)
    {
        return adoptPtrWillBeNoop(new InspectorInputAgent(inspectedFrames));
    }

    ~InspectorInputAgent() override;
    DECLARE_VIRTUAL_TRACE();

    // Methods called from the frontend for simulating input.
    void dispatchTouchEvent(ErrorString*, const String& type, PassOwnPtr<protocol::Array<protocol::Input::TouchPoint>> touchPoints, const Maybe<int>& modifiers, const Maybe<double>& timestamp) override;
private:
    explicit InspectorInputAgent(InspectedFrames*);

    RawPtrWillBeMember<InspectedFrames> m_inspectedFrames;
};


} // namespace blink

#endif // !defined(InspectorInputAgent_h)
