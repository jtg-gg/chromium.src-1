/*
 * Copyright (C) 2013 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @constructor
 * @param {number} startLine
 * @param {number} startColumn
 * @param {number} endLine
 * @param {number} endColumn
 */
WebInspector.TextRange = function(startLine, startColumn, endLine, endColumn)
{
    this.startLine = startLine;
    this.startColumn = startColumn;
    this.endLine = endLine;
    this.endColumn = endColumn;
}

/**
 * @param {number} line
 * @param {number} column
 * @return {!WebInspector.TextRange}
 */
WebInspector.TextRange.createFromLocation = function(line, column)
{
    return new WebInspector.TextRange(line, column, line, column);
}

/**
 * @param {!Object} serializedTextRange
 * @return {!WebInspector.TextRange}
 */
WebInspector.TextRange.fromObject = function(serializedTextRange)
{
    return new WebInspector.TextRange(serializedTextRange.startLine, serializedTextRange.startColumn, serializedTextRange.endLine, serializedTextRange.endColumn);
}

/**
 * @param {!WebInspector.TextRange} range1
 * @param {!WebInspector.TextRange} range2
 * @return {number}
 */
WebInspector.TextRange.comparator = function(range1, range2)
{
    return range1.compareTo(range2);
}

WebInspector.TextRange.prototype = {
    /**
     * @return {boolean}
     */
    isEmpty: function()
    {
        return this.startLine === this.endLine && this.startColumn === this.endColumn;
    },

    /**
     * @param {!WebInspector.TextRange} range
     * @return {boolean}
     */
    immediatelyPrecedes: function(range)
    {
        if (!range)
            return false;
        return this.endLine === range.startLine && this.endColumn === range.startColumn;
    },

    /**
     * @param {!WebInspector.TextRange} range
     * @return {boolean}
     */
    immediatelyFollows: function(range)
    {
        if (!range)
            return false;
        return range.immediatelyPrecedes(this);
    },

    /**
     * @param {!WebInspector.TextRange} range
     * @return {boolean}
     */
    follows: function(range)
    {
        return (range.endLine === this.startLine && range.endColumn <= this.startColumn)
            || range.endLine < this.startLine;
    },

    /**
     * @return {number}
     */
    get linesCount()
    {
        return this.endLine - this.startLine;
    },

    /**
     * @return {!WebInspector.TextRange}
     */
    collapseToEnd: function()
    {
        return new WebInspector.TextRange(this.endLine, this.endColumn, this.endLine, this.endColumn);
    },

    /**
     * @return {!WebInspector.TextRange}
     */
    collapseToStart: function()
    {
        return new WebInspector.TextRange(this.startLine, this.startColumn, this.startLine, this.startColumn);
    },

    /**
     * @return {!WebInspector.TextRange}
     */
    normalize: function()
    {
        if (this.startLine > this.endLine || (this.startLine === this.endLine && this.startColumn > this.endColumn))
            return new WebInspector.TextRange(this.endLine, this.endColumn, this.startLine, this.startColumn);
        else
            return this.clone();
    },

    /**
     * @return {!WebInspector.TextRange}
     */
    clone: function()
    {
        return new WebInspector.TextRange(this.startLine, this.startColumn, this.endLine, this.endColumn);
    },

    /**
     * @return {!{startLine: number, startColumn: number, endLine: number, endColumn: number}}
     */
    serializeToObject: function()
    {
        var serializedTextRange = {};
        serializedTextRange.startLine = this.startLine;
        serializedTextRange.startColumn = this.startColumn;
        serializedTextRange.endLine = this.endLine;
        serializedTextRange.endColumn = this.endColumn;
        return serializedTextRange;
    },

    /**
     * @param {!WebInspector.TextRange} other
     * @return {number}
     */
    compareTo: function(other)
    {
        if (this.startLine > other.startLine)
            return 1;
        if (this.startLine < other.startLine)
            return -1;
        if (this.startColumn > other.startColumn)
            return 1;
        if (this.startColumn < other.startColumn)
            return -1;
        return 0;
    },

    /**
     * @param {!WebInspector.TextRange} other
     * @return {boolean}
     */
    equal: function(other)
    {
        return this.startLine === other.startLine && this.endLine === other.endLine &&
            this.startColumn === other.startColumn && this.endColumn === other.endColumn;
    },

    /**
     * @param {number} lineOffset
     * @return {!WebInspector.TextRange}
     */
    shift: function(lineOffset)
    {
        return new WebInspector.TextRange(this.startLine + lineOffset, this.startColumn, this.endLine + lineOffset, this.endColumn);
    },

    /**
     * @param {number} line
     * @param {number} column
     * @return {!WebInspector.TextRange}
     */
    relativeTo: function(line, column)
    {
        var relative = this.clone();

        if (this.startLine == line)
            relative.startColumn -= column;
        if (this.endLine == line)
            relative.endColumn -= column;

        relative.startLine -= line;
        relative.endLine -= line;
        return relative;
    },

    /**
     * @param {string} text
     * @return {!WebInspector.SourceRange}
     */
    toSourceRange: function(text)
    {
        var start = (this.startLine ? text.lineEndings()[this.startLine - 1] + 1 : 0) + this.startColumn;
        var end = (this.endLine ? text.lineEndings()[this.endLine - 1] + 1 : 0) + this.endColumn;
        return new WebInspector.SourceRange(start, end - start);
    },

    /**
     * @param {!WebInspector.TextRange} originalRange
     * @param {!WebInspector.TextRange} editedRange
     * @return {!WebInspector.TextRange}
     */
    rebaseAfterTextEdit: function(originalRange, editedRange)
    {
        console.assert(originalRange.startLine === editedRange.startLine);
        console.assert(originalRange.startColumn === editedRange.startColumn);
        var rebase = this.clone();
        if (!this.follows(originalRange))
            return rebase;
        var lineDelta = editedRange.endLine - originalRange.endLine;
        var columnDelta = editedRange.endColumn - originalRange.endColumn;
        rebase.startLine += lineDelta;
        rebase.endLine += lineDelta;
        if (rebase.startLine === editedRange.endLine)
            rebase.startColumn += columnDelta;
        if (rebase.endLine === editedRange.endLine)
            rebase.endColumn += columnDelta;
        return rebase;
    },

    /**
     * @override
     * @return {string}
     */
    toString: function()
    {
        return JSON.stringify(this);
    },

    /**
     * @param {string} text
     * @param {string} replacement
     * @return {string}
     */
    replaceInText: function(text, replacement)
    {
        var sourceRange = this.toSourceRange(text);
        return text.substring(0, sourceRange.offset) + replacement + text.substring(sourceRange.offset + sourceRange.length);
    },

    /**
     * @param {string} text
     * @return {string}
     */
    extract: function(text)
    {
        var sourceRange = this.toSourceRange(text);
        return text.substr(sourceRange.offset, sourceRange.length);
    },

    /**
     * @param {number} lineNumber
     * @param {number} columnNumber
     * @return {boolean}
     */
    containsLocation: function(lineNumber, columnNumber)
    {
        if (this.startLine === this.endLine)
            return this.startLine === lineNumber && this.startColumn <= columnNumber && columnNumber <= this.endColumn;
        if (this.startLine === lineNumber)
            return this.startColumn <= columnNumber;
        if (this.endLine === lineNumber)
            return columnNumber <= this.endColumn;
        return this.startLine < lineNumber && lineNumber < this.endLine;
    }
}

