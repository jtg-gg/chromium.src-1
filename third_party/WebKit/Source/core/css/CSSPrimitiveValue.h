/*
 * (C) 1999-2003 Lars Knoll (knoll@kde.org)
 * Copyright (C) 2004, 2005, 2006, 2008 Apple Inc. All rights reserved.
 * Copyright (C) 2007 Alexey Proskuryakov <ap@webkit.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef CSSPrimitiveValue_h
#define CSSPrimitiveValue_h

#include "core/CSSPropertyNames.h"
#include "core/CSSValueKeywords.h"
#include "core/CoreExport.h"
#include "core/css/CSSValue.h"
#include "wtf/BitVector.h"
#include "wtf/Forward.h"
#include "wtf/MathExtras.h"
#include "wtf/PassRefPtr.h"
#include "wtf/TypeTraits.h"
#include "wtf/text/StringHash.h"

namespace blink {

class CSSCalcValue;
class CSSToLengthConversionData;
class Length;
class ComputedStyle;

// Dimension calculations are imprecise, often resulting in values of e.g.
// 44.99998. We need to go ahead and round if we're really close to the next
// integer value.
template<typename T> inline T roundForImpreciseConversion(double value)
{
    value += (value < 0) ? -0.01 : +0.01;
    return ((value > std::numeric_limits<T>::max()) || (value < std::numeric_limits<T>::min())) ? 0 : static_cast<T>(value);
}

template<> inline float roundForImpreciseConversion(double value)
{
    double ceiledValue = ceil(value);
    double proximityToNextInt = ceiledValue - value;
    if (proximityToNextInt <= 0.01 && value > 0)
        return static_cast<float>(ceiledValue);
    if (proximityToNextInt >= 0.99 && value < 0)
        return static_cast<float>(floor(value));
    return static_cast<float>(value);
}

// CSSPrimitiveValues are immutable. This class has manual ref-counting
// of unioned types and does not have the code necessary
// to handle any kind of mutations.
class CORE_EXPORT CSSPrimitiveValue : public CSSValue {
public:
    // These units are iterated through, so be careful when adding or changing the order.
    enum class UnitType {
        Unknown,
        Number,
        Percentage,
        // Length units
        Ems,
        Exs,
        Pixels,
        Centimeters,
        Millimeters,
        Inches,
        Points,
        Picas,
        ViewportWidth,
        ViewportHeight,
        ViewportMin,
        ViewportMax,
        Rems,
        Chs,
        UserUnits, // The SVG term for unitless lengths
        // Angle units
        Degrees,
        Radians,
        Gradians,
        Turns,
        // Time units
        Milliseconds,
        Seconds,
        Hertz,
        Kilohertz,
        // Resolution
        DotsPerPixel,
        DotsPerInch,
        DotsPerCentimeter,
        // Other units
        Fraction,
        Integer,
        Calc,
        CalcPercentageWithNumber,
        CalcPercentageWithLength,
        ValueID,

        // This value is used to handle quirky margins in reflow roots (body, td, and th) like WinIE.
        // The basic idea is that a stylesheet can use the value __qem (for quirky em) instead of em.
        // When the quirky value is used, if you're in quirks mode, the margin will collapse away
        // inside a table cell. This quirk is specified in the HTML spec but our impl is different.
        // TODO: Remove this. crbug.com/443952
        QuirkyEms,
    };

    enum LengthUnitType {
        UnitTypePixels = 0,
        UnitTypePercentage,
        UnitTypeFontSize,
        UnitTypeFontXSize,
        UnitTypeRootFontSize,
        UnitTypeZeroCharacterWidth,
        UnitTypeViewportWidth,
        UnitTypeViewportHeight,
        UnitTypeViewportMin,
        UnitTypeViewportMax,

        // This value must come after the last length unit type to enable iteration over the length unit types.
        LengthUnitTypeCount,
    };

    using CSSLengthArray = Vector<double, CSSPrimitiveValue::LengthUnitTypeCount>;
    using CSSLengthTypeArray = BitVector;

    void accumulateLengthArray(CSSLengthArray&, double multiplier = 1) const;
    void accumulateLengthArray(CSSLengthArray&, CSSLengthTypeArray&, double multiplier = 1) const;

    enum UnitCategory {
        UNumber,
        UPercent,
        ULength,
        UAngle,
        UTime,
        UFrequency,
        UResolution,
        UOther
    };
    static UnitCategory unitCategory(UnitType);
    static float clampToCSSLengthRange(double);

    static void initUnitTable();

    static UnitType fromName(const String& unit);

    bool isAngle() const
    {
        return type() == UnitType::Degrees
            || type() == UnitType::Radians
            || type() == UnitType::Gradians
            || type() == UnitType::Turns;
    }
    bool isFontRelativeLength() const
    {
        return type() == UnitType::QuirkyEms
            || type() == UnitType::Ems
            || type() == UnitType::Exs
            || type() == UnitType::Rems
            || type() == UnitType::Chs;
    }
    bool isQuirkyEms() const { return type() == UnitType::QuirkyEms; }
    bool isViewportPercentageLength() const { return isViewportPercentageLength(type()); }
    static bool isViewportPercentageLength(UnitType type) { return type >= UnitType::ViewportWidth && type <= UnitType::ViewportMax; }
    static bool isLength(UnitType type)
    {
        return (type >= UnitType::Ems && type <= UnitType::UserUnits)
            || type == UnitType::QuirkyEms;
    }
    static inline bool isRelativeUnit(UnitType type)
    {
        return type == UnitType::Percentage
            || type == UnitType::Ems
            || type == UnitType::Exs
            || type == UnitType::Rems
            || type == UnitType::Chs
            || isViewportPercentageLength(type);
    }
    bool isLength() const { return isLength(typeWithCalcResolved()); }
    bool isNumber() const { return typeWithCalcResolved() == UnitType::Number || typeWithCalcResolved() == UnitType::Integer; }
    bool isPercentage() const { return typeWithCalcResolved() == UnitType::Percentage; }
    bool isPx() const { return typeWithCalcResolved() == UnitType::Pixels; }
    bool isTime() const { return type() == UnitType::Seconds || type() == UnitType::Milliseconds; }
    bool isCalculated() const { return type() == UnitType::Calc; }
    bool isCalculatedPercentageWithNumber() const { return typeWithCalcResolved() == UnitType::CalcPercentageWithNumber; }
    bool isCalculatedPercentageWithLength() const { return typeWithCalcResolved() == UnitType::CalcPercentageWithLength; }
    static bool isResolution(UnitType type) { return type >= UnitType::DotsPerPixel && type <= UnitType::DotsPerCentimeter; }
    bool isFlex() const { return typeWithCalcResolved() == UnitType::Fraction; }
    bool isValueID() const { return type() == UnitType::ValueID; }
    bool colorIsDerivedFromElement() const;

    static PassRefPtrWillBeRawPtr<CSSPrimitiveValue> createIdentifier(CSSValueID valueID)
    {
        return adoptRefWillBeNoop(new CSSPrimitiveValue(valueID));
    }
    static PassRefPtrWillBeRawPtr<CSSPrimitiveValue> create(double value, UnitType type)
    {
        return adoptRefWillBeNoop(new CSSPrimitiveValue(value, type));
    }
    static PassRefPtrWillBeRawPtr<CSSPrimitiveValue> create(const Length& value, float zoom)
    {
        return adoptRefWillBeNoop(new CSSPrimitiveValue(value, zoom));
    }
    template<typename T> static PassRefPtrWillBeRawPtr<CSSPrimitiveValue> create(T value)
    {
        static_assert(!std::is_same<T, CSSValueID>::value, "Do not call create() with a CSSValueID; call createIdentifier() instead");
        return adoptRefWillBeNoop(new CSSPrimitiveValue(value));
    }

    ~CSSPrimitiveValue();

    UnitType typeWithCalcResolved() const;

    double computeDegrees() const;
    double computeSeconds() const;

    // Computes a length in pixels, resolving relative lengths
    template<typename T> T computeLength(const CSSToLengthConversionData&) const;

    // Converts to a Length (Fixed, Percent or Calculated)
    Length convertToLength(const CSSToLengthConversionData&) const;

    double getDoubleValue() const;
    float getFloatValue() const { return getValue<float>(); }
    int getIntValue() const { return getValue<int>(); }
    template<typename T> inline T getValue() const { return clampTo<T>(getDoubleValue()); }

    CSSCalcValue* cssCalcValue() const { ASSERT(isCalculated()); return m_value.calc; }

    CSSValueID getValueID() const { return type() == UnitType::ValueID ? m_value.valueID : CSSValueInvalid; }

    template<typename T> inline T convertTo() const; // Defined in CSSPrimitiveValueMappings.h

    static const char* unitTypeToString(UnitType);
    String customCSSText() const;

    bool equals(const CSSPrimitiveValue&) const;

    DECLARE_TRACE_AFTER_DISPATCH();

    static UnitType canonicalUnitTypeForCategory(UnitCategory);
    static double conversionToCanonicalUnitsScaleFactor(UnitType);

    // Returns true and populates lengthUnitType, if unitType is a length unit. Otherwise, returns false.
    static bool unitTypeToLengthUnitType(UnitType, LengthUnitType&);
    static UnitType lengthUnitTypeToUnitType(LengthUnitType);

private:
    CSSPrimitiveValue(CSSValueID);
    CSSPrimitiveValue(const Length&, float zoom);
    CSSPrimitiveValue(double, UnitType);

    template<typename T> CSSPrimitiveValue(T); // Defined in CSSPrimitiveValueMappings.h
    template<typename T> CSSPrimitiveValue(T* val)
        : CSSValue(PrimitiveClass)
    {
        init(PassRefPtrWillBeRawPtr<T>(val));
    }

    template<typename T> CSSPrimitiveValue(PassRefPtrWillBeRawPtr<T> val)
        : CSSValue(PrimitiveClass)
    {
        init(val);
    }

    static void create(int); // compile-time guard
    static void create(unsigned); // compile-time guard
    template<typename T> operator T*(); // compile-time guard

    void init(UnitType);
    void init(const Length&);
    void init(PassRefPtrWillBeRawPtr<CSSCalcValue>);

    double computeLengthDouble(const CSSToLengthConversionData&) const;

    inline UnitType type() const { return static_cast<UnitType>(m_primitiveUnitType); }

    union {
        CSSValueID valueID;
        double num;
        // FIXME: oilpan: Should be a member, but no support for members in unions. Just trace the raw ptr for now.
        CSSCalcValue* calc;
    } m_value;
};

using CSSLengthArray = CSSPrimitiveValue::CSSLengthArray;
using CSSLengthTypeArray = CSSPrimitiveValue::CSSLengthTypeArray;

DEFINE_CSS_VALUE_TYPE_CASTS(CSSPrimitiveValue, isPrimitiveValue());

} // namespace blink

#endif // CSSPrimitiveValue_h
