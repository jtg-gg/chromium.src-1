// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/css/parser/CSSParser.h"

#include "core/css/CSSColorValue.h"
#include "core/css/CSSKeyframeRule.h"
#include "core/css/StyleColor.h"
#include "core/css/StylePropertySet.h"
#include "core/css/StyleRule.h"
#include "core/css/StyleSheetContents.h"
#include "core/css/parser/CSSParserFastPaths.h"
#include "core/css/parser/CSSParserImpl.h"
#include "core/css/parser/CSSPropertyParser.h"
#include "core/css/parser/CSSSelectorParser.h"
#include "core/css/parser/CSSSupportsParser.h"
#include "core/css/parser/CSSTokenizer.h"
#include "core/css/parser/CSSVariableParser.h"
#include "core/layout/LayoutTheme.h"

namespace blink {

bool CSSParser::parseDeclarationList(const CSSParserContext& context, MutableStylePropertySet* propertySet, const String& declaration)
{
    return CSSParserImpl::parseDeclarationList(propertySet, declaration, context);
}

void CSSParser::parseDeclarationListForInspector(const CSSParserContext& context, const String& declaration, CSSParserObserver& observer)
{
    CSSParserImpl::parseDeclarationListForInspector(declaration, context, observer);
}

CSSSelectorList CSSParser::parseSelector(const CSSParserContext& context, StyleSheetContents* styleSheetContents, const String& selector)
{
    CSSTokenizer::Scope scope(selector);
    return CSSSelectorParser::parseSelector(scope.tokenRange(), context, styleSheetContents);
}

CSSSelectorList CSSParser::parsePageSelector(const CSSParserContext& context, StyleSheetContents* styleSheetContents, const String& selector)
{
    CSSTokenizer::Scope scope(selector);
    return CSSParserImpl::parsePageSelector(scope.tokenRange(), styleSheetContents);
}

PassRefPtrWillBeRawPtr<StyleRuleBase> CSSParser::parseRule(const CSSParserContext& context, StyleSheetContents* styleSheet, const String& rule)
{
    return CSSParserImpl::parseRule(rule, context, styleSheet, CSSParserImpl::AllowImportRules);
}

void CSSParser::parseSheet(const CSSParserContext& context, StyleSheetContents* styleSheet, const String& text)
{
    return CSSParserImpl::parseStyleSheet(text, context, styleSheet);
}

void CSSParser::parseSheetForInspector(const CSSParserContext& context, StyleSheetContents* styleSheet, const String& text, CSSParserObserver& observer)
{
    return CSSParserImpl::parseStyleSheetForInspector(text, context, styleSheet, observer);
}

bool CSSParser::parseValue(MutableStylePropertySet* declaration, CSSPropertyID unresolvedProperty, const String& string, bool important, StyleSheetContents* styleSheet)
{
    if (string.isEmpty())
        return false;
    CSSPropertyID resolvedProperty = resolveCSSPropertyID(unresolvedProperty);
    CSSParserMode parserMode = declaration->cssParserMode();
    RefPtrWillBeRawPtr<CSSValue> value = CSSParserFastPaths::maybeParseValue(resolvedProperty, string, parserMode);
    if (value)
        return declaration->setProperty(CSSProperty(resolvedProperty, value.release(), important));
    CSSParserContext context(parserMode, 0);
    if (styleSheet) {
        context = styleSheet->parserContext();
        context.setMode(parserMode);
    }
    return parseValue(declaration, unresolvedProperty, string, important, context);
}

bool CSSParser::parseValueForCustomProperty(MutableStylePropertySet* declaration, const AtomicString& propertyName, const String& value, bool important, StyleSheetContents* styleSheet)
{
    ASSERT(RuntimeEnabledFeatures::cssVariablesEnabled() && CSSVariableParser::isValidVariableName(propertyName));
    if (value.isEmpty())
        return false;
    CSSParserMode parserMode = declaration->cssParserMode();
    CSSParserContext context(parserMode, 0);
    if (styleSheet) {
        context = styleSheet->parserContext();
        context.setMode(parserMode);
    }
    return CSSParserImpl::parseVariableValue(declaration, propertyName, value, important, context);
}

bool CSSParser::parseValue(MutableStylePropertySet* declaration, CSSPropertyID unresolvedProperty, const String& string, bool important, const CSSParserContext& context)
{
    return CSSParserImpl::parseValue(declaration, unresolvedProperty, string, important, context);
}

PassRefPtrWillBeRawPtr<CSSValue> CSSParser::parseSingleValue(CSSPropertyID propertyID, const String& string, const CSSParserContext& context)
{
    if (string.isEmpty())
        return nullptr;
    if (RefPtrWillBeRawPtr<CSSValue> value = CSSParserFastPaths::maybeParseValue(propertyID, string, context.mode()))
        return value;
    RefPtrWillBeRawPtr<MutableStylePropertySet> stylePropertySet = MutableStylePropertySet::create(HTMLQuirksMode);
    bool changed = parseValue(stylePropertySet.get(), propertyID, string, false, context);
    ASSERT_UNUSED(changed, changed == stylePropertySet->hasProperty(propertyID));
    return stylePropertySet->getPropertyCSSValue(propertyID);
}

PassRefPtrWillBeRawPtr<ImmutableStylePropertySet> CSSParser::parseInlineStyleDeclaration(const String& styleString, Element* element)
{
    return CSSParserImpl::parseInlineStyleDeclaration(styleString, element);
}

PassOwnPtr<Vector<double>> CSSParser::parseKeyframeKeyList(const String& keyList)
{
    return CSSParserImpl::parseKeyframeKeyList(keyList);
}

PassRefPtrWillBeRawPtr<StyleRuleKeyframe> CSSParser::parseKeyframeRule(const CSSParserContext& context, const String& rule)
{
    RefPtrWillBeRawPtr<StyleRuleBase> keyframe = CSSParserImpl::parseRule(rule, context, nullptr, CSSParserImpl::KeyframeRules);
    return toStyleRuleKeyframe(keyframe.get());
}

bool CSSParser::parseSupportsCondition(const String& condition)
{
    CSSTokenizer::Scope scope(condition);
    CSSParserImpl parser(strictCSSParserContext());
    return CSSSupportsParser::supportsCondition(scope.tokenRange(), parser) == CSSSupportsParser::Supported;
}

bool CSSParser::parseColor(Color& color, const String& string, bool strict)
{
    if (string.isEmpty())
        return false;

    // The regular color parsers don't resolve named colors, so explicitly
    // handle these first.
    Color namedColor;
    if (namedColor.setNamedColor(string)) {
        color = namedColor;
        return true;
    }

    RefPtrWillBeRawPtr<CSSValue> value = CSSParserFastPaths::parseColor(string, strict ? HTMLStandardMode : HTMLQuirksMode);
    // TODO(timloh): Why is this always strict mode?
    if (!value)
        value = parseSingleValue(CSSPropertyColor, string, strictCSSParserContext());

    if (!value || !value->isColorValue())
        return false;
    color = toCSSColorValue(*value).value();
    return true;
}

bool CSSParser::parseSystemColor(Color& color, const String& colorString)
{
    CSSParserString cssColor;
    cssColor.init(colorString);
    CSSValueID id = cssValueKeywordID(cssColor);
    if (!CSSPropertyParser::isSystemColor(id))
        return false;

    color = LayoutTheme::theme().systemColor(id);
    return true;
}

PassRefPtrWillBeRawPtr<CSSValue> CSSParser::parseFontFaceDescriptor(CSSPropertyID propertyID, const String& propertyValue, const CSSParserContext& context)
{
    StringBuilder builder;
    builder.appendLiteral("@font-face { ");
    builder.append(getPropertyNameString(propertyID));
    builder.appendLiteral(" : ");
    builder.append(propertyValue);
    builder.appendLiteral("; }");
    RefPtrWillBeRawPtr<StyleRuleBase> rule = parseRule(context, nullptr, builder.toString());
    if (!rule || !rule->isFontFaceRule())
        return nullptr;
    return toStyleRuleFontFace(rule.get())->properties().getPropertyCSSValue(propertyID);
}

} // namespace blink
