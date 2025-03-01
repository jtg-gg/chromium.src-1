/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
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

#ifndef PageRuntimeAgent_h
#define PageRuntimeAgent_h

#include "bindings/core/v8/ScriptState.h"
#include "core/CoreExport.h"
#include "core/inspector/InspectorRuntimeAgent.h"
#include "wtf/PassOwnPtr.h"

namespace blink {

class InspectedFrames;
class SecurityOrigin;

class CORE_EXPORT PageRuntimeAgent final : public InspectorRuntimeAgent {
public:
    static PassOwnPtrWillBeRawPtr<PageRuntimeAgent> create(InspectorRuntimeAgent::Client* client, V8Debugger* debugger, InspectedFrames* inspectedFrames)
    {
        return adoptPtrWillBeNoop(new PageRuntimeAgent(client, debugger, inspectedFrames));
    }
    ~PageRuntimeAgent() override;
    DECLARE_VIRTUAL_TRACE();
    void init() override;
    void enable(ErrorString*) override;
    void disable(ErrorString*) override;

    void didClearDocumentOfWindowObject(LocalFrame*);
    void didCreateScriptContext(LocalFrame*, ScriptState*, SecurityOrigin*, int worldId);
    void willReleaseScriptContext(LocalFrame*, ScriptState*);

private:
    PageRuntimeAgent(Client*, V8Debugger*, InspectedFrames*);

    ScriptState* defaultScriptState() override;
    void muteConsole() override;
    void unmuteConsole() override;
    void reportExecutionContextCreation();
    void reportExecutionContext(ScriptState*, bool isPageContext, const String& origin, const String& frameId);

    RawPtrWillBeMember<InspectedFrames> m_inspectedFrames;
    bool m_mainWorldContextCreated;
};

} // namespace blink


#endif // !defined(InspectorPagerAgent_h)
