#include "TextEditor.h"

#include <algorithm>  // for max
#include <cstring>    // for strcmp, size_t
#include <memory>     // for allocator, make_unique, __shared_p...
#include <utility>    // for move

#include <gdk/gdkkeysyms.h>  // for GDK_KEY_B, GDK_KEY_ISO_Enter, GDK_...
#include <glib-object.h>     // for g_object_get, g_object_unref, G_CA...

#include "control/Control.h"       // for Control
#include "model/Font.h"            // for XojFont
#include "model/PageRef.h"         // for PageRef
#include "model/Text.h"            // for Text
#include "model/XojPage.h"         // for XojPage
#include "undo/ColorUndoAction.h"  // for ColorUndoAction
#include "undo/TextUndoAction.h"   // for TextUndoAction
#include "undo/UndoRedoHandler.h"  // for UndoRedoHandler
#include "util/Rectangle.h"        // for Rectangle
#include "view/TextView.h"         // for TextView

#include "PageView.h"          // for XojPageView
#include "TextEditorWidget.h"  // for gtk_xoj_int_txt_new
#include "XournalView.h"       // for XournalView
#include "XournalppCursor.h"   // for XournalppCursor

class UndoAction;

using std::string;

TextEditor::TextEditor(XojPageView* gui, GtkWidget* widget, Text* text, bool ownText):
        gui(gui), widget(widget), text(text), ownText(ownText) {
    this->text->setInEditing(true);
    this->textWidget = gtk_xoj_int_txt_new(this);
    this->lastText = text->getText();

    this->previousBoundingBox = text->boundingRect();

    this->buffer = gtk_text_buffer_new(nullptr);
    string txt = this->text->getText();
    gtk_text_buffer_set_text(this->buffer, txt.c_str(), -1);

    g_signal_connect(this->buffer, "paste-done", G_CALLBACK(bufferPasteDoneCallback), this);

    GtkTextIter first = {nullptr};
    gtk_text_buffer_get_iter_at_offset(this->buffer, &first, 0);
    gtk_text_buffer_place_cursor(this->buffer, &first);

    GtkSettings* settings = gtk_widget_get_settings(this->widget);
    g_object_get(settings, "gtk-cursor-blink", &this->cursorBlink, nullptr);
    g_object_get(settings, "gtk-cursor-blink-time", &this->cursorBlinkTime, nullptr);
    g_object_get(settings, "gtk-cursor-blink-timeout", &this->cursorBlinkTimeout, nullptr);

    this->imContext = gtk_im_multicontext_new();
    gtk_im_context_set_client_window(this->imContext, gtk_widget_get_parent_window(this->widget));
    gtk_im_context_focus_in(this->imContext);

    g_signal_connect(this->imContext, "commit", G_CALLBACK(iMCommitCallback), this);
    g_signal_connect(this->imContext, "preedit-changed", G_CALLBACK(iMPreeditChangedCallback), this);
    g_signal_connect(this->imContext, "retrieve-surrounding", G_CALLBACK(iMRetrieveSurroundingCallback), this);
    g_signal_connect(this->imContext, "delete-surrounding", G_CALLBACK(imDeleteSurroundingCallback), this);

    if (this->cursorBlink) {
        blinkCallback(this);
    } else {
        this->cursorVisible = true;
    }
}

TextEditor::~TextEditor() {
    this->text->setInEditing(false);
    this->widget = nullptr;

    Control* control = gui->getXournal()->getControl();
    control->setCopyPasteEnabled(false);

    this->contentsChanged(true);

    if (this->ownText) {
        UndoRedoHandler* handler = gui->getXournal()->getControl()->getUndoRedoHandler();
        for (TextUndoAction& undo: this->undoActions) { handler->removeUndoAction(&undo); }
    } else {
        for (TextUndoAction& undo: this->undoActions) { undo.textEditFinished(); }
    }
    this->undoActions.clear();

    if (this->ownText) {
        delete this->text;
        this->text = nullptr;
    }

    g_object_unref(this->buffer);
    gtk_widget_destroy(this->textWidget);

    if (this->blinkTimeout) {
        g_source_remove(this->blinkTimeout);
    }

    g_object_unref(this->imContext);

    this->text = nullptr;

    if (this->layout) {
        g_object_unref(this->layout);
        this->layout = nullptr;
    }

    if (this->preeditAttrList) {
        pango_attr_list_unref(this->preeditAttrList);
    }
}

auto TextEditor::getText() -> Text* {
    GtkTextIter start, end;

    gtk_text_buffer_get_bounds(this->buffer, &start, &end);
    char* text = gtk_text_iter_get_text(&start, &end);
    this->text->setText(text);
    g_free(text);

    return this->text;
}

void TextEditor::setText(const string& text) {
    gtk_text_buffer_set_text(this->buffer, text.c_str(), -1);

    GtkTextIter first = {nullptr};
    gtk_text_buffer_get_iter_at_offset(this->buffer, &first, 0);
    gtk_text_buffer_place_cursor(this->buffer, &first);
}

auto TextEditor::setColor(Color color) -> UndoAction* {
    auto origColor = this->text->getColor();
    this->text->setColor(color);

    repaintEditor();

    // This is a new text, so we don't need to create a undo action
    if (this->ownText) {
        return nullptr;
    }

    auto* undo = new ColorUndoAction(gui->getPage(), gui->getPage()->getSelectedLayer());
    undo->addStroke(this->text, origColor, color);

    return undo;
}