/**
 * @constructor
 * @param {number} offset
 * @param {number} length
 */
WebInspector.SourceRange = function(offset, length)
{
    this.offset = offset;
    this.length = length;
}

WebInspector.SourceRange.prototype = {
    /**
     * @param {string} text
     * @return {!WebInspector.TextRange}
     */
    toTextRange: function(text)
    {
        var p1 = fromOffset(text, this.offset);
        var p2 = fromOffset(text, this.offset + this.length);
        return new WebInspector.TextRange(p1.lineNumber, p1.columnNumber, p2.lineNumber, p2.columnNumber);

        /**
         * @param {string} text
         * @param {number} offset
         * @return {!{lineNumber: number, columnNumber: number}}
         */
        function fromOffset(text, offset)
        {
            var lineEndings = text.lineEndings();
            var lineNumber = lineEndings.lowerBound(offset);
            var columnNumber = lineNumber === 0 ? offset : offset - lineEndings[lineNumber - 1] - 1;
            return {lineNumber: lineNumber, columnNumber: columnNumber};
        }
    }
}

/**
 * @constructor
 * @param {string} sourceURL
 * @param {!WebInspector.TextRange} oldRange
 * @param {string} newText
 */
WebInspector.SourceEdit = function(sourceURL, oldRange, newText)
{
    this.sourceURL = sourceURL;
    this.oldRange = oldRange;
    this.newText = newText;
}

WebInspector.SourceEdit.prototype = {
    /**
     * @return {!WebInspector.TextRange}
     */
    newRange: function()
    {
        var endLine = this.oldRange.startLine;
        var endColumn = this.oldRange.startColumn + this.newText.length;
        var lineEndings = this.newText.lineEndings();
        if (lineEndings.length > 1) {
            endLine = this.oldRange.startLine + lineEndings.length - 1;
            var len = lineEndings.length;
            endColumn = lineEndings[len - 1] - lineEndings[len - 2] - 1;
        }
        return new WebInspector.TextRange(
            this.oldRange.startLine,
            this.oldRange.startColumn,
            endLine,
            endColumn);
    },

    /**
     * @param {string} text
     * @return {string}
     */
    applyToText: function(text)
    {
        return this.oldRange.replaceInText(text, this.newText);
    },
}
