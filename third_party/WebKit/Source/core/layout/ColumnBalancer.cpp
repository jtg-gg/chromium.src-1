// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/layout/ColumnBalancer.h"

#include "core/layout/LayoutMultiColumnFlowThread.h"
#include "core/layout/LayoutMultiColumnSet.h"
#include "core/layout/api/LineLayoutBlockFlow.h"

namespace blink {

ColumnBalancer::ColumnBalancer(const MultiColumnFragmentainerGroup& group)
    : m_group(group)
{
}

void ColumnBalancer::traverse()
{
    traverseSubtree(*m_group.columnSet().flowThread());
    ASSERT(!flowThreadOffset());
}

void ColumnBalancer::traverseSubtree(const LayoutBox& box)
{
    if (box.childrenInline() && box.isLayoutBlockFlow()) {
        // Look for breaks between lines.
        for (const RootInlineBox* line = toLayoutBlockFlow(box).firstRootBox(); line; line = line->nextRootBox()) {
            LayoutUnit lineTopInFlowThread = m_flowThreadOffset + line->lineTopWithLeading();
            if (lineTopInFlowThread < group().logicalTopInFlowThread())
                continue;
            if (lineTopInFlowThread >= group().logicalBottomInFlowThread())
                break;
            examineLine(*line);
        }
    }

    const LayoutFlowThread* flowThread = group().columnSet().flowThread();
    bool isHorizontalWritingMode = flowThread->isHorizontalWritingMode();

    // Look for breaks between and inside block-level children. Even if this is a block flow with
    // inline children, there may be interesting floats to examine here.
    for (const LayoutObject* child = box.slowFirstChild(); child; child = child->nextSibling()) {
        if (!child->isBox() || child->isInline())
            continue;
        const LayoutBox& childBox = toLayoutBox(*child);
        LayoutRect overflowRect = childBox.layoutOverflowRect();
        LayoutUnit childLogicalBottomWithOverflow = childBox.logicalTop() + (isHorizontalWritingMode ? overflowRect.maxY() : overflowRect.maxX());
        if (m_flowThreadOffset + childLogicalBottomWithOverflow <= group().logicalTopInFlowThread()) {
            // This child is fully above the fragmentainer group we're examining.
            continue;
        }
        LayoutUnit childLogicalTopWithOverflow = childBox.logicalTop() + (isHorizontalWritingMode ? overflowRect.y() : overflowRect.x());
        if (m_flowThreadOffset + childLogicalTopWithOverflow >= group().logicalBottomInFlowThread()) {
            // This child is fully below the fragmentainer group we're examining. We cannot just
            // stop here, though, thanks to negative margins. So keep looking.
            continue;
        }
        if (childBox.isOutOfFlowPositioned() || childBox.isColumnSpanAll())
            continue;

        // Tables are wicked. Both table rows and table cells are relative to their table section.
        LayoutUnit offsetForThisChild = childBox.isTableRow() ? LayoutUnit() : childBox.logicalTop();
        m_flowThreadOffset += offsetForThisChild;

        examineBoxAfterEntering(childBox);
        // Unless the child is unsplittable, or if the child establishes an inner multicol
        // container, we descend into its subtree for further examination.
        if (childBox.paginationBreakability() != LayoutBox::ForbidBreaks
            && (!childBox.isLayoutBlockFlow() || !toLayoutBlockFlow(childBox).multiColumnFlowThread()))
            traverseSubtree(childBox);
        examineBoxBeforeLeaving(childBox);

        m_flowThreadOffset -= offsetForThisChild;
    }
}

InitialColumnHeightFinder::InitialColumnHeightFinder(const MultiColumnFragmentainerGroup& group)
    : ColumnBalancer(group)
{
    m_shortestStruts.resize(group.columnSet().usedColumnCount());
    for (auto& strut : m_shortestStruts)
        strut = LayoutUnit::max();
    traverse();
    // We have now found each explicit / forced break, and their location. Now we need to figure out
    // how many additional implicit / soft breaks we need and guess where they will occur, in order
    // to provide an initial column height.
    distributeImplicitBreaks();
}

LayoutUnit InitialColumnHeightFinder::initialMinimalBalancedHeight() const
{
    unsigned index = contentRunIndexWithTallestColumns();
    LayoutUnit startOffset = index > 0 ? m_contentRuns[index - 1].breakOffset() : group().logicalTopInFlowThread();
    return m_contentRuns[index].columnLogicalHeight(startOffset);
}

void InitialColumnHeightFinder::examineBoxAfterEntering(const LayoutBox& box)
{
    if (isLogicalTopWithinBounds(flowThreadOffset() - box.paginationStrut())) {
        ASSERT(isFirstAfterBreak(flowThreadOffset()) || !box.paginationStrut());
        if (box.hasForcedBreakBefore()) {
            addContentRun(flowThreadOffset());
        } else if (isFirstAfterBreak(flowThreadOffset())) {
            // This box is first after a soft break.
            recordStrutBeforeOffset(flowThreadOffset(), box.paginationStrut());
        }
    }

    if (box.hasForcedBreakAfter()) {
        LayoutUnit logicalBottomInFlowThread = flowThreadOffset() + box.logicalHeight();
        if (isLogicalBottomWithinBounds(logicalBottomInFlowThread))
            addContentRun(logicalBottomInFlowThread);
    }

    if (box.paginationBreakability() != LayoutBox::AllowAnyBreaks) {
        LayoutUnit unsplittableLogicalHeight = box.logicalHeight();
        if (box.isFloating())
            unsplittableLogicalHeight += box.marginBefore() + box.marginAfter();
        m_tallestUnbreakableLogicalHeight = std::max(m_tallestUnbreakableLogicalHeight, unsplittableLogicalHeight);
        return;
    }
    // Need to examine inner multicol containers to find their tallest unbreakable piece of content.
    if (!box.isLayoutBlockFlow())
        return;
    LayoutMultiColumnFlowThread* innerFlowThread = toLayoutBlockFlow(box).multiColumnFlowThread();
    if (!innerFlowThread || innerFlowThread->isLayoutPagedFlowThread())
        return;
    LayoutUnit offsetInInnerFlowThread = flowThreadOffset() - innerFlowThread->blockOffsetInEnclosingFragmentationContext();
    LayoutUnit innerUnbreakableHeight = innerFlowThread->tallestUnbreakableLogicalHeight(offsetInInnerFlowThread);
    m_tallestUnbreakableLogicalHeight = std::max(m_tallestUnbreakableLogicalHeight, innerUnbreakableHeight);
}

void InitialColumnHeightFinder::examineBoxBeforeLeaving(const LayoutBox& box)
{
}

static inline LayoutUnit columnLogicalHeightRequirementForLine(const ComputedStyle& style, const RootInlineBox& lastLine)
{
    // We may require a certain minimum number of lines per page in order to satisfy
    // orphans and widows, and that may affect the minimum page height.
    unsigned minimumLineCount = std::max<unsigned>(style.hasAutoOrphans() ? 1 : style.orphans(), style.widows());
    const RootInlineBox* firstLine = &lastLine;
    for (unsigned i = 1; i < minimumLineCount && firstLine->prevRootBox(); i++)
        firstLine = firstLine->prevRootBox();
    return lastLine.lineBottomWithLeading() - firstLine->lineTopWithLeading();
}

void InitialColumnHeightFinder::examineLine(const RootInlineBox& line)
{
    LayoutUnit lineTop = line.lineTopWithLeading();
    LayoutUnit lineTopInFlowThread = flowThreadOffset() + lineTop;
    LayoutUnit minimumLogialHeight = columnLogicalHeightRequirementForLine(line.block().styleRef(), line);
    m_tallestUnbreakableLogicalHeight = std::max(m_tallestUnbreakableLogicalHeight, minimumLogialHeight);
    ASSERT(isFirstAfterBreak(lineTopInFlowThread) || !line.paginationStrut() || !isLogicalTopWithinBounds(lineTopInFlowThread - line.paginationStrut()));
    if (isFirstAfterBreak(lineTopInFlowThread))
        recordStrutBeforeOffset(lineTopInFlowThread, line.paginationStrut());
}

void InitialColumnHeightFinder::recordStrutBeforeOffset(LayoutUnit offsetInFlowThread, LayoutUnit strut)
{
    const LayoutMultiColumnSet& columnSet = group().columnSet();
    ASSERT(columnSet.usedColumnCount() >= 1);
    unsigned columnCount = columnSet.usedColumnCount();
    ASSERT(m_shortestStruts.size() == columnCount);
    unsigned index = group().columnIndexAtOffset(offsetInFlowThread - strut, MultiColumnFragmentainerGroup::AssumeNewColumns);
    if (index >= columnCount)
        return;
    m_shortestStruts[index] = std::min(m_shortestStruts[index], strut);
}

LayoutUnit InitialColumnHeightFinder::spaceUsedByStrutsAt(LayoutUnit offsetInFlowThread) const
{
    unsigned stopBeforeColumn = group().columnIndexAtOffset(offsetInFlowThread, MultiColumnFragmentainerGroup::AssumeNewColumns) + 1;
    stopBeforeColumn = std::min(stopBeforeColumn, group().columnSet().usedColumnCount());
    ASSERT(stopBeforeColumn <= m_shortestStruts.size());
    LayoutUnit totalStrutSpace;
    for (unsigned i = 0; i < stopBeforeColumn; i++) {
        if (m_shortestStruts[i] != LayoutUnit::max())
            totalStrutSpace += m_shortestStruts[i];
    }
    return totalStrutSpace;
}

void InitialColumnHeightFinder::addContentRun(LayoutUnit endOffsetInFlowThread)
{
    endOffsetInFlowThread -= spaceUsedByStrutsAt(endOffsetInFlowThread);
    if (!m_contentRuns.isEmpty() && endOffsetInFlowThread <= m_contentRuns.last().breakOffset())
        return;
    // Append another item as long as we haven't exceeded used column count. What ends up in the
    // overflow area shouldn't affect column balancing.
    if (m_contentRuns.size() < group().columnSet().usedColumnCount())
        m_contentRuns.append(ContentRun(endOffsetInFlowThread));
}

unsigned InitialColumnHeightFinder::contentRunIndexWithTallestColumns() const
{
    unsigned indexWithLargestHeight = 0;
    LayoutUnit largestHeight;
    LayoutUnit previousOffset = group().logicalTopInFlowThread();
    size_t runCount = m_contentRuns.size();
    ASSERT(runCount);
    for (size_t i = 0; i < runCount; i++) {
        const ContentRun& run = m_contentRuns[i];
        LayoutUnit height = run.columnLogicalHeight(previousOffset);
        if (largestHeight < height) {
            largestHeight = height;
            indexWithLargestHeight = i;
        }
        previousOffset = run.breakOffset();
    }
    return indexWithLargestHeight;
}

void InitialColumnHeightFinder::distributeImplicitBreaks()
{
    // Insert a final content run to encompass all content. This will include overflow if this is
    // the last group in the multicol container.
    addContentRun(group().logicalBottomInFlowThread());
    unsigned columnCount = m_contentRuns.size();

    // If there is room for more breaks (to reach the used value of column-count), imagine that we
    // insert implicit breaks at suitable locations. At any given time, the content run with the
    // currently tallest columns will get another implicit break "inserted", which will increase its
    // column count by one and shrink its columns' height. Repeat until we have the desired total
    // number of breaks. The largest column height among the runs will then be the initial column
    // height for the balancer to use.
    while (columnCount < group().columnSet().usedColumnCount()) {
        unsigned index = contentRunIndexWithTallestColumns();
        m_contentRuns[index].assumeAnotherImplicitBreak();
        columnCount++;
    }
}

MinimumSpaceShortageFinder::MinimumSpaceShortageFinder(const MultiColumnFragmentainerGroup& group)
    : ColumnBalancer(group)
    , m_minimumSpaceShortage(LayoutUnit::max())
    , m_pendingStrut(LayoutUnit::min())
    , m_forcedBreaksCount(0)
{
    traverse();
}

void MinimumSpaceShortageFinder::examineBoxAfterEntering(const LayoutBox& box)
{
    LayoutBox::PaginationBreakability breakability = box.paginationBreakability();

    // Look for breaks before the child box.
    if (isLogicalTopWithinBounds(flowThreadOffset() - box.paginationStrut())) {
        ASSERT(isFirstAfterBreak(flowThreadOffset()) || !box.paginationStrut());
        if (box.hasForcedBreakBefore()) {
            m_forcedBreaksCount++;
        } else if (isFirstAfterBreak(flowThreadOffset())) {
            // This box is first after a soft break.
            LayoutUnit strut = box.paginationStrut();
            // Figure out how much more space we would need to prevent it from being pushed to the next column.
            recordSpaceShortage(box.logicalHeight() - strut);
            if (breakability != LayoutBox::ForbidBreaks && m_pendingStrut == LayoutUnit::min()) {
                // We now want to look for the first piece of unbreakable content (e.g. a line or a
                // block-displayed image) inside this block. That ought to be a good candidate for
                // minimum space shortage; a much better one than reporting space shortage for the
                // entire block (which we'll also do (further down), in case we couldn't find anything
                // more suitable).
                m_pendingStrut = strut;
            }
        }
    }

    if (box.hasForcedBreakAfter() && isLogicalBottomWithinBounds(flowThreadOffset() + box.logicalHeight()))
        m_forcedBreaksCount++;

    if (breakability != LayoutBox::ForbidBreaks) {
        // See if this breakable box crosses column boundaries.
        LayoutUnit bottomInFlowThread = flowThreadOffset() + box.logicalHeight();
        if (isFirstAfterBreak(flowThreadOffset())
            || group().columnLogicalTopForOffset(flowThreadOffset()) != group().columnLogicalTopForOffset(bottomInFlowThread)) {
            // If the child crosses a column boundary, record space shortage, in case nothing
            // inside it has already done so. The column balancer needs to know by how much it
            // has to stretch the columns to make more content fit. If no breaks are reported
            // (but do occur), the balancer will have no clue. Only measure the space after the
            // last column boundary, in case it crosses more than one.
            LayoutUnit spaceUsedInLastColumn = bottomInFlowThread - group().columnLogicalTopForOffset(bottomInFlowThread);
            recordSpaceShortage(spaceUsedInLastColumn);
        }
    }

    // If this is an inner multicol container, look for space shortage inside it.
    if (!box.isLayoutBlockFlow())
        return;
    LayoutMultiColumnFlowThread* flowThread = toLayoutBlockFlow(box).multiColumnFlowThread();
    if (!flowThread || flowThread->isLayoutPagedFlowThread())
        return;
    for (const LayoutMultiColumnSet* columnSet = flowThread->firstMultiColumnSet(); columnSet; columnSet = columnSet->nextSiblingMultiColumnSet()) {
        for (const MultiColumnFragmentainerGroup& row : columnSet->fragmentainerGroups()) {
            MinimumSpaceShortageFinder innerFinder(row);
            recordSpaceShortage(innerFinder.minimumSpaceShortage());
        }
    }
}

void MinimumSpaceShortageFinder::examineBoxBeforeLeaving(const LayoutBox& box)
{
    if (m_pendingStrut == LayoutUnit::min() || box.paginationBreakability() != LayoutBox::ForbidBreaks)
        return;

    // The previous break was before a breakable block. Here's the first piece of unbreakable
    // content after / inside that block. We want to record the distance from the top of the column
    // to the bottom of this box as space shortage.
    LayoutUnit logicalOffsetFromCurrentColumn = flowThreadOffset() - group().columnLogicalTopForOffset(flowThreadOffset());
    recordSpaceShortage(logicalOffsetFromCurrentColumn + box.logicalHeight() - m_pendingStrut);
    m_pendingStrut = LayoutUnit::min();
}

void MinimumSpaceShortageFinder::examineLine(const RootInlineBox& line)
{
    LayoutUnit lineTop = line.lineTopWithLeading();
    LayoutUnit lineTopInFlowThread = flowThreadOffset() + lineTop;
    LayoutUnit lineHeight = line.lineBottomWithLeading() - lineTop;
    if (m_pendingStrut != LayoutUnit::min()) {
        // The previous break was before a breakable block. Here's the first line after / inside
        // that block. We want to record the distance from the top of the column to the bottom of
        // this box as space shortage.
        LayoutUnit logicalOffsetFromCurrentColumn = lineTopInFlowThread - group().columnLogicalTopForOffset(lineTopInFlowThread);
        recordSpaceShortage(logicalOffsetFromCurrentColumn + lineHeight - m_pendingStrut);
        m_pendingStrut = LayoutUnit::min();
        return;
    }
    ASSERT(isFirstAfterBreak(lineTopInFlowThread) || !line.paginationStrut() || !isLogicalTopWithinBounds(lineTopInFlowThread - line.paginationStrut()));
    if (isFirstAfterBreak(lineTopInFlowThread))
        recordSpaceShortage(lineHeight - line.paginationStrut());
}

} // namespace blink