void TextEditor::setFont(XojFont font) {
    this->text->setFont(font);
    xoj::view::TextView::updatePangoFont(this->layout, this->text);
    this->repaintEditor();
}

void TextEditor::textCopyed() { this->ownText = false; }

void TextEditor::iMCommitCallback(GtkIMContext* context, const gchar* str, TextEditor* te) {
    gtk_text_buffer_begin_user_action(te->buffer);
    gboolean had_selection = gtk_text_buffer_get_selection_bounds(te->buffer, nullptr, nullptr);

    gtk_text_buffer_delete_selection(te->buffer, true, true);

    if (!strcmp(str, "\n")) {
        if (!gtk_text_buffer_insert_interactive_at_cursor(te->buffer, "\n", 1, true)) {
            gtk_widget_error_bell(te->widget);
        } else {
            te->contentsChanged(true);
        }
    } else {
        if (!had_selection && te->cursorOverwrite) {
            GtkTextIter insert;

            gtk_text_buffer_get_iter_at_mark(te->buffer, &insert, gtk_text_buffer_get_insert(te->buffer));
            if (!gtk_text_iter_ends_line(&insert)) {
                te->deleteFromCursor(GTK_DELETE_CHARS, 1);
            }
        }

        if (!gtk_text_buffer_insert_interactive_at_cursor(te->buffer, str, -1, true)) {
            gtk_widget_error_bell(te->widget);
        }
    }

    gtk_text_buffer_end_user_action(te->buffer);
    te->contentsChanged();
    te->repaintEditor();
}

void TextEditor::iMPreeditChangedCallback(GtkIMContext* context, TextEditor* te) {
    gchar* str = nullptr;
    PangoAttrList* attrs = nullptr;
    gint cursor_pos = 0;
    GtkTextIter iter;

    gtk_text_buffer_get_iter_at_mark(te->buffer, &iter, gtk_text_buffer_get_insert(te->buffer));

    /* Keypress events are passed to input method even if cursor position is
     * not editable; so beep here if it's multi-key input sequence, input
     * method will be reset in key-press-event handler.
     */
    gtk_im_context_get_preedit_string(context, &str, &attrs, &cursor_pos);

    if (attrs == nullptr) {
        attrs = pango_attr_list_new();
    }
    if (te->preeditAttrList) {
        pango_attr_list_unref(te->preeditAttrList);
    }
    te->preeditAttrList = attrs;
    attrs = nullptr;

    if (str && str[0] && !gtk_text_iter_can_insert(&iter, true)) {
        gtk_widget_error_bell(te->widget);
        goto out;
    }

    if (str != nullptr) {
        te->preeditString = str;
    } else {
        te->preeditString = "";
    }
    te->preeditCursor = cursor_pos;
    te->contentsChanged();
    te->repaintEditor();

out:

    g_free(str);
}

auto TextEditor::iMRetrieveSurroundingCallback(GtkIMContext* context, TextEditor* te) -> bool {
    GtkTextIter start;
    GtkTextIter end;

    gtk_text_buffer_get_iter_at_mark(te->buffer, &start, gtk_text_buffer_get_insert(te->buffer));
    end = start;

    gint pos = gtk_text_iter_get_line_index(&start);
    gtk_text_iter_set_line_offset(&start, 0);
    gtk_text_iter_forward_to_line_end(&end);

    gchar* text = gtk_text_iter_get_slice(&start, &end);
    gtk_im_context_set_surrounding(context, text, -1, pos);
    g_free(text);

    te->contentsChanged();
    te->repaintEditor();
    return true;
}

auto TextEditor::imDeleteSurroundingCallback(GtkIMContext* context, gint offset, gint n_chars, TextEditor* te) -> bool {
    GtkTextIter start;
    GtkTextIter end;

    gtk_text_buffer_get_iter_at_mark(te->buffer, &start, gtk_text_buffer_get_insert(te->buffer));
    end = start;

    gtk_text_iter_forward_chars(&start, offset);
    gtk_text_iter_forward_chars(&end, offset + n_chars);

    gtk_text_buffer_delete_interactive(te->buffer, &start, &end, true);

    te->contentsChanged();
    te->repaintEditor();

    return true;
}

