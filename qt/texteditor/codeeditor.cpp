#include "codeeditor.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QTextBlock>
#include <QUrl>

namespace {

constexpr int kMaxSearchHighlights = 2000;

class LineNumberArea : public QWidget
{
public:
    explicit LineNumberArea(CodeEditor *editor)
        : QWidget(editor)
        , codeEditor(editor)
    {
    }

    QSize sizeHint() const override
    {
        return {codeEditor->lineNumberAreaWidth(), 0};
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        codeEditor->lineNumberAreaPaintEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            codeEditor->selectLineAt(event->position().toPoint());
            return;
        }
        QWidget::mousePressEvent(event);
    }

private:
    CodeEditor *codeEditor;
};

} // namespace

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    lineNumberArea = new LineNumberArea(this);

    QPalette editorPalette = palette();
    editorPalette.setColor(QPalette::Base, QColor(255, 255, 255));
    editorPalette.setColor(QPalette::Text, QColor(26, 26, 26));
    setPalette(editorPalette);

    setFrameStyle(QFrame::NoFrame);
    setCursorWidth(2);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setPlaceholderText(tr("Start typing, or open a file."));

    connect(this, &CodeEditor::blockCountChanged, this, [this](int) { updateLineNumberAreaWidth(); });
    connect(this, &CodeEditor::updateRequest, this, &CodeEditor::updateLineNumberArea);
    connect(this, &CodeEditor::cursorPositionChanged, this, [this]() {
        highlightCurrentLine();
        lineNumberArea->update();
    });

    updateTabStop();
    updateLineNumberAreaWidth();
    highlightCurrentLine();
}

int CodeEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    return 16 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth()
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy)
        lineNumberArea->scroll(0, dy);
    else
        lineNumberArea->update(0, rect.y(), lineNumberArea->width(), rect.height());

    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth();
}

void CodeEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect rect = contentsRect();
    lineNumberArea->setGeometry(QRect(rect.left(), rect.top(), lineNumberAreaWidth(), rect.height()));
}

void CodeEditor::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::FontChange) {
        updateTabStop();
        updateLineNumberAreaWidth();
    }
    QPlainTextEdit::changeEvent(event);
}

void CodeEditor::updateTabStop()
{
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1String("    ")));
}

void CodeEditor::highlightCurrentLine()
{
    QList<QTextEdit::ExtraSelection> selections;

    if (!isReadOnly()) {
        QTextEdit::ExtraSelection line;
        line.format.setBackground(QColor(232, 242, 254));
        line.format.setProperty(QTextFormat::FullWidthSelection, true);
        line.cursor = textCursor();
        line.cursor.clearSelection();
        selections.append(line);
    }

    selections.append(searchSelections);
    setExtraSelections(selections);
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event) const
{
    QPainter painter(lineNumberArea);
    painter.fillRect(event->rect(), QColor(243, 243, 243));

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int currentBlock = textCursor().blockNumber();

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const bool current = blockNumber == currentBlock;
            QFont numberFont = font();
            numberFont.setBold(current);
            painter.setFont(numberFont);
            painter.setPen(current ? QColor(30, 30, 30) : QColor(140, 140, 140));
            painter.drawText(0, top, lineNumberArea->width() - 8, fontMetrics().height(),
                             Qt::AlignRight, QString::number(blockNumber + 1));
        }

        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

void CodeEditor::selectLineAt(const QPoint &lineAreaPos)
{
    const QPoint viewportPos = viewport()->mapFromGlobal(lineNumberArea->mapToGlobal(lineAreaPos));
    QTextCursor cursor = cursorForPosition(QPoint(0, viewportPos.y()));
    cursor.movePosition(QTextCursor::StartOfBlock);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    setTextCursor(cursor);
}

int CodeEditor::setSearchText(const QString &text, QTextDocument::FindFlags flags, bool *hitLimit)
{
    searchSelections.clear();
    int count = 0;
    bool limited = false;

    if (!text.isEmpty()) {
        QTextCharFormat format;
        format.setBackground(QColor(255, 214, 92));
        format.setForeground(QColor(30, 30, 30));

        QTextCursor cursor(document());
        while (count < kMaxSearchHighlights) {
            cursor = document()->find(text, cursor, flags);
            if (cursor.isNull())
                break;

            QTextEdit::ExtraSelection selection;
            selection.cursor = cursor;
            selection.format = format;
            searchSelections.append(selection);
            ++count;
        }
        limited = count >= kMaxSearchHighlights;
    }

    if (hitLimit)
        *hitLimit = limited;

    highlightCurrentLine();
    return count;
}

bool CodeEditor::hasLocalFile(const QMimeData *mime) const
{
    if (!mime || !mime->hasUrls())
        return false;

    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile())
            return true;
    }
    return false;
}

void CodeEditor::dragEnterEvent(QDragEnterEvent *event)
{
    if (hasLocalFile(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    QPlainTextEdit::dragEnterEvent(event);
}

void CodeEditor::dragMoveEvent(QDragMoveEvent *event)
{
    if (hasLocalFile(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    QPlainTextEdit::dragMoveEvent(event);
}

void CodeEditor::dropEvent(QDropEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (mime && mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            if (url.isLocalFile()) {
                event->acceptProposedAction();
                emit fileDropped(url.toLocalFile());
                return;
            }
        }
    }
    QPlainTextEdit::dropEvent(event);
}
