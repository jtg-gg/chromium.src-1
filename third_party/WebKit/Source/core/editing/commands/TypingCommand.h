/*
 * Copyright (C) 2005, 2006, 2007, 2008 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef TypingCommand_h
#define TypingCommand_h

#include "core/editing/commands/TextInsertionBaseCommand.h"

namespace blink {

class TypingCommand final : public TextInsertionBaseCommand {
public:
    enum ETypingCommand {
        DeleteSelection,
        DeleteKey,
        ForwardDeleteKey,
        InsertText,
        InsertLineBreak,
        InsertParagraphSeparator,
        InsertParagraphSeparatorInQuotedContent
    };

    enum TextCompositionType {
        TextCompositionNone,
        TextCompositionUpdate,
        TextCompositionConfirm
    };

    enum Option {
        SelectInsertedText = 1 << 0,
        KillRing = 1 << 1,
        RetainAutocorrectionIndicator = 1 << 2,
        PreventSpellChecking = 1 << 3,
        SmartDelete = 1 << 4
    };
    typedef unsigned Options;

    static void deleteSelection(Document&, Options = 0);
    static void deleteKeyPressed(Document&, Options, TextGranularity = CharacterGranularity);
    static void forwardDeleteKeyPressed(Document&, EditingState*, Options = 0, TextGranularity = CharacterGranularity);
    static void insertText(Document&, const String&, Options, TextCompositionType = TextCompositionNone);
    static void insertText(Document&, const String&, const VisibleSelection&, Options, TextCompositionType = TextCompositionNone);
    static bool insertLineBreak(Document&);
    static bool insertParagraphSeparator(Document&);
    static bool insertParagraphSeparatorInQuotedContent(Document&);
    static void closeTyping(LocalFrame*);

    void insertText(const String &text, bool selectInsertedText, EditingState*);
    void insertTextRunWithoutNewlines(const String &text, bool selectInsertedText, EditingState*);
    void insertLineBreak(EditingState*);
    void insertParagraphSeparatorInQuotedContent(EditingState*);
    void insertParagraphSeparator(EditingState*);
    void deleteKeyPressed(TextGranularity, bool killRing, EditingState*);
    void forwardDeleteKeyPressed(TextGranularity, bool killRing, EditingState*);
    void deleteSelection(bool smartDelete, EditingState*);
    void setCompositionType(TextCompositionType type) { m_compositionType = type; }

private:
    static PassRefPtrWillBeRawPtr<TypingCommand> create(Document& document, ETypingCommand command, const String& text = "", Options options = 0, TextGranularity granularity = CharacterGranularity)
    {
        return adoptRefWillBeNoop(new TypingCommand(document, command, text, options, granularity, TextCompositionNone));
    }

    static PassRefPtrWillBeRawPtr<TypingCommand> create(Document& document, ETypingCommand command, const String& text, Options options, TextCompositionType compositionType)
    {
        return adoptRefWillBeNoop(new TypingCommand(document, command, text, options, CharacterGranularity, compositionType));
    }

    TypingCommand(Document&, ETypingCommand, const String& text, Options, TextGranularity, TextCompositionType);

    void setSmartDelete(bool smartDelete) { m_smartDelete = smartDelete; }
    bool isOpenForMoreTyping() const { return m_openForMoreTyping; }
    void closeTyping() { m_openForMoreTyping = false; }

    static PassRefPtrWillBeRawPtr<TypingCommand> lastTypingCommandIfStillOpenForTyping(LocalFrame*);

    void doApply(EditingState*) override;
    EditAction editingAction() const override;
    bool isTypingCommand() const override;
    bool preservesTypingStyle() const override { return m_preservesTypingStyle; }
    void setShouldRetainAutocorrectionIndicator(bool retain) override { m_shouldRetainAutocorrectionIndicator = retain; }
    bool shouldStopCaretBlinking() const override { return true; }
    void setShouldPreventSpellChecking(bool prevent) { m_shouldPreventSpellChecking = prevent; }

    static void updateSelectionIfDifferentFromCurrentSelection(TypingCommand*, LocalFrame*);

    void updatePreservesTypingStyle(ETypingCommand);
    void markMisspellingsAfterTyping(ETypingCommand);
    void typingAddedToOpenCommand(ETypingCommand);
    bool makeEditableRootEmpty(EditingState*);

    void updateCommandTypeOfOpenCommand(ETypingCommand typingCommand) { m_commandType = typingCommand; }
    ETypingCommand commandTypeOfOpenCommand() const { return m_commandType; }

    ETypingCommand m_commandType;
    String m_textToInsert;
    bool m_openForMoreTyping;
    bool m_selectInsertedText;
    bool m_smartDelete;
    TextGranularity m_granularity;
    TextCompositionType m_compositionType;
    bool m_killRing;
    bool m_preservesTypingStyle;

    // Undoing a series of backward deletes will restore a selection around all of the
    // characters that were deleted, but only if the typing command being undone
    // was opened with a backward delete.
    bool m_openedByBackwardDelete;

    bool m_shouldRetainAutocorrectionIndicator;
    bool m_shouldPreventSpellChecking;
};

} // namespace blink

#endif // TypingCommand_h
