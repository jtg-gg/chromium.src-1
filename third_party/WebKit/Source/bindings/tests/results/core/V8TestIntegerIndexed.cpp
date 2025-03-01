// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file has been auto-generated by code_generator_v8.py. DO NOT MODIFY!

#include "V8TestIntegerIndexed.h"

#include "bindings/core/v8/ExceptionState.h"
#include "bindings/core/v8/V8DOMConfiguration.h"
#include "bindings/core/v8/V8Document.h"
#include "bindings/core/v8/V8Node.h"
#include "bindings/core/v8/V8ObjectConstructor.h"
#include "core/dom/Document.h"
#include "wtf/GetPtr.h"
#include "wtf/RefPtr.h"

namespace blink {

// Suppress warning: global constructors, because struct WrapperTypeInfo is trivial
// and does not depend on another global objects.
#if defined(COMPONENT_BUILD) && defined(WIN32) && COMPILER(CLANG)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wglobal-constructors"
#endif
const WrapperTypeInfo V8TestIntegerIndexed::wrapperTypeInfo = { gin::kEmbedderBlink, V8TestIntegerIndexed::domTemplate, V8TestIntegerIndexed::refObject, V8TestIntegerIndexed::derefObject, V8TestIntegerIndexed::trace, 0, V8TestIntegerIndexed::preparePrototypeAndInterfaceObject, V8TestIntegerIndexed::installConditionallyEnabledProperties, "TestIntegerIndexed", 0, WrapperTypeInfo::WrapperTypeObjectPrototype, WrapperTypeInfo::ObjectClassId, WrapperTypeInfo::NotInheritFromEventTarget, WrapperTypeInfo::Independent, WrapperTypeInfo::RefCountedObject };
#if defined(COMPONENT_BUILD) && defined(WIN32) && COMPILER(CLANG)
#pragma clang diagnostic pop
#endif

// This static member must be declared by DEFINE_WRAPPERTYPEINFO in TestIntegerIndexed.h.
// For details, see the comment of DEFINE_WRAPPERTYPEINFO in
// bindings/core/v8/ScriptWrappable.h.
const WrapperTypeInfo& TestIntegerIndexed::s_wrapperTypeInfo = V8TestIntegerIndexed::wrapperTypeInfo;

namespace TestIntegerIndexedV8Internal {

static void lengthAttributeGetter(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Local<v8::Object> holder = info.Holder();
    TestIntegerIndexed* impl = V8TestIntegerIndexed::toImpl(holder);
    v8SetReturnValueInt(info, impl->length());
}

static void lengthAttributeGetterCallback(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    TestIntegerIndexedV8Internal::lengthAttributeGetter(info);
}

static void lengthAttributeSetter(v8::Local<v8::Value> v8Value, const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Local<v8::Object> holder = info.Holder();
    ExceptionState exceptionState(ExceptionState::SetterContext, "length", "TestIntegerIndexed", holder, info.GetIsolate());
    TestIntegerIndexed* impl = V8TestIntegerIndexed::toImpl(holder);
    int cppValue = toInt16(info.GetIsolate(), v8Value, NormalConversion, exceptionState);
    if (exceptionState.throwIfNeeded())
        return;
    impl->setLength(cppValue);
}

static void lengthAttributeSetterCallback(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Local<v8::Value> v8Value = info[0];
    TestIntegerIndexedV8Internal::lengthAttributeSetter(v8Value, info);
}

static void voidMethodDocumentMethod(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    if (UNLIKELY(info.Length() < 1)) {
        V8ThrowException::throwException(createMinimumArityTypeErrorForMethod(info.GetIsolate(), "voidMethodDocument", "TestIntegerIndexed", 1, info.Length()), info.GetIsolate());
        return;
    }
    TestIntegerIndexed* impl = V8TestIntegerIndexed::toImpl(info.Holder());
    Document* document;
    {
        document = V8Document::toImplWithTypeCheck(info.GetIsolate(), info[0]);
        if (!document) {
            V8ThrowException::throwTypeError(info.GetIsolate(), ExceptionMessages::failedToExecute("voidMethodDocument", "TestIntegerIndexed", "parameter 1 is not of type 'Document'."));
            return;
        }
    }
    impl->voidMethodDocument(document);
}

static void voidMethodDocumentMethodCallback(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    TestIntegerIndexedV8Internal::voidMethodDocumentMethod(info);
}

static void indexedPropertyGetterCallback(uint32_t index, const v8::PropertyCallbackInfo<v8::Value>& info)
{
    V8TestIntegerIndexed::indexedPropertyGetterCustom(index, info);
}

static void indexedPropertySetterCallback(uint32_t index, v8::Local<v8::Value> v8Value, const v8::PropertyCallbackInfo<v8::Value>& info)
{
    V8TestIntegerIndexed::indexedPropertySetterCustom(index, v8Value, info);
}

static void indexedPropertyDeleterCallback(uint32_t index, const v8::PropertyCallbackInfo<v8::Boolean>& info)
{
    V8TestIntegerIndexed::indexedPropertyDeleterCustom(index, info);
}

static void namedPropertyGetterCallback(v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info)
{
    V8TestIntegerIndexed::namedPropertyGetterCustom(name, info);
}

static void namedPropertySetterCallback(v8::Local<v8::Name> name, v8::Local<v8::Value> v8Value, const v8::PropertyCallbackInfo<v8::Value>& info)
{
    V8TestIntegerIndexed::namedPropertySetterCustom(name, v8Value, info);
}

static void namedPropertyQueryCallback(v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Integer>& info)
{
    V8TestIntegerIndexed::namedPropertyQueryCustom(name, info);
}

static void namedPropertyDeleterCallback(v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Boolean>& info)
{
    V8TestIntegerIndexed::namedPropertyDeleterCustom(name, info);
}

static void namedPropertyEnumeratorCallback(const v8::PropertyCallbackInfo<v8::Array>& info)
{
    V8TestIntegerIndexed::namedPropertyEnumeratorCustom(info);
}

} // namespace TestIntegerIndexedV8Internal

const V8DOMConfiguration::AccessorConfiguration V8TestIntegerIndexedAccessors[] = {
    {"length", TestIntegerIndexedV8Internal::lengthAttributeGetterCallback, TestIntegerIndexedV8Internal::lengthAttributeSetterCallback, 0, 0, 0, v8::DEFAULT, static_cast<v8::PropertyAttribute>(v8::None), V8DOMConfiguration::ExposedToAllScripts, V8DOMConfiguration::OnPrototype, V8DOMConfiguration::CheckHolder},
};

const V8DOMConfiguration::MethodConfiguration V8TestIntegerIndexedMethods[] = {
    {"voidMethodDocument", TestIntegerIndexedV8Internal::voidMethodDocumentMethodCallback, 0, 1, v8::None, V8DOMConfiguration::ExposedToAllScripts, V8DOMConfiguration::OnPrototype},
};

static void installV8TestIntegerIndexedTemplate(v8::Local<v8::FunctionTemplate> functionTemplate, v8::Isolate* isolate)
{
    functionTemplate->ReadOnlyPrototype();

    v8::Local<v8::Signature> defaultSignature;
    defaultSignature = V8DOMConfiguration::installDOMClassTemplate(isolate, functionTemplate, V8TestIntegerIndexed::wrapperTypeInfo.interfaceName, v8::Local<v8::FunctionTemplate>(), V8TestIntegerIndexed::internalFieldCount,
        0, 0,
        V8TestIntegerIndexedAccessors, WTF_ARRAY_LENGTH(V8TestIntegerIndexedAccessors),
        V8TestIntegerIndexedMethods, WTF_ARRAY_LENGTH(V8TestIntegerIndexedMethods));
    v8::Local<v8::ObjectTemplate> instanceTemplate = functionTemplate->InstanceTemplate();
    ALLOW_UNUSED_LOCAL(instanceTemplate);
    v8::Local<v8::ObjectTemplate> prototypeTemplate = functionTemplate->PrototypeTemplate();
    ALLOW_UNUSED_LOCAL(prototypeTemplate);
    V8DOMConfiguration::setClassString(isolate, prototypeTemplate, V8TestIntegerIndexed::wrapperTypeInfo.interfaceName);
    if (RuntimeEnabledFeatures::iterableCollectionsEnabled()) {
        prototypeTemplate->SetIntrinsicDataProperty(v8::Symbol::GetIterator(isolate), v8::kArrayProto_values, v8::DontEnum);
    }
    v8::IndexedPropertyHandlerConfiguration indexedPropertyHandlerConfig(TestIntegerIndexedV8Internal::indexedPropertyGetterCallback, TestIntegerIndexedV8Internal::indexedPropertySetterCallback, 0, TestIntegerIndexedV8Internal::indexedPropertyDeleterCallback, indexedPropertyEnumerator<TestIntegerIndexed>, v8::Local<v8::Value>(), v8::PropertyHandlerFlags::kNone);
    instanceTemplate->SetHandler(indexedPropertyHandlerConfig);
    v8::NamedPropertyHandlerConfiguration namedPropertyHandlerConfig(TestIntegerIndexedV8Internal::namedPropertyGetterCallback, TestIntegerIndexedV8Internal::namedPropertySetterCallback, TestIntegerIndexedV8Internal::namedPropertyQueryCallback, TestIntegerIndexedV8Internal::namedPropertyDeleterCallback, TestIntegerIndexedV8Internal::namedPropertyEnumeratorCallback, v8::Local<v8::Value>(), static_cast<v8::PropertyHandlerFlags>(int(v8::PropertyHandlerFlags::kOnlyInterceptStrings) | int(v8::PropertyHandlerFlags::kNonMasking)));
    instanceTemplate->SetHandler(namedPropertyHandlerConfig);
}

v8::Local<v8::FunctionTemplate> V8TestIntegerIndexed::domTemplate(v8::Isolate* isolate)
{
    return V8DOMConfiguration::domClassTemplate(isolate, const_cast<WrapperTypeInfo*>(&wrapperTypeInfo), installV8TestIntegerIndexedTemplate);
}

bool V8TestIntegerIndexed::hasInstance(v8::Local<v8::Value> v8Value, v8::Isolate* isolate)
{
    return V8PerIsolateData::from(isolate)->hasInstance(&wrapperTypeInfo, v8Value);
}

v8::Local<v8::Object> V8TestIntegerIndexed::findInstanceInPrototypeChain(v8::Local<v8::Value> v8Value, v8::Isolate* isolate)
{
    return V8PerIsolateData::from(isolate)->findInstanceInPrototypeChain(&wrapperTypeInfo, v8Value);
}

TestIntegerIndexed* V8TestIntegerIndexed::toImplWithTypeCheck(v8::Isolate* isolate, v8::Local<v8::Value> value)
{
    return hasInstance(value, isolate) ? toImpl(v8::Local<v8::Object>::Cast(value)) : 0;
}

void V8TestIntegerIndexed::refObject(ScriptWrappable* scriptWrappable)
{
    scriptWrappable->toImpl<TestIntegerIndexed>()->ref();
}

void V8TestIntegerIndexed::derefObject(ScriptWrappable* scriptWrappable)
{
    scriptWrappable->toImpl<TestIntegerIndexed>()->deref();
}

} // namespace blink
