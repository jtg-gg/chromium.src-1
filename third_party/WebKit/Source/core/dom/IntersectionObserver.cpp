// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/dom/IntersectionObserver.h"

#include "bindings/core/v8/ExceptionState.h"
#include "core/css/parser/CSSParserTokenRange.h"
#include "core/css/parser/CSSTokenizer.h"
#include "core/dom/ExceptionCode.h"
#include "core/dom/ExecutionContext.h"
#include "core/dom/IntersectionObserverCallback.h"
#include "core/dom/IntersectionObserverController.h"
#include "core/dom/IntersectionObserverEntry.h"
#include "core/dom/IntersectionObserverInit.h"
#include "core/dom/NodeIntersectionObserverData.h"
#include "core/html/HTMLFrameOwnerElement.h"
#include "core/layout/LayoutView.h"
#include "core/timing/DOMWindowPerformance.h"
#include "core/timing/Performance.h"
#include "platform/Timer.h"
#include "wtf/MainThread.h"
#include <algorithm>

namespace blink {

static void parseRootMargin(String rootMarginParameter, Vector<Length>& rootMargin, ExceptionState& exceptionState)
{
    // TODO(szager): Make sure this exact syntax and behavior is spec-ed somewhere.

    // The root margin argument accepts syntax similar to that for CSS margin:
    //
    // "1px" = top/right/bottom/left
    // "1px 2px" = top/bottom left/right
    // "1px 2px 3px" = top left/right bottom
    // "1px 2px 3px 4px" = top left right bottom
    //
    // Any extra stuff after the first four tokens is ignored.
    CSSTokenizer::Scope tokenizerScope(rootMarginParameter);
    CSSParserTokenRange tokenRange = tokenizerScope.tokenRange();
    while (rootMargin.size() < 4 && tokenRange.peek().type() != EOFToken && !exceptionState.hadException()) {
        const CSSParserToken& token = tokenRange.consumeIncludingWhitespace();
        switch (token.type()) {
        case PercentageToken:
            rootMargin.append(Length(token.numericValue(), Percent));
            break;
        case DimensionToken:
            switch (token.unitType()) {
            case CSSPrimitiveValue::UnitType::Pixels:
                rootMargin.append(Length(static_cast<int>(floor(token.numericValue())), Fixed));
                break;
            case CSSPrimitiveValue::UnitType::Percentage:
                rootMargin.append(Length(token.numericValue(), Percent));
                break;
            default:
                exceptionState.throwTypeError("rootMargin must be specified in pixels or percent.");
            }
            break;
        default:
            exceptionState.throwTypeError("rootMargin must be specified in pixels or percent.");
        }
    }
}

static void parseThresholds(const DoubleOrDoubleArray& thresholdParameter, Vector<float>& thresholds, ExceptionState& exceptionState)
{
    if (thresholdParameter.isDouble()) {
        thresholds.append(static_cast<float>(thresholdParameter.getAsDouble()));
    } else {
        for (auto thresholdValue : thresholdParameter.getAsDoubleArray())
            thresholds.append(static_cast<float>(thresholdValue));
    }

    for (auto thresholdValue : thresholds) {
        if (thresholdValue < 0.0 || thresholdValue > 1.0) {
            exceptionState.throwTypeError("Threshold values must be between 0 and 1");
            break;
        }
    }

    std::sort(thresholds.begin(), thresholds.end());
}

IntersectionObserver* IntersectionObserver::create(const IntersectionObserverInit& observerInit, IntersectionObserverCallback& callback, ExceptionState& exceptionState)
{
    RefPtrWillBeRawPtr<Node> root = observerInit.root();
    if (!root) {
        // TODO(szager): Use Document instead of document element for implicit root. (crbug.com/570538)
        ExecutionContext* context = callback.executionContext();
        ASSERT(context->isDocument());
        Frame* mainFrame = toDocument(context)->frame()->tree().top();
        if (mainFrame && mainFrame->isLocalFrame())
            root = toLocalFrame(mainFrame)->document();
    }
    if (!root) {
        exceptionState.throwDOMException(HierarchyRequestError, "Unable to get root node in main frame to track.");
        return nullptr;
    }

    Vector<Length> rootMargin;
    if (observerInit.hasRootMargin())
        parseRootMargin(observerInit.rootMargin(), rootMargin, exceptionState);
    if (exceptionState.hadException())
        return nullptr;

    Vector<float> thresholds;
    if (observerInit.hasThreshold())
        parseThresholds(observerInit.threshold(), thresholds, exceptionState);
    else
        thresholds.append(0);
    if (exceptionState.hadException())
        return nullptr;

    return new IntersectionObserver(callback, *root, rootMargin, thresholds);
}

IntersectionObserver::IntersectionObserver(IntersectionObserverCallback& callback, Node& root, const Vector<Length>& rootMargin, const Vector<float>& thresholds)
    : m_callback(&callback)
    , m_thresholds(thresholds)
    , m_topMargin(Fixed)
    , m_rightMargin(Fixed)
    , m_bottomMargin(Fixed)
    , m_leftMargin(Fixed)
{
    if (root.isDocumentNode())
        m_root = toDocument(root).ensureIntersectionObserverData().createWeakPtr(&root);
    else
        m_root = toElement(root).ensureIntersectionObserverData().createWeakPtr(&root);
    switch (rootMargin.size()) {
    case 0:
        break;
    case 1:
        m_topMargin = m_rightMargin = m_bottomMargin = m_leftMargin = rootMargin[0];
        break;
    case 2:
        m_topMargin = m_bottomMargin = rootMargin[0];
        m_rightMargin = m_leftMargin = rootMargin[1];
        break;
    case 3:
        m_topMargin = rootMargin[0];
        m_rightMargin = m_leftMargin = rootMargin[1];
        m_bottomMargin = rootMargin[2];
        break;
    case 4:
        m_topMargin = rootMargin[0];
        m_rightMargin = rootMargin[1];
        m_bottomMargin = rootMargin[2];
        m_leftMargin = rootMargin[3];
        break;
    default:
        ASSERT_NOT_REACHED();
        break;
    }
    root.document().ensureIntersectionObserverController().addTrackedObserver(*this);
}

#if ENABLE(OILPAN)
void IntersectionObserver::clearWeakMembers(Visitor* visitor)
{
    if (Heap::isHeapObjectAlive(m_root))
        return;
    disconnect();
    m_root = nullptr;
}
#endif

LayoutObject* IntersectionObserver::rootLayoutObject() const
{
    Node* node = rootNode();
    if (node->isDocumentNode())
        return toDocument(node)->layoutView();
    return toElement(node)->layoutObject();
}

void IntersectionObserver::observe(Element* target, ExceptionState& exceptionState)
{
    if (!m_root) {
        exceptionState.throwDOMException(HierarchyRequestError, "Invalid observer: root element or containing document has been deleted.");
        return;
    }
    if (!target) {
        exceptionState.throwTypeError("Observation target must be an element.");
        return;
    }
    if (m_root.get() == target) {
        exceptionState.throwDOMException(HierarchyRequestError, "Cannot use the same element for root and target.");
        return;
    }

    // TODO(szager): Add a pointer to the spec that describes this policy.
    bool shouldReportRootBounds = target->document().frame()->securityContext()->securityOrigin()->canAccess(rootNode()->document().frame()->securityContext()->securityOrigin());
    if (!shouldReportRootBounds && hasPercentMargin()) {
        exceptionState.throwDOMException(HierarchyRequestError, "Cannot observe a cross-origin target because the observer has a root margin value specified as a percent.");
        return;
    }

    if (target->ensureIntersectionObserverData().getObservationFor(*this))
        return;

    IntersectionObservation* observation = new IntersectionObservation(*this, *target, shouldReportRootBounds);
    target->ensureIntersectionObserverData().addObservation(*observation);
    m_observations.add(observation);
}

void IntersectionObserver::unobserve(Element* target, ExceptionState&)
{
    if (!target || !target->intersectionObserverData())
        return;
    // TODO(szager): unobserve callback
    if (IntersectionObservation* observation = target->intersectionObserverData()->getObservationFor(*this))
        observation->disconnect();
}

void IntersectionObserver::computeIntersectionObservations()
{
    if (!m_root || !m_root->inDocument())
        return;
    Document* callbackDocument = toDocument(m_callback->executionContext());
    if (!callbackDocument)
        return;
    LocalDOMWindow* callbackDOMWindow = callbackDocument->domWindow();
    if (!callbackDOMWindow)
        return;
    DOMHighResTimeStamp timestamp = DOMWindowPerformance::performance(*callbackDOMWindow)->now();
    for (auto& observation : m_observations)
        observation->computeIntersectionObservations(timestamp);
}

void IntersectionObserver::disconnect()
{
    for (auto& observation : m_observations)
        observation->clearRootAndRemoveFromTarget();
    m_observations.clear();
}

void IntersectionObserver::removeObservation(IntersectionObservation& observation)
{
    m_observations.remove(&observation);
}

HeapVector<Member<IntersectionObserverEntry>> IntersectionObserver::takeRecords()
{
    HeapVector<Member<IntersectionObserverEntry>> entries;
    entries.swap(m_entries);
    return entries;
}

Element* IntersectionObserver::root() const
{
    Node* node = rootNode();
    if (node->isDocumentNode())
        return nullptr;
    return toElement(node);
}

static void appendLength(StringBuilder& stringBuilder, const Length& length)
{
    stringBuilder.appendNumber(length.intValue());
    if (length.type() == Percent)
        stringBuilder.append('%');
    else
        stringBuilder.append("px", 2);
}

String IntersectionObserver::rootMargin() const
{
    StringBuilder stringBuilder;
    appendLength(stringBuilder, m_topMargin);
    stringBuilder.append(' ');
    appendLength(stringBuilder, m_rightMargin);
    stringBuilder.append(' ');
    appendLength(stringBuilder, m_bottomMargin);
    stringBuilder.append(' ');
    appendLength(stringBuilder, m_leftMargin);
    return stringBuilder.toString();
}

void IntersectionObserver::enqueueIntersectionObserverEntry(IntersectionObserverEntry& entry)
{
    m_entries.append(&entry);
    toDocument(m_callback->executionContext())->ensureIntersectionObserverController().scheduleIntersectionObserverForDelivery(*this);
}

static LayoutUnit computeMargin(const Length& length, LayoutUnit referenceLength)
{
    if (length.type() == Percent)
        return LayoutUnit(static_cast<int>(referenceLength.toFloat() * length.percent() / 100.0));
    ASSERT(length.type() == Fixed);
    return LayoutUnit(length.intValue());
}

void IntersectionObserver::applyRootMargin(LayoutRect& rect) const
{
    // TODO(szager): Make sure the spec is clear that left/right margins are resolved against
    // width and not height.
    LayoutUnit topMargin = computeMargin(m_topMargin, rect.height());
    LayoutUnit rightMargin = computeMargin(m_rightMargin, rect.width());
    LayoutUnit bottomMargin = computeMargin(m_bottomMargin, rect.height());
    LayoutUnit leftMargin = computeMargin(m_leftMargin, rect.width());

    rect.setX(rect.x() - leftMargin);
    rect.setWidth(rect.width() + leftMargin + rightMargin);
    rect.setY(rect.y() - topMargin);
    rect.setHeight(rect.height() + topMargin + bottomMargin);
}

unsigned IntersectionObserver::firstThresholdGreaterThan(float ratio) const
{
    unsigned result = 0;

    // Special handling for zero threshold, which means "any non-zero number of pixels."
    // If the ratio is zero, then it should be treated as smaller than any threshold,
    // even a zero threshold.
    if (!ratio)
        return 0;

    while (result < m_thresholds.size() && m_thresholds[result] <= ratio)
        ++result;
    return result;
}

void IntersectionObserver::deliver()
{

    if (m_entries.isEmpty())
        return;

    HeapVector<Member<IntersectionObserverEntry>> entries;
    entries.swap(m_entries);
    m_callback->handleEvent(entries, *this);
}

bool IntersectionObserver::hasPercentMargin() const
{
    return (m_topMargin.type() == Percent
        || m_rightMargin.type() == Percent
        || m_bottomMargin.type() == Percent
        || m_leftMargin.type() == Percent);
}

DEFINE_TRACE(IntersectionObserver)
{
#if ENABLE(OILPAN)
    visitor->template registerWeakMembers<IntersectionObserver, &IntersectionObserver::clearWeakMembers>(this);
#endif
    visitor->trace(m_callback);
    visitor->trace(m_observations);
    visitor->trace(m_entries);
}

} // namespace blink
