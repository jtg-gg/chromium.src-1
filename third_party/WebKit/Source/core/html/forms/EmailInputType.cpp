/*
 * This file is part of the WebKit project.
 *
 * Copyright (C) 2009 Michelangelo De Simone <micdesim@gmail.com>
 * Copyright (C) 2010 Google Inc. All rights reserved.
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
 *
 */

#include "core/html/forms/EmailInputType.h"

#include "bindings/core/v8/ScriptRegexp.h"
#include "core/InputTypeNames.h"
#include "core/html/HTMLInputElement.h"
#include "core/html/parser/HTMLParserIdioms.h"
#include "core/page/ChromeClient.h"
#include "platform/text/PlatformLocale.h"
#include "public/platform/Platform.h"
#include "wtf/LeakAnnotations.h"
#include "wtf/PassOwnPtr.h"
#include "wtf/text/StringBuilder.h"
#include <unicode/idna.h>
#include <unicode/unistr.h>

namespace blink {

using blink::WebLocalizedString;

// http://www.whatwg.org/specs/web-apps/current-work/multipage/states-of-the-type-attribute.html#valid-e-mail-address
static const char localPartCharacters[] = "abcdefghijklmnopqrstuvwxyz0123456789!#$%&'*+/=?^_`{|}~.-";
static const char emailPattern[] =
    "[a-z0-9!#$%&'*+/=?^_`{|}~.-]+" // local part
    "@"
    "[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?" // domain part
    "(?:\\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)*";

// RFC5321 says the maximum total length of a domain name is 255 octets.
static const int32_t maximumDomainNameLength = 255;
// Use the same option as in url/url_canon_icu.cc
static const int32_t idnaConversionOption = UIDNA_CHECK_BIDI;

String EmailInputType::convertEmailAddressToASCII(const String& address)
{
    if (address.containsOnlyASCII())
        return address;

    size_t atPosition = address.find('@');
    if (atPosition == kNotFound)
        return address;
    String host = address.substring(atPosition + 1);

    // UnicodeString ctor for copy-on-write does not work reliably (in debug
    // build.) TODO(jshin): In an unlikely case this is a perf-issue, treat
    // 8bit and non-8bit strings separately.
    icu::UnicodeString idnDomainName(host.charactersWithNullTermination().data(), host.length());
    icu::UnicodeString domainName;

    // Leak |idna| at the end.
    UErrorCode errorCode = U_ZERO_ERROR;
    static icu::IDNA *idna = icu::IDNA::createUTS46Instance(idnaConversionOption, errorCode);
    ASSERT(idna);
    icu::IDNAInfo idnaInfo;
    idna->nameToASCII(idnDomainName, domainName, idnaInfo, errorCode);
    if (U_FAILURE(errorCode) || idnaInfo.hasErrors() || domainName.length() > maximumDomainNameLength)
        return address;

    StringBuilder builder;
    builder.append(address, 0, atPosition + 1);
    builder.append(domainName.getBuffer(), domainName.length());
    String asciiEmail = builder.toString();
    return isValidEmailAddress(asciiEmail) ? asciiEmail : address;
}

String EmailInputType::convertEmailAddressToUnicode(const String& address) const
{
    if (!address.containsOnlyASCII())
        return address;

    size_t atPosition = address.find('@');
    if (atPosition == kNotFound)
        return address;

    if (address.find("xn--", atPosition + 1) == kNotFound)
        return address;

    if (!chromeClient())
        return address;

    String languages = chromeClient()->acceptLanguages();
    String unicodeHost = Platform::current()->convertIDNToUnicode(address.substring(atPosition + 1), languages);
    StringBuilder builder;
    builder.append(address, 0, atPosition + 1);
    builder.append(unicodeHost);
    return builder.toString();
}

static bool isInvalidLocalPartCharacter(UChar ch)
{
    if (!isASCII(ch))
        return true;
    DEFINE_STATIC_LOCAL(const String, validCharacters, (localPartCharacters));
    return validCharacters.find(toASCIILower(ch)) == kNotFound;
}

static bool isInvalidDomainCharacter(UChar ch)
{
    if (!isASCII(ch))
        return true;
    return !isASCIILower(ch) && !isASCIIUpper(ch) && !isASCIIDigit(ch) && ch != '.' && ch != '-';
}

static bool checkValidDotUsage(const String& domain)
{
    if (domain.isEmpty())
        return true;
    if (domain[0] == '.' || domain[domain.length() - 1] == '.')
        return false;
    return domain.find("..") == kNotFound;
}

bool EmailInputType::isValidEmailAddress(const String& address)
{
    int addressLength = address.length();
    if (!addressLength)
        return false;

    LEAK_SANITIZER_DISABLED_SCOPE;
    DEFINE_STATIC_LOCAL(const ScriptRegexp, regExp, (emailPattern, TextCaseInsensitive));

    int matchLength;
    int matchOffset = regExp.match(address, 0, &matchLength);

    return !matchOffset && matchLength == addressLength;
}

PassRefPtrWillBeRawPtr<InputType> EmailInputType::create(HTMLInputElement& element)
{
    return adoptRefWillBeNoop(new EmailInputType(element));
}

void EmailInputType::countUsage()
{
    countUsageIfVisible(UseCounter::InputTypeEmail);
    bool hasMaxLength = element().fastHasAttribute(HTMLNames::maxlengthAttr);
    if (hasMaxLength)
        countUsageIfVisible(UseCounter::InputTypeEmailMaxLength);
    if (element().multiple()) {
        countUsageIfVisible(UseCounter::InputTypeEmailMultiple);
        if (hasMaxLength)
            countUsageIfVisible(UseCounter::InputTypeEmailMultipleMaxLength);
    }
}

const AtomicString& EmailInputType::formControlType() const
{
    return InputTypeNames::email;
}

// The return value is an invalid email address string if the specified string
// contains an invalid email address. Otherwise, null string is returned.
// If an empty string is returned, it means empty address is specified.
// e.g. "foo@example.com,,bar@example.com" for multiple case.
String EmailInputType::findInvalidAddress(const String& value) const
{
    if (value.isEmpty())
        return String();
    if (!element().multiple())
        return isValidEmailAddress(value) ? String() : value;
    Vector<String> addresses;
    value.split(',', true, addresses);
    for (unsigned i = 0; i < addresses.size(); ++i) {
        String stripped = stripLeadingAndTrailingHTMLSpaces(addresses[i]);
        if (!isValidEmailAddress(stripped))
            return stripped;
    }
    return String();
}

bool EmailInputType::typeMismatchFor(const String& value) const
{
    return !findInvalidAddress(value).isNull();
}

bool EmailInputType::typeMismatch() const
{
    return typeMismatchFor(element().value());
}

String EmailInputType::typeMismatchText() const
{
    String invalidAddress = findInvalidAddress(element().value());
    ASSERT(!invalidAddress.isNull());
    if (invalidAddress.isEmpty())
        return locale().queryString(WebLocalizedString::ValidationTypeMismatchForEmailEmpty);
    String atSign = String("@");
    size_t atIndex = invalidAddress.find('@');
    if (atIndex == kNotFound)
        return locale().queryString(WebLocalizedString::ValidationTypeMismatchForEmailNoAtSign, atSign, invalidAddress);
    // We check validity against an ASCII value because of difficulty to check
    // invalid characters. However we should show Unicode value.
    String unicodeAddress = convertEmailAddressToUnicode(invalidAddress);
    String localPart = invalidAddress.left(atIndex);
    String domain = invalidAddress.substring(atIndex + 1);
    if (localPart.isEmpty())
        return locale().queryString(WebLocalizedString::ValidationTypeMismatchForEmailEmptyLocal, atSign, unicodeAddress);
    if (domain.isEmpty())
        return locale().queryString(WebLocalizedString::ValidationTypeMismatchForEmailEmptyDomain, atSign, unicodeAddress);
    size_t invalidCharIndex = localPart.find(isInvalidLocalPartCharacter);
    if (invalidCharIndex != kNotFound) {
        unsigned charLength = U_IS_LEAD(localPart[invalidCharIndex]) ? 2 : 1;
        return locale().queryString(WebLocalizedString::ValidationTypeMismatchForEmailInvalidLocal, atSign, localPart.substring(invalidCharIndex, charLength));
    }
    invalidCharIndex = domain.find(isInvalidDomainCharacter);
    if (invalidCharIndex != kNotFound) {
        unsigned charLength = U_IS_LEAD(domain[invalidCharIndex]) ? 2 : 1;
        return locale().queryString(WebLocalizedString::ValidationTypeMismatchForEmailInvalidDomain, atSign, domain.substring(invalidCharIndex, charLength));
    }
    if (!checkValidDotUsage(domain)) {
        size_t atIndexInUnicode = unicodeAddress.find('@');
        ASSERT(atIndexInUnicode != kNotFound);
        return locale().queryString(WebLocalizedString::ValidationTypeMismatchForEmailInvalidDots, String("."), unicodeAddress.substring(atIndexInUnicode + 1));
    }
    if (element().multiple())
        return locale().queryString(WebLocalizedString::ValidationTypeMismatchForMultipleEmail);
    return locale().queryString(WebLocalizedString::ValidationTypeMismatchForEmail);
}

void EmailInputType::warnIfValueIsInvalid(const String& value) const
{
    String invalidAddress = findInvalidAddress(value);
    if (invalidAddress.isNull())
        return;
    addWarningToConsole("The specified value %s is not a valid email address.", invalidAddress);
}

bool EmailInputType::supportsSelectionAPI() const
{
    return false;
}

String EmailInputType::sanitizeValue(const String& proposedValue) const
{
    String noLineBreakValue = proposedValue.removeCharacters(isHTMLLineBreak);
    if (!element().multiple())
        return stripLeadingAndTrailingHTMLSpaces(noLineBreakValue);
    Vector<String> addresses;
    noLineBreakValue.split(',', true, addresses);
    StringBuilder strippedValue;
    for (size_t i = 0; i < addresses.size(); ++i) {
        if (i > 0)
            strippedValue.append(',');
        strippedValue.append(stripLeadingAndTrailingHTMLSpaces(addresses[i]));
    }
    return strippedValue.toString();
}

String EmailInputType::convertFromVisibleValue(const String& visibleValue) const
{
    String sanitizedValue = sanitizeValue(visibleValue);
    if (!element().multiple())
        return convertEmailAddressToASCII(sanitizedValue);
    Vector<String> addresses;
    sanitizedValue.split(',', true, addresses);
    StringBuilder builder;
    builder.reserveCapacity(sanitizedValue.length());
    for (size_t i = 0; i < addresses.size(); ++i) {
        if (i > 0)
            builder.append(',');
        builder.append(convertEmailAddressToASCII(addresses[i]));
    }
    return builder.toString();
}

String EmailInputType::visibleValue() const
{
    String value = element().value();
    if (!element().multiple())
        return convertEmailAddressToUnicode(value);

    Vector<String> addresses;
    value.split(',', true, addresses);
    StringBuilder builder;
    builder.reserveCapacity(value.length());
    for (size_t i = 0; i < addresses.size(); ++i) {
        if (i > 0)
            builder.append(',');
        builder.append(convertEmailAddressToUnicode(addresses[i]));
    }
    return builder.toString();
}

} // namespace blink