auto TextEditor::onKeyPressEvent(GdkEventKey* event) -> bool {
    bool retval = false;
    bool obscure = false;

    GtkTextIter iter;
    GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
    GtkTextMark* insert = gtk_text_buffer_get_insert(this->buffer);
    gtk_text_buffer_get_iter_at_mark(this->buffer, &iter, insert);
    bool canInsert = gtk_text_iter_can_insert(&iter, true);

    // IME needs to handle the input first so the candidate window works correctly
    if (gtk_im_context_filter_keypress(this->imContext, event)) {
        this->needImReset = true;
        if (!canInsert) {
            this->resetImContext();
        }
        obscure = canInsert;
        retval = true;
    } else if (gtk_bindings_activate_event(G_OBJECT(this->textWidget), event)) {
        return true;
    } else if ((event->state & modifiers) == GDK_CONTROL_MASK) {
        // Bold text
        if (event->keyval == GDK_KEY_b || event->keyval == GDK_KEY_B) {
            toggleBold();
            return true;
        }
        // Increase text size
        if (event->keyval == GDK_KEY_plus) {
            incSize();
            return true;
        }
        // Decrease text size
        if (event->keyval == GDK_KEY_minus) {
            decSize();
            return true;
        }
    } else if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_ISO_Enter ||
               event->keyval == GDK_KEY_KP_Enter) {
        this->resetImContext();
        iMCommitCallback(nullptr, "\n", this);

        obscure = true;
        retval = true;
    }
    // Pass through Tab as literal tab, unless Control is held down
    else if ((event->keyval == GDK_KEY_Tab || event->keyval == GDK_KEY_KP_Tab ||
              event->keyval == GDK_KEY_ISO_Left_Tab) &&
             !(event->state & GDK_CONTROL_MASK)) {
        resetImContext();
        iMCommitCallback(nullptr, "\t", this);
        obscure = true;
        retval = true;
    } else {
        retval = false;
    }

    if (obscure) {
        XournalppCursor* cursor = gui->getXournal()->getCursor();
        cursor->setInvisible(true);
    }

    return retval;
}

auto TextEditor::onKeyReleaseEvent(GdkEventKey* event) -> bool {
    GtkTextIter iter;

    GtkTextMark* insert = gtk_text_buffer_get_insert(this->buffer);
    gtk_text_buffer_get_iter_at_mark(this->buffer, &iter, insert);
    if (gtk_text_iter_can_insert(&iter, true) && gtk_im_context_filter_keypress(this->imContext, event)) {
        this->needImReset = true;
        return true;
    }
    return false;
}

void TextEditor::toggleOverwrite() {
    this->cursorOverwrite = !this->cursorOverwrite;
    repaintCursor();
}

/**
 * I know it's a bit rough and duplicated
 * Improve that later on...
 */
void TextEditor::decSize() {
    XojFont& font = text->getFont();
    double fontSize = font.getSize();
    fontSize--;
    font.setSize(fontSize);
    setFont(font);
}

void TextEditor::incSize() {
    XojFont& font = text->getFont();
    double fontSize = font.getSize();
    fontSize++;
    font.setSize(fontSize);
    setFont(font);
}

void TextEditor::toggleBold() {
    // get the current/used font
    XojFont& font = text->getFont();
    string fontName = font.getName();

    std::size_t found = fontName.find("Bold");

    // toggle bold
    if (found == string::npos) {
        fontName = fontName + " Bold";
    } else {
        fontName = fontName.substr(0, found - 1);
    }

    // commit changes
    font.setName(fontName);
    setFont(font);

    // this->repaintEditor();
}

void TextEditor::selectAtCursor(TextEditor::SelectType ty) {
    GtkTextMark* mark = gtk_text_buffer_get_insert(this->buffer);
    GtkTextIter startPos;
    GtkTextIter endPos;
    gtk_text_buffer_get_selection_bounds(this->buffer, &startPos, &endPos);
    const auto searchFlag = GTK_TEXT_SEARCH_TEXT_ONLY;  // To be used to find double newlines

    switch (ty) {
        case TextEditor::SelectType::word:
            // Do nothing if cursor is over whitespace
            GtkTextIter currentPos;
            gtk_text_buffer_get_iter_at_mark(this->buffer, &currentPos, mark);
            if (!gtk_text_iter_inside_word(&currentPos)) {
                return;
            }

            if (!gtk_text_iter_starts_word(&currentPos)) {
                gtk_text_iter_backward_word_start(&startPos);
            }
            if (!gtk_text_iter_ends_word(&currentPos)) {
                gtk_text_iter_forward_word_end(&endPos);
            }
            break;
        case TextEditor::SelectType::paragraph:
            // Note that a GTK "paragraph" is a line, so there's no nice one-liner.
            // We define a paragraph as text separated by double newlines.
            while (!gtk_text_iter_is_start(&startPos)) {
                // There's no GTK function to go to line start, so do it manually.
                while (!gtk_text_iter_starts_line(&startPos)) {
                    if (!gtk_text_iter_backward_word_start(&startPos)) {
                        break;
                    }
                }
                // Check for paragraph start
                GtkTextIter searchPos = startPos;
                gtk_text_iter_backward_chars(&searchPos, 2);
                if (gtk_text_iter_backward_search(&startPos, "\n\n", searchFlag, nullptr, nullptr, &searchPos)) {
                    break;
                }
                gtk_text_iter_backward_line(&startPos);
            }
            while (!gtk_text_iter_ends_line(&endPos)) {
                gtk_text_iter_forward_to_line_end(&endPos);
                // Check for paragraph end
                GtkTextIter searchPos = endPos;
                gtk_text_iter_forward_chars(&searchPos, 2);
                if (gtk_text_iter_forward_search(&endPos, "\n\n", searchFlag, nullptr, nullptr, &searchPos)) {
                    break;
                }
                gtk_text_iter_forward_line(&endPos);
            }
            break;
        case TextEditor::SelectType::all:
            gtk_text_buffer_get_bounds(this->buffer, &startPos, &endPos);
            break;
    }

    gtk_text_buffer_select_range(this->buffer, &startPos, &endPos);

    this->repaintEditor();
}

