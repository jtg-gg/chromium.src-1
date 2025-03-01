// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/animation/SVGNumberListInterpolationType.h"

#include "core/animation/InterpolationEnvironment.h"
#include "core/animation/UnderlyingLengthChecker.h"
#include "core/svg/SVGNumberList.h"

namespace blink {

InterpolationValue SVGNumberListInterpolationType::maybeConvertNeutral(const InterpolationValue& underlying, ConversionCheckers& conversionCheckers) const
{
    size_t underlyingLength = UnderlyingLengthChecker::getUnderlyingLength(underlying);
    conversionCheckers.append(UnderlyingLengthChecker::create(underlyingLength));

    if (underlyingLength == 0)
        return nullptr;

    OwnPtr<InterpolableList> result = InterpolableList::create(underlyingLength);
    for (size_t i = 0; i < underlyingLength; i++)
        result->set(i, InterpolableNumber::create(0));
    return InterpolationValue(result.release());
}

InterpolationValue SVGNumberListInterpolationType::maybeConvertSVGValue(const SVGPropertyBase& svgValue) const
{
    if (svgValue.type() != AnimatedNumberList)
        return nullptr;

    const SVGNumberList& numberList = toSVGNumberList(svgValue);
    OwnPtr<InterpolableList> result = InterpolableList::create(numberList.length());
    for (size_t i = 0; i < numberList.length(); i++)
        result->set(i, InterpolableNumber::create(numberList.at(i)->value()));
    return InterpolationValue(result.release());
}

PairwiseInterpolationValue SVGNumberListInterpolationType::mergeSingleConversions(InterpolationValue& start, InterpolationValue& end) const
{
    size_t startLength = toInterpolableList(*start.interpolableValue).length();
    size_t endLength = toInterpolableList(*end.interpolableValue).length();
    if (startLength != endLength)
        return nullptr;
    return InterpolationType::mergeSingleConversions(start, end);
}

static void padWithZeroes(OwnPtr<InterpolableValue>& listPointer, size_t paddedLength)
{
    InterpolableList& list = toInterpolableList(*listPointer);

    if (list.length() >= paddedLength)
        return;

    OwnPtr<InterpolableList> result = InterpolableList::create(paddedLength);
    size_t i = 0;
    for (; i < list.length(); i++)
        result->set(i, list.getMutable(i).release());
    for (; i < paddedLength; i++)
        result->set(i, InterpolableNumber::create(0));
    listPointer = result.release();
}

void SVGNumberListInterpolationType::composite(UnderlyingValueOwner& underlyingValueOwner, double underlyingFraction, const InterpolationValue& value) const
{
    const InterpolableList& list = toInterpolableList(*value.interpolableValue);

    if (toInterpolableList(*underlyingValueOwner.value().interpolableValue).length() <= list.length())
        padWithZeroes(underlyingValueOwner.mutableValue().interpolableValue, list.length());

    InterpolableList& underlyingList = toInterpolableList(*underlyingValueOwner.mutableValue().interpolableValue);

    ASSERT(underlyingList.length() >= list.length());
    size_t i = 0;
    for (; i < list.length(); i++)
        underlyingList.getMutable(i)->scaleAndAdd(underlyingFraction, *list.get(i));
    for (; i < underlyingList.length(); i++)
        underlyingList.getMutable(i)->scale(underlyingFraction);
}

PassRefPtrWillBeRawPtr<SVGPropertyBase> SVGNumberListInterpolationType::appliedSVGValue(const InterpolableValue& interpolableValue, const NonInterpolableValue*) const
{
    RefPtrWillBeRawPtr<SVGNumberList> result = SVGNumberList::create();
    const InterpolableList& list = toInterpolableList(interpolableValue);
    for (size_t i = 0; i < list.length(); i++)
        result->append(SVGNumber::create(toInterpolableNumber(list.get(i))->value()));
    return result.release();
}

} // namespace blink
