// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/v8_inspector/RemoteObjectId.h"

#include "platform/inspector_protocol/Parser.h"
#include "platform/inspector_protocol/Values.h"
#include "wtf/PassOwnPtr.h"
#include "wtf/RefPtr.h"
#include "wtf/text/WTFString.h"

namespace blink {

RemoteObjectIdBase::RemoteObjectIdBase() : m_injectedScriptId(0) { }

PassRefPtr<protocol::DictionaryValue> RemoteObjectIdBase::parseInjectedScriptId(const String& objectId)
{
    RefPtr<protocol::Value> parsedValue = protocol::parseJSON(objectId);
    if (!parsedValue || parsedValue->type() != protocol::Value::TypeObject)
        return nullptr;

    RefPtr<protocol::DictionaryValue> parsedObjectId = protocol::DictionaryValue::cast(parsedValue.release());
    bool success = parsedObjectId->getNumber("injectedScriptId", &m_injectedScriptId);
    if (success)
        return parsedObjectId.release();
    return nullptr;
}

RemoteObjectId::RemoteObjectId() : RemoteObjectIdBase(), m_id(0) { }

PassOwnPtr<RemoteObjectId> RemoteObjectId::parse(const String& objectId)
{
    OwnPtr<RemoteObjectId> result = adoptPtr(new RemoteObjectId());
    RefPtr<protocol::DictionaryValue> parsedObjectId = result->parseInjectedScriptId(objectId);
    if (!parsedObjectId)
        return nullptr;

    bool success = parsedObjectId->getNumber("id", &result->m_id);
    if (success)
        return result.release();
    return nullptr;
}

RemoteCallFrameId::RemoteCallFrameId() : RemoteObjectIdBase(), m_frameOrdinal(0), m_asyncStackOrdinal(0) { }

PassOwnPtr<RemoteCallFrameId> RemoteCallFrameId::parse(const String& objectId)
{
    OwnPtr<RemoteCallFrameId> result = adoptPtr(new RemoteCallFrameId());
    RefPtr<protocol::DictionaryValue> parsedObjectId = result->parseInjectedScriptId(objectId);
    if (!parsedObjectId)
        return nullptr;

    bool success = parsedObjectId->getNumber("ordinal", &result->m_frameOrdinal);
    if (!success)
        return nullptr;

    RefPtr<protocol::Value> value = parsedObjectId->get("asyncOrdinal");
    if (value &&!value->asNumber(&result->m_asyncStackOrdinal))
        return nullptr;
    return result.release();
}

} // namespace blink