void TextEditor::moveCursor(GtkMovementStep step, int count, bool extendSelection) {
    resetImContext();

    // Not possible, but we have to handle the events, else the page gets scrolled
    //	if (step == GTK_MOVEMENT_PAGES) {
    //		if (!gtk_text_view_scroll_pages(text_view, count, extend_selection))
    //			gtk_widget_error_bell(GTK_WIDGET (text_view));
    //
    //		gtk_text_view_check_cursor_blink(text_view);
    //		gtk_text_view_pend_cursor_blink(text_view);
    //		return;
    //	} else if (step == GTK_MOVEMENT_HORIZONTAL_PAGES) {
    //		if (!gtk_text_view_scroll_hpages(text_view, count, extend_selection))
    //			gtk_widget_error_bell(GTK_WIDGET (text_view));
    //
    //		gtk_text_view_check_cursor_blink(text_view);
    //		gtk_text_view_pend_cursor_blink(text_view);
    //		return;
    //	}

    GtkTextIter insert;
    gtk_text_buffer_get_iter_at_mark(this->buffer, &insert, gtk_text_buffer_get_insert(this->buffer));
    GtkTextIter newplace = insert;

    bool updateVirtualCursor = true;

    switch (step) {
        case GTK_MOVEMENT_LOGICAL_POSITIONS:  // not used!?
            gtk_text_iter_forward_visible_cursor_positions(&newplace, count);
            break;
        case GTK_MOVEMENT_VISUAL_POSITIONS:
            if (count < 0) {
                gtk_text_iter_backward_cursor_position(&newplace);
            } else {
                gtk_text_iter_forward_cursor_position(&newplace);
            }
            break;

        case GTK_MOVEMENT_WORDS:
            if (count < 0) {
                gtk_text_iter_backward_visible_word_starts(&newplace, -count);
            } else if (count > 0) {
                if (!gtk_text_iter_forward_visible_word_ends(&newplace, count)) {
                    gtk_text_iter_forward_to_line_end(&newplace);
                }
            }
            break;

        case GTK_MOVEMENT_DISPLAY_LINES:
            updateVirtualCursor = false;
            jumpALine(&newplace, count);
            break;

        case GTK_MOVEMENT_PARAGRAPHS:
            if (count > 0) {
                if (!gtk_text_iter_ends_line(&newplace)) {
                    gtk_text_iter_forward_to_line_end(&newplace);
                    --count;
                }
                gtk_text_iter_forward_visible_lines(&newplace, count);
                gtk_text_iter_forward_to_line_end(&newplace);
            } else if (count < 0) {
                if (gtk_text_iter_get_line_offset(&newplace) > 0) {
                    gtk_text_iter_set_line_offset(&newplace, 0);
                }
                gtk_text_iter_forward_visible_lines(&newplace, count);
                gtk_text_iter_set_line_offset(&newplace, 0);
            }
            break;

        case GTK_MOVEMENT_DISPLAY_LINE_ENDS:
        case GTK_MOVEMENT_PARAGRAPH_ENDS:
            if (count > 0) {
                if (!gtk_text_iter_ends_line(&newplace)) {
                    gtk_text_iter_forward_to_line_end(&newplace);
                }
            } else if (count < 0) {
                gtk_text_iter_set_line_offset(&newplace, 0);
            }
            break;

        case GTK_MOVEMENT_BUFFER_ENDS:
            if (count > 0) {
                gtk_text_buffer_get_end_iter(this->buffer, &newplace);
            } else if (count < 0) {
                gtk_text_buffer_get_iter_at_offset(this->buffer, &newplace, 0);
            }
            break;

        default:
            break;
    }

    // call moveCursor() even if the cursor hasn't moved, since it cancels the selection
    moveCursor(&newplace, extendSelection);

    if (updateVirtualCursor) {
        calcVirtualCursor();
    }

    if (gtk_text_iter_equal(&insert, &newplace)) {
        gtk_widget_error_bell(this->widget);
    }

    if (this->cursorBlink) {
        this->cursorVisible = false;
        if (this->blinkTimeout) {
            g_source_remove(this->blinkTimeout);
        }
        blinkCallback(this);
    } else {
        repaintCursor();
    }
}

void TextEditor::findPos(GtkTextIter* iter, double xPos, double yPos) {
    if (!this->layout) {
        return;
    }

    int index = 0;
    if (!pango_layout_xy_to_index(this->layout, xPos * PANGO_SCALE, yPos * PANGO_SCALE, &index, nullptr)) {
        index++;
    }

    gtk_text_iter_set_offset(iter, getCharOffset(index));
}

void TextEditor::contentsChanged(bool forceCreateUndoAction) {
    string currentText = getText()->getText();

    // I know it's a little bit bulky, but ABS on subtracted size_t is a little bit unsafe
    if (forceCreateUndoAction ||
        ((lastText.length() >= currentText.length()) ? (lastText.length() - currentText.length()) :
                                                       (currentText.length() - lastText.length())) > 100) {
        if (!lastText.empty() && !this->undoActions.empty() &&
            this->undoActions.front().get().getUndoText() != currentText) {
            auto undo = std::make_unique<TextUndoAction>(gui->getPage(), gui->getPage()->getSelectedLayer(), this->text,
                                                         lastText, this);
            UndoRedoHandler* handler = gui->getXournal()->getControl()->getUndoRedoHandler();
            this->undoActions.emplace_back(std::ref(*undo));
            handler->addUndoAction(std::move(undo));
        }
        lastText = currentText;
    }
}

