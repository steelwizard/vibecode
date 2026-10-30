#ifndef CODEEDITOR_H
#define CODEEDITOR_H

#include <QPlainTextEdit>
#include <QTextDocument>

class QMimeData;

class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit CodeEditor(QWidget *parent = nullptr);

    void lineNumberAreaPaintEvent(QPaintEvent *event) const;
    int lineNumberAreaWidth() const;
    void selectLineAt(const QPoint &lineAreaPos);

    // Highlights matches. The count is capped; hitLimit is set when the cap is reached.
    int setSearchText(const QString &text, QTextDocument::FindFlags flags, bool *hitLimit = nullptr);

signals:
    void fileDropped(const QString &path);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect &rect, int dy);
    void highlightCurrentLine();
    void updateTabStop();
    bool hasLocalFile(const QMimeData *mime) const;

    QWidget *lineNumberArea = nullptr;
    QList<QTextEdit::ExtraSelection> searchSelections;
};

#endif
