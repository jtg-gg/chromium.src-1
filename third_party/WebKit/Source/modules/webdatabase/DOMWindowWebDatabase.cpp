/*
 * Copyright (C) 2006, 2007, 2008, 2010 Apple Inc. All rights reserved.
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies)
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
 */

#include "modules/webdatabase/DOMWindowWebDatabase.h"

#include "bindings/core/v8/ExceptionState.h"
#include "core/dom/Document.h"
#include "core/dom/ExceptionCode.h"
#include "core/frame/LocalDOMWindow.h"
#include "modules/webdatabase/Database.h"
#include "modules/webdatabase/DatabaseCallback.h"
#include "modules/webdatabase/DatabaseManager.h"
#include "platform/RuntimeEnabledFeatures.h"
#include "platform/weborigin/SecurityOrigin.h"

namespace blink {

Database* DOMWindowWebDatabase::openDatabase(DOMWindow& windowArg, const String& name, const String& version, const String& displayName, unsigned long estimatedSize, const String& immediateCommand, DatabaseCallback* creationCallback, ExceptionState& exceptionState)
{
    LocalDOMWindow& window = toLocalDOMWindow(windowArg);
    if (!window.isCurrentlyDisplayedInFrame())
        return nullptr;

    Database* database = nullptr;
    DatabaseManager& dbManager = DatabaseManager::manager();
    DatabaseError error = DatabaseError::None;
    if (RuntimeEnabledFeatures::databaseEnabled() && window.document()->securityOrigin()->canAccessDatabase()) {
        String errorMessage;
        database = dbManager.openDatabase(window.document(), name, version, displayName, estimatedSize, immediateCommand, creationCallback, error, errorMessage);
        ASSERT(database || error != DatabaseError::None);
        if (error != DatabaseError::None)
            DatabaseManager::throwExceptionForDatabaseError(error, errorMessage, exceptionState);
    } else {
        exceptionState.throwSecurityError("Access to the WebDatabase API is denied in this context.");
    }

    return database;
}

Database* DOMWindowWebDatabase::openDatabase(DOMWindow& windowArg, const String& name, const String& version, const String& displayName, unsigned long estimatedSize, ExceptionState& exceptionState) {
    return openDatabase(windowArg, name, version, displayName, estimatedSize, "", NULL, exceptionState);
}

} // namespace blink