auto TextEditor::getFirstUndoAction() -> UndoAction* {
    if (!this->undoActions.empty()) {
        return &this->undoActions.front().get();
    }
    return nullptr;
}

void TextEditor::markPos(double x, double y, bool extendSelection) {
    if (this->layout == nullptr) {
        this->markPosX = x;
        this->markPosY = y;
        this->markPosExtendSelection = extendSelection;
        this->markPosQueue = true;
        return;
    }
    GtkTextIter iter;
    GtkTextMark* insert = gtk_text_buffer_get_insert(this->buffer);
    gtk_text_buffer_get_iter_at_mark(this->buffer, &iter, insert);
    GtkTextIter newplace = iter;

    findPos(&newplace, x, y);

    // Noting changed
    if (gtk_text_iter_equal(&newplace, &iter)) {
        return;
    }
    moveCursor(&newplace, extendSelection);
    calcVirtualCursor();
    repaintCursor();
}

void TextEditor::mousePressed(double x, double y, XojPageView* page) {
    xoj::util::Rectangle<double> matchRect(x, y, 1, 1);
    
    if (matchRect.intersects(this->previousBoundingBox)) {
        this->mouseDown = true;
        markPos(x - this->text->getX(), y - this->text->getY(), false);
    } else if (matchRect.intersects(this->getPreviousBoundingBoxWithPadding())) {
        this->text->setFixedWidth(true);
        this->resizing = true;
        
        //Update wrap width in the pango layout
        pango_layout_set_width(layout, static_cast<int>(this->text->getElementWidth() * PANGO_SCALE));
    } else {
        page->endText();
    };

}

void TextEditor::mouseMoved(double x, double y, XojPageView* page) {
    const double dX = x - this->text->getX();
    const double dY = y - this->text->getY();

    if (this->mouseDown) {
        markPos(dX, dY, true);
    } else if (this->resizing) {
        const double new_width = x - this->text->getX();
        this->text->setWidth(std::max(new_width, TextEditor::MIN_TEXT_WIDTH*this->text->getFontSize()));
        // Update wrap width in the pango layout
        pango_layout_set_width(layout, static_cast<int>(this->text->getElementWidth() * PANGO_SCALE));
        this->repaintEditor();
    }
}

void TextEditor::mouseReleased() { this->mouseDown = false; this->resizing = false; }

void TextEditor::jumpALine(GtkTextIter* textIter, int count) {
    int cursorLine = gtk_text_iter_get_line(textIter);

    if (cursorLine + count < 0) {
        return;
    }

    PangoLayoutLine* line = pango_layout_get_line(this->layout, cursorLine + count);
    if (line == nullptr) {
        return;
    }

    int index = 0;
    pango_layout_line_x_to_index(line, this->virtualCursor * PANGO_SCALE, &index, nullptr);

    index = getCharOffset(index);

    gtk_text_iter_set_offset(textIter, index);
}

void TextEditor::calcVirtualCursor() {
    this->virtualCursor = 0;
    GtkTextIter cursorIter = {nullptr};
    GtkTextMark* cursor = gtk_text_buffer_get_insert(this->buffer);
    gtk_text_buffer_get_iter_at_mark(this->buffer, &cursorIter, cursor);

    int offset = getByteOffset(gtk_text_iter_get_offset(&cursorIter));

    PangoRectangle rect = {0};
    pango_layout_index_to_pos(this->layout, offset, &rect);
    this->virtualCursor = (static_cast<double>(rect.x)) / PANGO_SCALE;
}

void TextEditor::moveCursor(const GtkTextIter* newLocation, gboolean extendSelection) {
    Control* control = gui->getXournal()->getControl();

    if (extendSelection) {
        gtk_text_buffer_move_mark_by_name(this->buffer, "insert", newLocation);
        control->setCopyPasteEnabled(true);
    } else {
        gtk_text_buffer_place_cursor(this->buffer, newLocation);
        control->setCopyPasteEnabled(false);
    }

    this->repaintEditor();
}

static auto whitespace(gunichar ch, gpointer user_data) -> gboolean { return (ch == ' ' || ch == '\t'); }

static auto not_whitespace(gunichar ch, gpointer user_data) -> gboolean { return !whitespace(ch, user_data); }

static auto find_whitepace_region(const GtkTextIter* center, GtkTextIter* start, GtkTextIter* end) -> gboolean {
    *start = *center;
    *end = *center;

    if (gtk_text_iter_backward_find_char(start, not_whitespace, nullptr, nullptr)) {
        gtk_text_iter_forward_char(start); /* we want the first whitespace... */
    }
    if (whitespace(gtk_text_iter_get_char(end), nullptr)) {
        gtk_text_iter_forward_find_char(end, not_whitespace, nullptr, nullptr);
    }

    return !gtk_text_iter_equal(start, end);
}

