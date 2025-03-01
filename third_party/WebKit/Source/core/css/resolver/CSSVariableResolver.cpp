// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/resolver/CSSVariableResolver.h"

#include "core/CSSPropertyNames.h"
#include "core/CSSValueKeywords.h"
#include "core/StyleBuilderFunctions.h"
#include "core/StylePropertyShorthand.h"
#include "core/css/CSSValuePool.h"
#include "core/css/CSSVariableData.h"
#include "core/css/CSSVariableReferenceValue.h"
#include "core/css/parser/CSSParserToken.h"
#include "core/css/parser/CSSParserTokenRange.h"
#include "core/css/parser/CSSParserValues.h"
#include "core/css/parser/CSSPropertyParser.h"
#include "core/css/resolver/StyleBuilder.h"
#include "core/css/resolver/StyleResolverState.h"
#include "core/style/StyleVariableData.h"
#include "wtf/Vector.h"

namespace blink {

bool CSSVariableResolver::resolveFallback(CSSParserTokenRange range, Vector<CSSParserToken>& result)
{
    if (range.atEnd())
        return false;
    ASSERT(range.peek().type() == CommaToken);
    range.consume();
    return resolveTokenRange(range, result);
}

CSSVariableData* CSSVariableResolver::valueForCustomProperty(AtomicString name)
{
    if (m_variablesSeen.contains(name)) {
        m_cycleStartPoints.add(name);
        return nullptr;
    }

    if (!m_styleVariableData)
        return nullptr;
    CSSVariableData* variableData = m_styleVariableData->getVariable(name);
    if (!variableData)
        return nullptr;
    if (!variableData->needsVariableResolution())
        return variableData;
    RefPtr<CSSVariableData> newVariableData = resolveCustomProperty(name, *variableData);
    m_styleVariableData->setVariable(name, newVariableData);
    return newVariableData.get();
}

PassRefPtr<CSSVariableData> CSSVariableResolver::resolveCustomProperty(AtomicString name, const CSSVariableData& variableData)
{
    ASSERT(variableData.needsVariableResolution());

    Vector<CSSParserToken> tokens;
    m_variablesSeen.add(name);
    bool success = resolveTokenRange(variableData.tokens(), tokens);
    m_variablesSeen.remove(name);

    // The old variable data holds onto the backing string the new resolved CSSVariableData
    // relies on. Ensure it will live beyond us overwriting the RefPtr in StyleVariableData.
    ASSERT(variableData.refCount() > 1);

    if (!success || !m_cycleStartPoints.isEmpty()) {
        m_cycleStartPoints.remove(name);
        return nullptr;
    }
    return CSSVariableData::createResolved(tokens, variableData);
}

bool CSSVariableResolver::resolveVariableReference(CSSParserTokenRange range, Vector<CSSParserToken>& result)
{
    range.consumeWhitespace();
    ASSERT(range.peek().type() == IdentToken);
    AtomicString variableName = range.consumeIncludingWhitespace().value();
    ASSERT(range.atEnd() || (range.peek().type() == CommaToken));

    CSSVariableData* variableData = valueForCustomProperty(variableName);
    if (!variableData)
        return resolveFallback(range, result);

    result.appendVector(variableData->tokens());
    Vector<CSSParserToken> trash;
    resolveFallback(range, trash);
    return true;
}

bool CSSVariableResolver::resolveTokenRange(CSSParserTokenRange range,
    Vector<CSSParserToken>& result)
{
    bool success = true;
    while (!range.atEnd()) {
        if (range.peek().functionId() == CSSValueVar) {
            success &= resolveVariableReference(range.consumeBlock(), result);
        } else {
            result.append(range.consume());
        }
    }
    return success;
}

PassRefPtrWillBeRawPtr<CSSValue> CSSVariableResolver::resolveVariableReferences(StyleVariableData* styleVariableData, CSSPropertyID id, const CSSVariableReferenceValue& value)
{
    ASSERT(!isShorthandProperty(id));

    CSSVariableResolver resolver(styleVariableData);
    Vector<CSSParserToken> tokens;
    if (!resolver.resolveTokenRange(value.variableDataValue()->tokens(), tokens))
        return cssValuePool().createUnsetValue();

    CSSParserContext context(HTMLStandardMode, nullptr);
    WillBeHeapVector<CSSProperty, 256> parsedProperties;
    // TODO(timloh): This should be CSSParser::parseSingleValue and not need a vector.
    if (!CSSPropertyParser::parseValue(id, false, CSSParserTokenRange(tokens), context, parsedProperties, StyleRule::RuleType::Style))
        return cssValuePool().createUnsetValue();
    ASSERT(parsedProperties.size() == 1);
    return parsedProperties[0].value();
}

void CSSVariableResolver::resolveAndApplyVariableReferences(StyleResolverState& state, CSSPropertyID id, const CSSVariableReferenceValue& value)
{
    CSSVariableResolver resolver(state.style()->variables());

    Vector<CSSParserToken> tokens;
    if (resolver.resolveTokenRange(value.variableDataValue()->tokens(), tokens)) {
        CSSParserContext context(HTMLStandardMode, 0);

        WillBeHeapVector<CSSProperty, 256> parsedProperties;

        if (CSSPropertyParser::parseValue(id, false, CSSParserTokenRange(tokens), context, parsedProperties, StyleRule::RuleType::Style)) {
            unsigned parsedPropertiesCount = parsedProperties.size();
            for (unsigned i = 0; i < parsedPropertiesCount; ++i)
                StyleBuilder::applyProperty(parsedProperties[i].id(), state, parsedProperties[i].value());
            return;
        }
    }

    RefPtrWillBeRawPtr<CSSUnsetValue> unset = cssValuePool().createUnsetValue();
    if (isShorthandProperty(id)) {
        StylePropertyShorthand shorthand = shorthandForProperty(id);
        for (unsigned i = 0; i < shorthand.length(); i++)
            StyleBuilder::applyProperty(shorthand.properties()[i], state, unset.get());
        return;
    }

    StyleBuilder::applyProperty(id, state, unset.get());
}

void CSSVariableResolver::resolveVariableDefinitions(StyleVariableData* variables)
{
    if (!variables)
        return;

    CSSVariableResolver resolver(variables);
    for (auto& variable : variables->m_data) {
        if (variable.value && variable.value->needsVariableResolution())
            variable.value = resolver.resolveCustomProperty(variable.key, *variable.value);
    }
}

CSSVariableResolver::CSSVariableResolver(StyleVariableData* styleVariableData)
    : m_styleVariableData(styleVariableData)
{
}

} // namespace blink
