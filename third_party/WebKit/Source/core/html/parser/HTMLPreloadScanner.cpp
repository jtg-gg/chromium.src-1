/*
 * Copyright (C) 2008 Apple Inc. All Rights Reserved.
 * Copyright (C) 2009 Torch Mobile, Inc. http://www.torchmobile.com/
 * Copyright (C) 2010 Google Inc. All Rights Reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "core/html/parser/HTMLPreloadScanner.h"

#include "core/HTMLNames.h"
#include "core/InputTypeNames.h"
#include "core/css/MediaList.h"
#include "core/css/MediaQueryEvaluator.h"
#include "core/css/MediaValuesCached.h"
#include "core/css/parser/SizesAttributeParser.h"
#include "core/dom/Document.h"
#include "core/fetch/IntegrityMetadata.h"
#include "core/frame/Settings.h"
#include "core/frame/SubresourceIntegrity.h"
#include "core/html/CrossOriginAttribute.h"
#include "core/html/HTMLImageElement.h"
#include "core/html/HTMLMetaElement.h"
#include "core/html/LinkRelAttribute.h"
#include "core/html/parser/HTMLParserIdioms.h"
#include "core/html/parser/HTMLSrcsetParser.h"
#include "core/html/parser/HTMLTokenizer.h"
#include "core/loader/LinkLoader.h"
#include "platform/ContentType.h"
#include "platform/MIMETypeRegistry.h"
#include "platform/RuntimeEnabledFeatures.h"
#include "platform/TraceEvent.h"
#include "wtf/MainThread.h"

namespace blink {

using namespace HTMLNames;

static bool match(const StringImpl* impl, const QualifiedName& qName)
{
    return impl == qName.localName().impl();
}

static bool match(const AtomicString& name, const QualifiedName& qName)
{
    ASSERT(isMainThread());
    return qName.localName() == name;
}

static bool match(const String& name, const QualifiedName& qName)
{
    return threadSafeMatch(name, qName);
}

static const StringImpl* tagImplFor(const HTMLToken::DataVector& data)
{
    AtomicString tagName(data);
    const StringImpl* result = tagName.impl();
    if (result->isStatic())
        return result;
    return nullptr;
}

static const StringImpl* tagImplFor(const String& tagName)
{
    const StringImpl* result = tagName.impl();
    if (result->isStatic())
        return result;
    return nullptr;
}

static String initiatorFor(const StringImpl* tagImpl)
{
    ASSERT(tagImpl);
    if (match(tagImpl, imgTag))
        return imgTag.localName();
    if (match(tagImpl, inputTag))
        return inputTag.localName();
    if (match(tagImpl, linkTag))
        return linkTag.localName();
    if (match(tagImpl, scriptTag))
        return scriptTag.localName();
    if (match(tagImpl, videoTag))
        return videoTag.localName();
    ASSERT_NOT_REACHED();
    return emptyString();
}

static bool mediaAttributeMatches(const MediaValuesCached& mediaValues, const String& attributeValue)
{
    RefPtrWillBeRawPtr<MediaQuerySet> mediaQueries = MediaQuerySet::createOffMainThread(attributeValue);
    MediaQueryEvaluator mediaQueryEvaluator(mediaValues);
    return mediaQueryEvaluator.eval(mediaQueries.get());
}

class TokenPreloadScanner::StartTagScanner {
    STACK_ALLOCATED();
public:
    StartTagScanner(const StringImpl* tagImpl, PassRefPtrWillBeRawPtr<MediaValuesCached> mediaValues)
        : m_tagImpl(tagImpl)
        , m_linkIsStyleSheet(false)
        , m_linkTypeIsMissingOrSupportedStyleSheet(true)
        , m_linkIsPreconnect(false)
        , m_linkIsPreload(false)
        , m_linkIsImport(false)
        , m_matchedMediaAttribute(true)
        , m_inputIsImage(false)
        , m_sourceSize(0)
        , m_sourceSizeSet(false)
        , m_defer(FetchRequest::NoDefer)
        , m_crossOrigin(CrossOriginAttributeNotSet)
        , m_mediaValues(mediaValues)
        , m_referrerPolicySet(false)
        , m_referrerPolicy(ReferrerPolicyDefault)
    {
        if (match(m_tagImpl, imgTag)
            || match(m_tagImpl, sourceTag)) {
            m_sourceSize = SizesAttributeParser(m_mediaValues, String()).length();
            return;
        }
        if ( !match(m_tagImpl, inputTag)
            && !match(m_tagImpl, linkTag)
            && !match(m_tagImpl, scriptTag)
            && !match(m_tagImpl, videoTag))
            m_tagImpl = 0;
    }

    enum URLReplacement {
        AllowURLReplacement,
        DisallowURLReplacement
    };

    void processAttributes(const HTMLToken::AttributeList& attributes)
    {
        ASSERT(isMainThread());
        if (!m_tagImpl)
            return;
        for (const HTMLToken::Attribute& htmlTokenAttribute : attributes) {
            AtomicString attributeName(htmlTokenAttribute.name());
            String attributeValue = htmlTokenAttribute.value8BitIfNecessary();
            processAttribute(attributeName, attributeValue);
        }
    }

    void processAttributes(const Vector<CompactHTMLToken::Attribute>& attributes)
    {
        if (!m_tagImpl)
            return;
        for (const CompactHTMLToken::Attribute& htmlTokenAttribute : attributes)
            processAttribute(htmlTokenAttribute.name(), htmlTokenAttribute.value());
    }

    void handlePictureSourceURL(PictureData& pictureData)
    {
        if (match(m_tagImpl, sourceTag) && m_matchedMediaAttribute && pictureData.sourceURL.isEmpty()) {
            pictureData.sourceURL = m_srcsetImageCandidate.toString();
            pictureData.sourceSizeSet = m_sourceSizeSet;
            pictureData.sourceSize = m_sourceSize;
            pictureData.picked = true;
        } else if (match(m_tagImpl, imgTag) && !pictureData.sourceURL.isEmpty()) {
            setUrlToLoad(pictureData.sourceURL, AllowURLReplacement);
        }
    }

    PassOwnPtr<PreloadRequest> createPreloadRequest(const KURL& predictedBaseURL, const SegmentedString& source, const ClientHintsPreferences& clientHintsPreferences, const PictureData& pictureData, const ReferrerPolicy documentReferrerPolicy)
    {
        PreloadRequest::RequestType requestType = PreloadRequest::RequestTypePreload;
        if (shouldPreconnect())
            requestType = PreloadRequest::RequestTypePreconnect;
        else if (isLinkRelPreload())
            requestType = PreloadRequest::RequestTypeLinkRelPreload;
        else if (!shouldPreload() || !m_matchedMediaAttribute)
            return nullptr;

        TextPosition position = TextPosition(source.currentLine(), source.currentColumn());
        FetchRequest::ResourceWidth resourceWidth;
        float sourceSize = m_sourceSize;
        bool sourceSizeSet = m_sourceSizeSet;
        if (pictureData.picked) {
            sourceSizeSet = pictureData.sourceSizeSet;
            sourceSize = pictureData.sourceSize;
        }
        if (sourceSizeSet) {
            resourceWidth.width = sourceSize;
            resourceWidth.isSet = true;
        }

        Resource::Type type;
        if (!resourceType(type))
            return nullptr;

        // The element's 'referrerpolicy' attribute (if present) takes precedence over the document's referrer policy.
        ReferrerPolicy referrerPolicy = (m_referrerPolicy != ReferrerPolicyDefault && RuntimeEnabledFeatures::referrerPolicyAttributeEnabled()) ? m_referrerPolicy : documentReferrerPolicy;
        OwnPtr<PreloadRequest> request = PreloadRequest::create(initiatorFor(m_tagImpl), position, m_urlToLoad, predictedBaseURL, type, referrerPolicy, resourceWidth, clientHintsPreferences, requestType);
        request->setCrossOrigin(m_crossOrigin);
        request->setCharset(charset());
        request->setDefer(m_defer);
        request->setIntegrityMetadata(m_integrityMetadata);
        return request.release();
    }

private:
    template<typename NameType>
    void processScriptAttribute(const NameType& attributeName, const String& attributeValue)
    {
        // FIXME - Don't set crossorigin multiple times.
        if (match(attributeName, srcAttr))
            setUrlToLoad(attributeValue, DisallowURLReplacement);
        else if (match(attributeName, crossoriginAttr))
            setCrossOrigin(attributeValue);
        else if (match(attributeName, asyncAttr))
            setDefer(FetchRequest::LazyLoad);
        else if (match(attributeName, deferAttr))
            setDefer(FetchRequest::LazyLoad);
        // Note that only scripts need to have the integrity metadata set on
        // preloads. This is because script resources fetches, and only script
        // resource fetches, need to re-request resources if a cached version
        // has different metadata (including empty) from the metadata on the
        // request. See the comment before the call to
        // mustRefetchDueToIntegrityMismatch() in
        // Source/core/fetch/ResourceFetcher.cpp for a more complete
        // explanation.
        else if (match(attributeName, integrityAttr))
            SubresourceIntegrity::parseIntegrityAttribute(attributeValue, m_integrityMetadata);
    }

    template<typename NameType>
    void processImgAttribute(const NameType& attributeName, const String& attributeValue)
    {
        if (match(attributeName, srcAttr) && m_imgSrcUrl.isNull()) {
            m_imgSrcUrl = attributeValue;
            setUrlToLoad(bestFitSourceForImageAttributes(m_mediaValues->devicePixelRatio(), m_sourceSize, attributeValue, m_srcsetImageCandidate), AllowURLReplacement);
        } else if (match(attributeName, crossoriginAttr)) {
            setCrossOrigin(attributeValue);
        } else if (match(attributeName, srcsetAttr) && m_srcsetImageCandidate.isEmpty()) {
            m_srcsetAttributeValue = attributeValue;
            m_srcsetImageCandidate = bestFitSourceForSrcsetAttribute(m_mediaValues->devicePixelRatio(), m_sourceSize, attributeValue);
            setUrlToLoad(bestFitSourceForImageAttributes(m_mediaValues->devicePixelRatio(), m_sourceSize, m_imgSrcUrl, m_srcsetImageCandidate), AllowURLReplacement);
        } else if (match(attributeName, sizesAttr) && !m_sourceSizeSet) {
            m_sourceSize = SizesAttributeParser(m_mediaValues, attributeValue).length();
            m_sourceSizeSet = true;
            if (!m_srcsetImageCandidate.isEmpty()) {
                m_srcsetImageCandidate = bestFitSourceForSrcsetAttribute(m_mediaValues->devicePixelRatio(), m_sourceSize, m_srcsetAttributeValue);
                setUrlToLoad(bestFitSourceForImageAttributes(m_mediaValues->devicePixelRatio(), m_sourceSize, m_imgSrcUrl, m_srcsetImageCandidate), AllowURLReplacement);
            }
        } else if (!m_referrerPolicySet && RuntimeEnabledFeatures::referrerPolicyAttributeEnabled() && match(attributeName, referrerpolicyAttr) && !attributeValue.isNull()) {
            m_referrerPolicySet = true;
            SecurityPolicy::referrerPolicyFromString(attributeValue, &m_referrerPolicy);
        }
    }

    template<typename NameType>
    void processLinkAttribute(const NameType& attributeName, const String& attributeValue)
    {
        // FIXME - Don't set rel/media/crossorigin multiple times.
        if (match(attributeName, hrefAttr)) {
            setUrlToLoad(attributeValue, DisallowURLReplacement);
        } else if (match(attributeName, relAttr)) {
            LinkRelAttribute rel(attributeValue);
            m_linkIsStyleSheet = rel.isStyleSheet() && !rel.isAlternate() && rel.getIconType() == InvalidIcon && !rel.isDNSPrefetch();
            m_linkIsPreconnect = rel.isPreconnect();
            m_linkIsPreload = rel.isLinkPreload();
            m_linkIsImport = rel.isImport();
        } else if (match(attributeName, mediaAttr)) {
            m_matchedMediaAttribute = mediaAttributeMatches(*m_mediaValues, attributeValue);
        } else if (match(attributeName, crossoriginAttr)) {
            setCrossOrigin(attributeValue);
        } else if (match(attributeName, asAttr)) {
            m_asAttributeValue = attributeValue;
        } else if (match(attributeName, typeAttr)) {
            m_linkTypeIsMissingOrSupportedStyleSheet = MIMETypeRegistry::isSupportedStyleSheetMIMEType(ContentType(attributeValue).type());
        }
    }

    template<typename NameType>
    void processInputAttribute(const NameType& attributeName, const String& attributeValue)
    {
        // FIXME - Don't set type multiple times.
        if (match(attributeName, srcAttr))
            setUrlToLoad(attributeValue, DisallowURLReplacement);
        else if (match(attributeName, typeAttr))
            m_inputIsImage = equalIgnoringCase(attributeValue, InputTypeNames::image);
    }

    template<typename NameType>
    void processSourceAttribute(const NameType& attributeName, const String& attributeValue)
    {
        if (match(attributeName, srcsetAttr) && m_srcsetImageCandidate.isEmpty()) {
            m_srcsetAttributeValue = attributeValue;
            m_srcsetImageCandidate = bestFitSourceForSrcsetAttribute(m_mediaValues->devicePixelRatio(), m_sourceSize, attributeValue);
        } else if (match(attributeName, sizesAttr) && !m_sourceSizeSet) {
            m_sourceSize = SizesAttributeParser(m_mediaValues, attributeValue).length();
            m_sourceSizeSet = true;
            if (!m_srcsetImageCandidate.isEmpty()) {
                m_srcsetImageCandidate = bestFitSourceForSrcsetAttribute(m_mediaValues->devicePixelRatio(), m_sourceSize, m_srcsetAttributeValue);
            }
        } else if (match(attributeName, mediaAttr)) {
            // FIXME - Don't match media multiple times.
            m_matchedMediaAttribute = mediaAttributeMatches(*m_mediaValues, attributeValue);
        }
    }

    template<typename NameType>
    void processVideoAttribute(const NameType& attributeName, const String& attributeValue)
    {
        if (match(attributeName, posterAttr))
            setUrlToLoad(attributeValue, DisallowURLReplacement);
    }

    template<typename NameType>
    void processAttribute(const NameType& attributeName, const String& attributeValue)
    {
        if (match(attributeName, charsetAttr))
            m_charset = attributeValue;

        if (match(m_tagImpl, scriptTag))
            processScriptAttribute(attributeName, attributeValue);
        else if (match(m_tagImpl, imgTag))
            processImgAttribute(attributeName, attributeValue);
        else if (match(m_tagImpl, linkTag))
            processLinkAttribute(attributeName, attributeValue);
        else if (match(m_tagImpl, inputTag))
            processInputAttribute(attributeName, attributeValue);
        else if (match(m_tagImpl, sourceTag))
            processSourceAttribute(attributeName, attributeValue);
        else if (match(m_tagImpl, videoTag))
            processVideoAttribute(attributeName, attributeValue);
    }

    void setUrlToLoad(const String& value, URLReplacement replacement)
    {
        // We only respect the first src/href, per HTML5:
        // http://www.whatwg.org/specs/web-apps/current-work/multipage/tokenization.html#attribute-name-state
        if (replacement == DisallowURLReplacement && !m_urlToLoad.isEmpty())
            return;
        String url = stripLeadingAndTrailingHTMLSpaces(value);
        if (url.isEmpty())
            return;
        m_urlToLoad = url;
    }

    const String& charset() const
    {
        // FIXME: Its not clear that this if is needed, the loader probably ignores charset for image requests anyway.
        if (match(m_tagImpl, imgTag) || match(m_tagImpl, videoTag))
            return emptyString();
        return m_charset;
    }

    bool resourceType(Resource::Type& type) const
    {
        if (match(m_tagImpl, scriptTag)) {
            type = Resource::Script;
        } else if (match(m_tagImpl, imgTag) || match(m_tagImpl, videoTag) || (match(m_tagImpl, inputTag) && m_inputIsImage)) {
            type = Resource::Image;
        } else if (match(m_tagImpl, linkTag) && m_linkIsStyleSheet) {
            type = Resource::CSSStyleSheet;
        } else if (m_linkIsPreconnect) {
            type = Resource::Raw;
        } else if (m_linkIsPreload) {
            if (!LinkLoader::getResourceTypeFromAsAttribute(m_asAttributeValue, type))
                return false;
        } else if (match(m_tagImpl, linkTag) && m_linkIsImport) {
            type = Resource::ImportResource;
        } else {
            ASSERT_NOT_REACHED();
        }
        return true;
    }

    bool shouldPreconnect() const
    {
        return match(m_tagImpl, linkTag) && m_linkIsPreconnect && !m_urlToLoad.isEmpty();
    }

    bool isLinkRelPreload() const
    {
        return match(m_tagImpl, linkTag) && m_linkIsPreload && !m_urlToLoad.isEmpty();
    }

    bool shouldPreload() const
    {
        if (m_urlToLoad.isEmpty())
            return false;
        if (match(m_tagImpl, linkTag) && !m_linkIsStyleSheet && !m_linkIsImport && !m_linkIsPreload)
            return false;
        if (match(m_tagImpl, linkTag) && m_linkIsStyleSheet && !m_linkTypeIsMissingOrSupportedStyleSheet)
            return false;
        if (match(m_tagImpl, inputTag) && !m_inputIsImage)
            return false;
        return true;
    }

    void setCrossOrigin(const String& corsSetting)
    {
        m_crossOrigin = crossOriginAttributeValue(corsSetting);
    }

    void setDefer(FetchRequest::DeferOption defer)
    {
        m_defer = defer;
    }

    bool defer() const
    {
        return m_defer;
    }

    const StringImpl* m_tagImpl;
    String m_urlToLoad;
    ImageCandidate m_srcsetImageCandidate;
    String m_charset;
    bool m_linkIsStyleSheet;
    bool m_linkTypeIsMissingOrSupportedStyleSheet;
    bool m_linkIsPreconnect;
    bool m_linkIsPreload;
    bool m_linkIsImport;
    bool m_matchedMediaAttribute;
    bool m_inputIsImage;
    String m_imgSrcUrl;
    String m_srcsetAttributeValue;
    String m_asAttributeValue;
    float m_sourceSize;
    bool m_sourceSizeSet;
    FetchRequest::DeferOption m_defer;
    CrossOriginAttributeValue m_crossOrigin;
    RefPtrWillBeMember<MediaValuesCached> m_mediaValues;
    bool m_referrerPolicySet;
    ReferrerPolicy m_referrerPolicy;
    IntegrityMetadataSet m_integrityMetadata;
};

TokenPreloadScanner::TokenPreloadScanner(const KURL& documentURL, PassOwnPtr<CachedDocumentParameters> documentParameters, const MediaValuesCached::MediaValuesCachedData& mediaValuesCachedData)
    : m_documentURL(documentURL)
    , m_inStyle(false)
    , m_inPicture(false)
    , m_isAppCacheEnabled(false)
    , m_isCSPEnabled(false)
    , m_templateCount(0)
    , m_documentParameters(documentParameters)
    , m_mediaValues(MediaValuesCached::create(mediaValuesCachedData))
{
    ASSERT(m_documentParameters.get());
    ASSERT(m_mediaValues.get());
}

TokenPreloadScanner::~TokenPreloadScanner()
{
}

TokenPreloadScannerCheckpoint TokenPreloadScanner::createCheckpoint()
{
    TokenPreloadScannerCheckpoint checkpoint = m_checkpoints.size();
    m_checkpoints.append(Checkpoint(m_predictedBaseElementURL, m_inStyle, m_isAppCacheEnabled, m_isCSPEnabled, m_templateCount));
    return checkpoint;
}

void TokenPreloadScanner::rewindTo(TokenPreloadScannerCheckpoint checkpointIndex)
{
    ASSERT(checkpointIndex < m_checkpoints.size()); // If this ASSERT fires, checkpointIndex is invalid.
    const Checkpoint& checkpoint = m_checkpoints[checkpointIndex];
    m_predictedBaseElementURL = checkpoint.predictedBaseElementURL;
    m_inStyle = checkpoint.inStyle;
    m_isAppCacheEnabled = checkpoint.isAppCacheEnabled;
    m_isCSPEnabled = checkpoint.isCSPEnabled;
    m_templateCount = checkpoint.templateCount;
    m_cssScanner.reset();
    m_checkpoints.clear();
}

void TokenPreloadScanner::scan(const HTMLToken& token, const SegmentedString& source, PreloadRequestStream& requests)
{
    scanCommon(token, source, requests);
}

void TokenPreloadScanner::scan(const CompactHTMLToken& token, const SegmentedString& source, PreloadRequestStream& requests)
{
    scanCommon(token, source, requests);
}

static void handleMetaViewport(const String& attributeValue, const CachedDocumentParameters* documentParameters, MediaValuesCached* mediaValues)
{
    if (!documentParameters->viewportMetaEnabled)
        return;
    ViewportDescription description(ViewportDescription::ViewportMeta);
    HTMLMetaElement::getViewportDescriptionFromContentAttribute(attributeValue, description, nullptr, documentParameters->viewportMetaZeroValuesQuirk);
    FloatSize initialViewport(mediaValues->deviceWidth(), mediaValues->deviceHeight());
    PageScaleConstraints constraints = description.resolve(initialViewport, documentParameters->defaultViewportMinWidth);
    mediaValues->setViewportHeight(constraints.layoutSize.height());
    mediaValues->setViewportWidth(constraints.layoutSize.width());
}

static void handleMetaReferrer(const String& attributeValue, CachedDocumentParameters* documentParameters, CSSPreloadScanner* cssScanner)
{
    if (attributeValue.isEmpty() || attributeValue.isNull() || !SecurityPolicy::referrerPolicyFromString(attributeValue, &documentParameters->referrerPolicy)) {
        documentParameters->referrerPolicy = ReferrerPolicyDefault;
    }
    cssScanner->setReferrerPolicy(documentParameters->referrerPolicy);
}

template <typename Token>
static void handleMetaNameAttribute(const Token& token, CachedDocumentParameters* documentParameters, MediaValuesCached* mediaValues, CSSPreloadScanner* cssScanner)
{
    const typename Token::Attribute* nameAttribute = token.getAttributeItem(nameAttr);
    if (!nameAttribute)
        return;

    String nameAttributeValue(nameAttribute->value());
    const typename Token::Attribute* contentAttribute = token.getAttributeItem(contentAttr);
    if (!contentAttribute)
        return;

    String contentAttributeValue(contentAttribute->value());
    if (equalIgnoringCase(nameAttributeValue, "viewport")) {
        handleMetaViewport(contentAttributeValue, documentParameters, mediaValues);
        return;
    }

    if (equalIgnoringCase(nameAttributeValue, "referrer")) {
        handleMetaReferrer(contentAttributeValue, documentParameters, cssScanner);
    }
}

template <typename Token>
void TokenPreloadScanner::scanCommon(const Token& token, const SegmentedString& source, PreloadRequestStream& requests)
{
    if (!m_documentParameters->doHtmlPreloadScanning)
        return;

    // Disable preload for documents with AppCache.
    if (m_isAppCacheEnabled)
        return;

    // http://crbug.com/434230 Disable preload for documents with CSP <meta> tags
    if (m_isCSPEnabled)
        return;

    switch (token.type()) {
    case HTMLToken::Character: {
        if (!m_inStyle)
            return;
        m_cssScanner.scan(token.data(), source, requests, m_predictedBaseElementURL);
        return;
    }
    case HTMLToken::EndTag: {
        const StringImpl* tagImpl = tagImplFor(token.data());
        if (match(tagImpl, templateTag)) {
            if (m_templateCount)
                --m_templateCount;
            return;
        }
        if (match(tagImpl, styleTag)) {
            if (m_inStyle)
                m_cssScanner.reset();
            m_inStyle = false;
            return;
        }
        if (match(tagImpl, pictureTag))
            m_inPicture = false;
        return;
    }
    case HTMLToken::StartTag: {
        if (m_templateCount)
            return;
        const StringImpl* tagImpl = tagImplFor(token.data());
        if (match(tagImpl, templateTag)) {
            ++m_templateCount;
            return;
        }
        if (match(tagImpl, styleTag)) {
            m_inStyle = true;
            return;
        }
        if (match(tagImpl, baseTag)) {
            // The first <base> element is the one that wins.
            if (!m_predictedBaseElementURL.isEmpty())
                return;
            updatePredictedBaseURL(token);
            return;
        }
        if (match(tagImpl, htmlTag) && token.getAttributeItem(manifestAttr)) {
            m_isAppCacheEnabled = true;
            return;
        }
        if (match(tagImpl, metaTag)) {
            const typename Token::Attribute* equivAttribute = token.getAttributeItem(http_equivAttr);
            if (equivAttribute) {
                String equivAttributeValue(equivAttribute->value());
                if (equalIgnoringCase(equivAttributeValue, "content-security-policy")) {
                    m_isCSPEnabled = true;
                } else if (equalIgnoringCase(equivAttributeValue, "accept-ch")) {
                    const typename Token::Attribute* contentAttribute = token.getAttributeItem(contentAttr);
                    if (contentAttribute)
                        m_clientHintsPreferences.updateFromAcceptClientHintsHeader(contentAttribute->value(), nullptr);
                }
                return;
            }

            handleMetaNameAttribute(token, m_documentParameters.get(), m_mediaValues.get(), &m_cssScanner);
        }

        if (match(tagImpl, pictureTag)) {
            m_inPicture = true;
            m_pictureData = PictureData();
            return;
        }

        StartTagScanner scanner(tagImpl, m_mediaValues);
        scanner.processAttributes(token.attributes());
        if (m_inPicture)
            scanner.handlePictureSourceURL(m_pictureData);
        OwnPtr<PreloadRequest> request = scanner.createPreloadRequest(m_predictedBaseElementURL, source, m_clientHintsPreferences, m_pictureData, m_documentParameters->referrerPolicy);
        if (request)
            requests.append(request.release());
        return;
    }
    default: {
        return;
    }
    }
}

template<typename Token>
void TokenPreloadScanner::updatePredictedBaseURL(const Token& token)
{
    ASSERT(m_predictedBaseElementURL.isEmpty());
    if (const typename Token::Attribute* hrefAttribute = token.getAttributeItem(hrefAttr)) {
        KURL url(m_documentURL, stripLeadingAndTrailingHTMLSpaces(hrefAttribute->value8BitIfNecessary()));
        m_predictedBaseElementURL = url.isValid() ? url.copy() : KURL();
    }
}

HTMLPreloadScanner::HTMLPreloadScanner(const HTMLParserOptions& options, const KURL& documentURL, PassOwnPtr<CachedDocumentParameters> documentParameters, const MediaValuesCached::MediaValuesCachedData& mediaValuesCachedData)
    : m_scanner(documentURL, documentParameters, mediaValuesCachedData)
    , m_tokenizer(HTMLTokenizer::create(options))
{
}

HTMLPreloadScanner::~HTMLPreloadScanner()
{
}

void HTMLPreloadScanner::appendToEnd(const SegmentedString& source)
{
    m_source.append(source);
}

void HTMLPreloadScanner::scan(ResourcePreloader* preloader, const KURL& startingBaseElementURL)
{
    ASSERT(isMainThread()); // HTMLTokenizer::updateStateFor only works on the main thread.

    TRACE_EVENT1("blink", "HTMLPreloadScanner::scan", "source_length", m_source.length());

    // When we start scanning, our best prediction of the baseElementURL is the real one!
    if (!startingBaseElementURL.isEmpty())
        m_scanner.setPredictedBaseElementURL(startingBaseElementURL);

    PreloadRequestStream requests;

    while (m_tokenizer->nextToken(m_source, m_token)) {
        if (m_token.type() == HTMLToken::StartTag)
            m_tokenizer->updateStateFor(attemptStaticStringCreation(m_token.name(), Likely8Bit));
        m_scanner.scan(m_token, m_source, requests);
        m_token.clear();
    }

    preloader->takeAndPreload(requests);
}

CachedDocumentParameters::CachedDocumentParameters(Document* document)
{
    ASSERT(isMainThread());
    ASSERT(document);
    doHtmlPreloadScanning = !document->settings() || document->settings()->doHtmlPreloadScanning();
    defaultViewportMinWidth = document->viewportDefaultMinWidth();
    viewportMetaZeroValuesQuirk = document->settings() && document->settings()->viewportMetaZeroValuesQuirk();
    viewportMetaEnabled = document->settings() && document->settings()->viewportMetaEnabled();
    referrerPolicy = ReferrerPolicyDefault;
}

} // namespace blink