void TextEditor::deleteFromCursor(GtkDeleteType type, int count) {
    GtkTextIter insert;
    // gboolean leave_one = false; // not needed

    this->resetImContext();

    if (type == GTK_DELETE_CHARS) {
        // Char delete deletes the selection, if one exists
        if (gtk_text_buffer_delete_selection(this->buffer, true, true)) {
            this->contentsChanged(true);
            this->repaintEditor();
            return;
        }
    }

    gtk_text_buffer_get_iter_at_mark(this->buffer, &insert, gtk_text_buffer_get_insert(this->buffer));

    GtkTextIter start = insert;
    GtkTextIter end = insert;

    switch (type) {
        case GTK_DELETE_CHARS:
            gtk_text_iter_forward_cursor_positions(&end, count);
            break;

        case GTK_DELETE_WORD_ENDS:
            if (count > 0) {
                gtk_text_iter_forward_word_ends(&end, count);
            } else if (count < 0) {
                gtk_text_iter_backward_word_starts(&start, 0 - count);
            }
            break;

        case GTK_DELETE_WORDS:
            break;

        case GTK_DELETE_DISPLAY_LINE_ENDS:
            break;

        case GTK_DELETE_DISPLAY_LINES:
            break;

        case GTK_DELETE_PARAGRAPH_ENDS:
            if (count > 0) {
                /* If we're already at a newline, we need to
                 * simply delete that newline, instead of
                 * moving to the next one.
                 */
                if (gtk_text_iter_ends_line(&end)) {
                    gtk_text_iter_forward_line(&end);
                    --count;
                }

                while (count > 0) {
                    if (!gtk_text_iter_forward_to_line_end(&end)) {
                        break;
                    }

                    --count;
                }
            } else if (count < 0) {
                if (gtk_text_iter_starts_line(&start)) {
                    gtk_text_iter_backward_line(&start);
                    if (!gtk_text_iter_ends_line(&end)) {
                        gtk_text_iter_forward_to_line_end(&start);
                    }
                } else {
                    gtk_text_iter_set_line_offset(&start, 0);
                }
                ++count;

                gtk_text_iter_backward_lines(&start, -count);
            }
            break;

        case GTK_DELETE_PARAGRAPHS:
            if (count > 0) {
                gtk_text_iter_set_line_offset(&start, 0);
                gtk_text_iter_forward_to_line_end(&end);

                /* Do the lines beyond the first. */
                while (count > 1) {
                    gtk_text_iter_forward_to_line_end(&end);
                    --count;
                }
            }

            break;

        case GTK_DELETE_WHITESPACE: {
            find_whitepace_region(&insert, &start, &end);
        } break;

        default:
            break;
    }

    if (!gtk_text_iter_equal(&start, &end)) {
        gtk_text_buffer_begin_user_action(this->buffer);

        if (gtk_text_buffer_delete_interactive(this->buffer, &start, &end, true)) {
            /*if (leave_one) // leave_one is statically false
            {
                gtk_text_buffer_insert_interactive_at_cursor(this->buffer, " ", 1, true);
            }*/
        } else {
            gtk_widget_error_bell(this->widget);
        }

        gtk_text_buffer_end_user_action(this->buffer);
    } else {
        gtk_widget_error_bell(this->widget);
    }

    this->contentsChanged();
    this->repaintEditor();
}

void TextEditor::backspace() {
    GtkTextIter insert;

    resetImContext();

    // Backspace deletes the selection, if one exists
    if (gtk_text_buffer_delete_selection(this->buffer, true, true)) {
        this->contentsChanged();
        this->repaintEditor();
        return;
    }

    gtk_text_buffer_get_iter_at_mark(this->buffer, &insert, gtk_text_buffer_get_insert(this->buffer));

    if (gtk_text_buffer_backspace(this->buffer, &insert, true, true)) {
        this->contentsChanged();
        this->repaintEditor();
    } else {
        gtk_widget_error_bell(this->widget);
    }
}

auto TextEditor::getSelection() -> string {
    GtkTextIter start, end;
    char* text = nullptr;
    string s;

    if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
        text = gtk_text_iter_get_text(&start, &end);
        s = text;
        g_free(text);
    }
    return s;
}

void TextEditor::copyToCliboard() {
    GtkClipboard* clipboard = gtk_widget_get_clipboard(this->widget, GDK_SELECTION_CLIPBOARD);
    gtk_text_buffer_copy_clipboard(this->buffer, clipboard);
}

void TextEditor::cutToClipboard() {
    GtkClipboard* clipboard = gtk_widget_get_clipboard(this->widget, GDK_SELECTION_CLIPBOARD);
    gtk_text_buffer_cut_clipboard(this->buffer, clipboard, true);

    this->contentsChanged(true);
    this->repaintEditor();
}

void TextEditor::pasteFromClipboard() {
    GtkClipboard* clipboard = gtk_widget_get_clipboard(this->widget, GDK_SELECTION_CLIPBOARD);
    gtk_text_buffer_paste_clipboard(this->buffer, clipboard, nullptr, true);
}

void TextEditor::bufferPasteDoneCallback(GtkTextBuffer* buffer, GtkClipboard* clipboard, TextEditor* te) {
    te->contentsChanged(true);
    te->repaintEditor();
}

void TextEditor::resetImContext() {
    if (this->needImReset) {
        this->needImReset = false;
        gtk_im_context_reset(this->imContext);
    }
}

void TextEditor::repaintCursor() { this->gui->repaintElement(this->text); }

#define CURSOR_ON_MULTIPLIER 2
#define CURSOR_OFF_MULTIPLIER 1
#define CURSOR_PEND_MULTIPLIER 3
#define CURSOR_DIVIDER 3

/*
 * Blink!
 */
auto TextEditor::blinkCallback(TextEditor* te) -> gint {
    if (te->cursorVisible) {
        te->blinkTimeout = gdk_threads_add_timeout(te->cursorBlinkTime * CURSOR_OFF_MULTIPLIER / CURSOR_DIVIDER,
                                                   reinterpret_cast<GSourceFunc>(blinkCallback), te);
    } else {
        te->blinkTimeout = gdk_threads_add_timeout(te->cursorBlinkTime * CURSOR_ON_MULTIPLIER / CURSOR_DIVIDER,
                                                   reinterpret_cast<GSourceFunc>(blinkCallback), te);
    }

    te->cursorVisible = !te->cursorVisible;

    te->repaintCursor();

    // Remove ourselves
    return false;
}

void TextEditor::setTextToPangoLayout(PangoLayout* pl) const {
    std::string txt = this->text->getText();

    if (!this->preeditString.empty()) {
        // When using an Input Method, we need to insert the preeditString into the text at the cursor location

        // Get the byte position of the cursor in the string, so we can insert at the right place
        int pos = 0;
        {
            // Get an iterator at the cursor location
            GtkTextIter it = {nullptr};
            GtkTextMark* cursor = gtk_text_buffer_get_insert(this->buffer);
            gtk_text_buffer_get_iter_at_mark(this->buffer, &it, cursor);
            // Bytes from beginning of line to iterator
            pos = gtk_text_iter_get_line_index(&it);
            gtk_text_iter_set_line_index(&it, 0);
            // Count bytes of previous lines
            while (gtk_text_iter_backward_line(&it)) {
                pos += gtk_text_iter_get_bytes_in_line(&it);
            }
        }
        txt.insert(static_cast<size_t>(pos), this->preeditString);

        PangoAttrList* attrlist = pango_attr_list_new();
        PangoAttrList* preedit_attrlist = this->preeditAttrList;
        pango_attr_list_splice(attrlist, preedit_attrlist, pos, static_cast<int>(preeditString.length()));
        pango_layout_set_attributes(pl, attrlist);
        pango_attr_list_unref(attrlist);
        attrlist = nullptr;
    }
    pango_layout_set_text(pl, txt.c_str(), static_cast<int>(txt.length()));
}

auto TextEditor::computeBoundingRect() -> xoj::util::Rectangle<double> {
    /*
     * We draw on a fake surface to get the size of the printed text
     * See also TextView::calcSize
     *
     * NB: we cannot rely on TextView::calcSize directly, since it would not take the size changes due to the IM
     * preeditString into account.
     */
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* cr = cairo_create(surface);
    auto* textElement = this->getText();

    PangoLayout* pl = xoj::view::TextView::initPango(cr, textElement);

    setTextToPangoLayout(pl);
    
    int w = 0;
    int h = 0;
    pango_layout_get_size(pl, &w, &h);
    double width = this->text->isFixedWidth() ? this->text->getElementWidth() : (static_cast<double>(w)) / PANGO_SCALE;
    double height = (static_cast<double>(h)) / PANGO_SCALE;
    g_object_unref(pl);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return xoj::util::Rectangle<double>(textElement->getX(), textElement->getY(), width, height);
}

auto TextEditor::getPreviousBoundingBoxWithPadding() -> xoj::util::Rectangle<double> {
    const double zoom = this->gui->getXournal()->getZoom();
    const double padding = (BORDER_WIDTH_IN_PIXELS + PADDING_IN_PIXELS) / zoom;
    return xoj::util::Rectangle<double>(this->previousBoundingBox.x - padding, this->previousBoundingBox.y - padding,
                           this->previousBoundingBox.width + 2.0 * padding,
                           this->previousBoundingBox.height + 2.0 * padding);

}


void TextEditor::repaintEditor() {
    auto rect = this->computeBoundingRect();
    this->previousBoundingBox.unite(rect);
    auto rectWithPadding = getPreviousBoundingBoxWithPadding();
    this->gui->repaintRect(rectWithPadding.x, rectWithPadding.y, rectWithPadding.width, rectWithPadding.height);
    this->previousBoundingBox = rect;
}

/**
 * Calculate the UTF-8 Char offset into a byte offset.
 */
auto TextEditor::getByteOffset(int charOffset) -> int {
    const char* text = pango_layout_get_text(this->layout);
    return g_utf8_offset_to_pointer(text, charOffset) - text;
}

/**
 * Calculate the UTF-8 Char byte offset into a char offset.
 */
auto TextEditor::getCharOffset(int byteOffset) -> int {
    const char* text = pango_layout_get_text(this->layout);

    return g_utf8_pointer_to_offset(text, text + byteOffset);
}

void TextEditor::drawCursor(cairo_t* cr, double x, double y, double height, double zoom) {
    double cw = 2 / zoom;
    double dX = 0;
    if (this->cursorOverwrite) {
        dX = -cw / 2;
        cw *= 2;
    }

    // Not draw cursor if a move is pending
    if (!this->markPosQueue) {
        if (this->cursorVisible) {
            cairo_save(cr);

            cairo_set_operator(cr, CAIRO_OPERATOR_DIFFERENCE);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_rectangle(cr, x + dX, y, cw, height);
            cairo_fill(cr);

            cairo_restore(cr);
        }
    }
    Util::cairo_set_source_rgbi(cr, this->text->getColor());
}

void TextEditor::paint(cairo_t* cr, double zoom) {
    GdkRGBA selectionColor = this->gui->getSelectionColor();

    cairo_save(cr);

    Util::cairo_set_source_rgbi(cr, this->text->getColor());

    double x0 = this->text->getX();
    double y0 = this->text->getY();
    cairo_translate(cr, x0, y0);
    double x1 = this->gui->getX();
    double y1 = this->gui->getY();

    if (this->layout == nullptr) {
        this->layout = xoj::view::TextView::initPango(cr, this->text);
    }

    this->setTextToPangoLayout(this->layout);

    if (this->preeditString.empty()) {
        GtkTextIter start;
        GtkTextIter end;
        bool hasSelection = gtk_text_buffer_get_selection_bounds(this->buffer, &start, &end);

        if (hasSelection) {
            auto selectionColorU16 = Util::GdkRGBA_to_ColorU16(selectionColor);
            PangoAttribute* attrib =
                    pango_attr_background_new(selectionColorU16.red, selectionColorU16.green, selectionColorU16.blue);
            attrib->start_index = getByteOffset(gtk_text_iter_get_offset(&start));
            attrib->end_index = getByteOffset(gtk_text_iter_get_offset(&end));

            PangoAttrList* attrlist = pango_attr_list_new();
            pango_attr_list_insert(attrlist, attrib);
            pango_layout_set_attributes(this->layout, attrlist);
            pango_attr_list_unref(attrlist);
            attrlist = nullptr;
        } else {
            // remove all attributes
            PangoAttrList* attrlist = pango_attr_list_new();
            pango_layout_set_attributes(this->layout, attrlist);
            pango_attr_list_unref(attrlist);
            attrlist = nullptr;
        }
    }

    pango_cairo_show_layout(cr, this->layout);
    int w = 0;
    int h = 0;
    pango_layout_get_size(this->layout, &w, &h);
    //Only calculate width if the text doesn't have a fixed width.
    double width = this->text->isFixedWidth() ? this->text->getElementWidth() : (static_cast<double>(w)) / PANGO_SCALE;
    double height = (static_cast<double>(h)) / PANGO_SCALE;


    GtkTextIter cursorIter = {nullptr};
    GtkTextMark* cursor = gtk_text_buffer_get_insert(this->buffer);
    gtk_text_buffer_get_iter_at_mark(this->buffer, &cursorIter, cursor);

    int offset = gtk_text_iter_get_offset(&cursorIter);
    PangoRectangle rect = {0};
    int pcursInd = 0;
    if (!this->preeditString.empty() && this->preeditCursor != 0) {
        const gchar* preeditText = this->preeditString.c_str();
        pcursInd = g_utf8_offset_to_pointer(preeditText, preeditCursor) - preeditText;
    }
    int pangoOffset = getByteOffset(offset) + pcursInd;
    pango_layout_index_to_pos(this->layout, pangoOffset, &rect);
    double cX = (static_cast<double>(rect.x)) / PANGO_SCALE;
    double cY = (static_cast<double>(rect.y)) / PANGO_SCALE;
    double cHeight = (static_cast<double>(rect.height)) / PANGO_SCALE;

    drawCursor(cr, cX, cY, cHeight, zoom);

    cairo_restore(cr);

    // set the line always the same size on display
    cairo_set_line_width(cr, BORDER_WIDTH_IN_PIXELS / zoom);
    gdk_cairo_set_source_rgba(cr, &selectionColor);

    cairo_rectangle(cr, x0 - PADDING_IN_PIXELS / zoom, y0 - PADDING_IN_PIXELS / zoom,
                    width + 2 * PADDING_IN_PIXELS / zoom, height + 2 * PADDING_IN_PIXELS / zoom);
    cairo_stroke(cr);

    // Notify the IM of the app's window and cursor position.
    GdkRectangle cursorRect;
    cursorRect.x = static_cast<int>(zoom * x0 + x1 + zoom * cX);
    cursorRect.y = static_cast<int>(zoom * y0 + y1 + zoom * cY);
    cursorRect.height = static_cast<int>(zoom * cHeight);
    // // The next setting treat the text box as if it were a cursor rectangle.
    // cursorRect.x = static_cast<int>(zoom * x0 + x1 - 10);
    // cursorRect.y = static_cast<int>(zoom * y0 + y1 - 10);
    // cursorRect.width = static_cast<int>(zoom * width + 20);
    // cursorRect.height = static_cast<int>(zoom * height + 20);
    // // This is also useful, so it is good to make it user's preference.
    gtk_im_context_set_cursor_location(this->imContext, &cursorRect);
    
    if (this->text->isFixedWidth())
        this->text->setWidth(width);
    
    this->text->setHeight(height);

    if (this->markPosQueue) {
        this->markPosQueue = false;
        markPos(this->markPosX, this->markPosY, this->markPosExtendSelection);
    }
}
