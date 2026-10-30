#include "mainwindow.h"

#include "codeeditor.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDialog>
#include <QFrame>
#include <QGridLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPrintDialog>
#include <QPrinter>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QShortcut>
#include <QStatusBar>
#include <QStringConverter>
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextStream>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , editor(new CodeEditor(this))
{
    baseFont.setFamilies({QStringLiteral("Cascadia Mono"),
                          QStringLiteral("Cascadia Code"),
                          QStringLiteral("Consolas"),
                          QStringLiteral("Courier New")});
    baseFont.setStyleHint(QFont::Monospace);
    baseFont.setFixedPitch(true);
    baseFont.setPointSize(12);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(editor, 1);
    createFindBar();
    layout->addWidget(findBar);
    setCentralWidget(central);

    createActions();
    createMenus();
    createStatusBar();
    readSettings();

    editor->setFont(baseFont);
    updateWindowTitle();
    updateStatus();
    setWindowIcon(style()->standardIcon(QStyle::SP_FileIcon));
    editor->setFocus();

    connect(editor, &CodeEditor::fileDropped, this, [this](const QString &path) {
        if (maybeSave())
            loadFile(path);
    });
    connect(editor, &QPlainTextEdit::textChanged, this, [this]() {
        updateStatus();
        if (suppressSearchHighlights == 0 && findBar->isVisible())
            updateSearchHighlights();
    });
    connect(editor, &QPlainTextEdit::cursorPositionChanged, this, &MainWindow::updateStatus);
    connect(editor->document(), &QTextDocument::modificationChanged, this, [this](bool modified) {
        setWindowModified(modified);
        modifiedLabel->setText(modified ? tr("Modified") : QString());
    });
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (maybeSave()) {
        writeSettings();
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::createActions()
{
    undoAction = new QAction(tr("&Undo"), this);
    undoAction->setShortcut(QKeySequence::Undo);
    undoAction->setStatusTip(tr("Undo the last edit"));
    undoAction->setEnabled(false);
    undoAction->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    connect(undoAction, &QAction::triggered, editor, &QPlainTextEdit::undo);
    connect(editor, &QPlainTextEdit::undoAvailable, undoAction, &QAction::setEnabled);

    redoAction = new QAction(tr("&Redo"), this);
    redoAction->setShortcut(QKeySequence::Redo);
    redoAction->setStatusTip(tr("Redo the last undone edit"));
    redoAction->setEnabled(false);
    redoAction->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    connect(redoAction, &QAction::triggered, editor, &QPlainTextEdit::redo);
    connect(editor, &QPlainTextEdit::redoAvailable, redoAction, &QAction::setEnabled);

    cutAction = new QAction(tr("Cu&t"), this);
    cutAction->setShortcut(QKeySequence::Cut);
    cutAction->setStatusTip(tr("Cut the selection"));
    cutAction->setEnabled(false);
    connect(cutAction, &QAction::triggered, editor, &QPlainTextEdit::cut);
    connect(editor, &QPlainTextEdit::copyAvailable, cutAction, &QAction::setEnabled);

    copyAction = new QAction(tr("&Copy"), this);
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setStatusTip(tr("Copy the selection"));
    copyAction->setEnabled(false);
    connect(copyAction, &QAction::triggered, editor, &QPlainTextEdit::copy);
    connect(editor, &QPlainTextEdit::copyAvailable, copyAction, &QAction::setEnabled);

    wordWrapAction = new QAction(tr("&Word Wrap"), this);
    wordWrapAction->setCheckable(true);
    wordWrapAction->setChecked(true);
    wordWrapAction->setStatusTip(tr("Wrap long lines to the window width"));
    connect(wordWrapAction, &QAction::toggled, this, [this](bool checked) {
        editor->setLineWrapMode(checked ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    });
}

void MainWindow::createMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));

    QAction *newAction = fileMenu->addAction(tr("&New"), this, &MainWindow::newFile);
    newAction->setShortcut(QKeySequence::New);
    newAction->setStatusTip(tr("Create a blank document"));
    newAction->setIcon(style()->standardIcon(QStyle::SP_FileIcon));

    QAction *openAction = fileMenu->addAction(tr("&Open..."), this, &MainWindow::open);
    openAction->setShortcut(QKeySequence::Open);
    openAction->setStatusTip(tr("Open an existing file"));
    openAction->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));

    recentMenu = fileMenu->addMenu(tr("Open &Recent"));
    fileMenu->addSeparator();

    QAction *saveAction = fileMenu->addAction(tr("&Save"), this, &MainWindow::save);
    saveAction->setShortcut(QKeySequence::Save);
    saveAction->setStatusTip(tr("Save the document"));
    saveAction->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));

    QAction *saveAsAction = fileMenu->addAction(tr("Save &As..."), this, &MainWindow::saveAs);
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    saveAsAction->setStatusTip(tr("Save the document under a new name"));

    fileMenu->addSeparator();
    QAction *printAction = fileMenu->addAction(tr("&Print..."), this, &MainWindow::printDocument);
    printAction->setShortcut(QKeySequence::Print);
    printAction->setStatusTip(tr("Print the document"));

    fileMenu->addSeparator();
    QAction *exitAction = fileMenu->addAction(tr("E&xit"), this, &QWidget::close);
    exitAction->setShortcut(QKeySequence::Quit);
    exitAction->setStatusTip(tr("Quit the editor"));

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(undoAction);
    editMenu->addAction(redoAction);
    editMenu->addSeparator();
    editMenu->addAction(cutAction);
    editMenu->addAction(copyAction);

    QAction *pasteAction = editMenu->addAction(tr("&Paste"), editor, &QPlainTextEdit::paste);
    pasteAction->setShortcut(QKeySequence::Paste);
    pasteAction->setStatusTip(tr("Paste the clipboard"));

    QAction *deleteAction = editMenu->addAction(tr("&Delete"), this, [this]() {
        QTextCursor cursor = editor->textCursor();
        if (cursor.hasSelection())
            cursor.removeSelectedText();
        else
            cursor.deleteChar();
        editor->setTextCursor(cursor);
    });
    deleteAction->setShortcut(QKeySequence::Delete);
    deleteAction->setStatusTip(tr("Delete the selection"));

    editMenu->addSeparator();
    QAction *selectAllAction = editMenu->addAction(tr("Select &All"), editor, &QPlainTextEdit::selectAll);
    selectAllAction->setShortcut(QKeySequence::SelectAll);
    selectAllAction->setStatusTip(tr("Select the whole document"));

    editMenu->addSeparator();
    QAction *findAction = editMenu->addAction(tr("&Find..."), this, [this]() { openFindBar(false); });
    findAction->setShortcut(QKeySequence::Find);
    findAction->setStatusTip(tr("Find text"));
    findAction->setIcon(style()->standardIcon(QStyle::SP_FileDialogContentsView));

    QAction *replaceAction = editMenu->addAction(tr("&Replace..."), this, [this]() { openFindBar(true); });
    replaceAction->setShortcut(QKeySequence::Replace);
    replaceAction->setStatusTip(tr("Find and replace text"));

    QAction *findNextAction = editMenu->addAction(tr("Find &Next"), this, &MainWindow::findNext);
    findNextAction->setShortcut(QKeySequence::FindNext);
    findNextAction->setStatusTip(tr("Find the next match"));

    QAction *findPrevAction = editMenu->addAction(tr("Find Pre&vious"), this, &MainWindow::findPrevious);
    findPrevAction->setShortcut(QKeySequence::FindPrevious);
    findPrevAction->setStatusTip(tr("Find the previous match"));

    editMenu->addSeparator();
    QAction *goToAction = editMenu->addAction(tr("&Go to Line..."), this, &MainWindow::goToLine);
    goToAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    goToAction->setStatusTip(tr("Move the cursor to a line number"));

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(wordWrapAction);
    viewMenu->addSeparator();

    QAction *zoomInAction = viewMenu->addAction(tr("Zoom &In"), this, [this]() { zoomBy(1); });
    zoomInAction->setShortcut(QKeySequence::ZoomIn);
    zoomInAction->setStatusTip(tr("Increase the font size"));

    QAction *zoomOutAction = viewMenu->addAction(tr("Zoom &Out"), this, [this]() { zoomBy(-1); });
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    zoomOutAction->setStatusTip(tr("Decrease the font size"));

    QAction *resetZoomAction = viewMenu->addAction(tr("&Reset Zoom"), this, &MainWindow::resetZoom);
    resetZoomAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    resetZoomAction->setStatusTip(tr("Restore the original font size"));

    viewMenu->addSeparator();
    QAction *fontAction = viewMenu->addAction(tr("&Font..."), this, &MainWindow::chooseFont);
    fontAction->setStatusTip(tr("Choose the editor font"));

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About"), this, &MainWindow::about);
    helpMenu->addAction(tr("About &Qt"), qApp, &QApplication::aboutQt);

    auto *toolbar = addToolBar(tr("Main"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addAction(newAction);
    toolbar->addAction(openAction);
    toolbar->addAction(saveAction);
    toolbar->addSeparator();
    toolbar->addAction(undoAction);
    toolbar->addAction(redoAction);
    toolbar->addSeparator();
    toolbar->addAction(findAction);
}

void MainWindow::createFindBar()
{
    auto *frame = new QFrame(this);
    frame->setFrameShape(QFrame::NoFrame);
    frame->setStyleSheet(QStringLiteral(
        "QFrame { background: #f3f3f3; border-top: 1px solid #d0d0d0; }"));
    frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    findEdit = new QLineEdit(frame);
    replaceEdit = new QLineEdit(frame);
    findEdit->setPlaceholderText(tr("Find"));
    replaceEdit->setPlaceholderText(tr("Replace"));
    findEdit->setClearButtonEnabled(true);
    replaceEdit->setClearButtonEnabled(true);
    findEdit->setMinimumWidth(220);

    caseCheck = new QCheckBox(tr("Match case"), frame);
    wholeCheck = new QCheckBox(tr("Whole words"), frame);
    matchLabel = new QLabel(frame);
    matchLabel->setMinimumWidth(110);
    matchLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto *previousButton = new QPushButton(tr("Previous"), frame);
    auto *nextButton = new QPushButton(tr("Next"), frame);
    auto *replaceButton = new QPushButton(tr("Replace"), frame);
    auto *replaceAllButton = new QPushButton(tr("Replace All"), frame);
    auto *closeButton = new QToolButton(frame);
    closeButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    closeButton->setAutoRaise(true);
    closeButton->setToolTip(tr("Close"));

    auto *grid = new QGridLayout(frame);
    grid->setContentsMargins(8, 6, 8, 6);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(4);
    grid->addWidget(new QLabel(tr("Find"), frame), 0, 0);
    grid->addWidget(findEdit, 0, 1);
    grid->addWidget(previousButton, 0, 2);
    grid->addWidget(nextButton, 0, 3);
    grid->addWidget(caseCheck, 0, 4);
    grid->addWidget(matchLabel, 0, 5);
    grid->addWidget(closeButton, 0, 6);
    grid->addWidget(new QLabel(tr("Replace"), frame), 1, 0);
    grid->addWidget(replaceEdit, 1, 1);
    grid->addWidget(replaceButton, 1, 2);
    grid->addWidget(replaceAllButton, 1, 3);
    grid->addWidget(wholeCheck, 1, 4);

    connect(nextButton, &QPushButton::clicked, this, &MainWindow::findNext);
    connect(previousButton, &QPushButton::clicked, this, &MainWindow::findPrevious);
    connect(replaceButton, &QPushButton::clicked, this, &MainWindow::replace);
    connect(replaceAllButton, &QPushButton::clicked, this, &MainWindow::replaceAll);
    connect(closeButton, &QToolButton::clicked, this, &MainWindow::hideFindBar);
    connect(findEdit, &QLineEdit::returnPressed, this, &MainWindow::findNext);
    connect(replaceEdit, &QLineEdit::returnPressed, this, &MainWindow::replace);
    connect(findEdit, &QLineEdit::textChanged, this, &MainWindow::updateSearchHighlights);
    connect(caseCheck, &QCheckBox::toggled, this, &MainWindow::updateSearchHighlights);
    connect(wholeCheck, &QCheckBox::toggled, this, &MainWindow::updateSearchHighlights);

    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), frame);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, &MainWindow::hideFindBar);

    auto *shiftReturn = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Return), frame);
    shiftReturn->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shiftReturn, &QShortcut::activated, this, &MainWindow::findPrevious);

    frame->hide();
    findBar = frame;
}

void MainWindow::createStatusBar()
{
    modifiedLabel = new QLabel(this);
    statsLabel = new QLabel(this);
    positionLabel = new QLabel(this);
    auto *encodingLabel = new QLabel(tr("UTF-8"), this);

    const auto pad = [](QLabel *label) {
        label->setContentsMargins(8, 0, 8, 0);
    };
    pad(modifiedLabel);
    pad(statsLabel);
    pad(positionLabel);
    pad(encodingLabel);

    statusBar()->addPermanentWidget(modifiedLabel);
    statusBar()->addPermanentWidget(statsLabel);
    statusBar()->addPermanentWidget(positionLabel);
    statusBar()->addPermanentWidget(encodingLabel);
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::readSettings()
{
    QSettings settings;
    if (settings.contains(QStringLiteral("geometry")))
        restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
    else
        resize(960, 640);

    const QString fontDescription = settings.value(QStringLiteral("font")).toString();
    if (!fontDescription.isEmpty()) {
        QFont stored;
        if (stored.fromString(fontDescription))
            baseFont = stored;
    }

    const bool wrap = settings.value(QStringLiteral("wordWrap"), true).toBool();
    wordWrapAction->setChecked(wrap);
    editor->setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);

    lastDirectory = settings.value(QStringLiteral("lastDirectory")).toString();
    recentFiles = settings.value(QStringLiteral("recentFiles")).toStringList();
    recentFiles.removeAll(QString());
    while (recentFiles.size() > MaxRecentFiles)
        recentFiles.removeLast();
    updateRecentMenu();
}

void MainWindow::writeSettings()
{
    QSettings settings;
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("font"), editor->font().toString());
    settings.setValue(QStringLiteral("wordWrap"), wordWrapAction->isChecked());
    settings.setValue(QStringLiteral("lastDirectory"), lastDirectory);
    settings.setValue(QStringLiteral("recentFiles"), recentFiles);
}

void MainWindow::newFile()
{
    if (!maybeSave())
        return;

    editor->clear();
    setCurrentFile(QString());
    statusBar()->showMessage(tr("New document"), 2000);
}

void MainWindow::open()
{
    if (!maybeSave())
        return;

    const QString start = lastDirectory.isEmpty() ? QDir::homePath() : lastDirectory;
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open"),
        start,
        tr("All files (*);;Text files (*.txt *.md *.log);;C and C++ (*.c *.cc *.cpp *.cxx *.h *.hpp)"));
    if (path.isEmpty())
        return;

    loadFile(path);
}

bool MainWindow::loadFile(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        QMessageBox::warning(this, tr("Open"), tr("File not found:\n%1").arg(path));
        recentFiles.removeAll(path);
        updateRecentMenu();
        return false;
    }

    if (info.size() > 50LL * 1024 * 1024) {
        const auto answer = QMessageBox::question(
            this,
            tr("Open"),
            tr("This file is larger than 50 MB and may take a moment to open.\nOpen it anyway?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Open"),
                             tr("Could not open file:\n%1\n%2").arg(path, file.errorString()));
        return false;
    }

    if (file.peek(4096).contains('\0')) {
        const auto answer = QMessageBox::question(
            this,
            tr("Open"),
            tr("This file contains null bytes and may be binary.\nOpen it as text anyway?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return false;
    }

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    in.setAutoDetectUnicode(true);
    const QString text = in.readAll();
    if (in.status() != QTextStream::Ok) {
        QMessageBox::warning(this, tr("Open"), tr("Could not read file:\n%1").arg(path));
        return false;
    }

    editor->setPlainText(text);
    setCurrentFile(info.absoluteFilePath());
    statusBar()->showMessage(tr("Opened %1").arg(info.fileName()), 3000);
    return true;
}

bool MainWindow::save()
{
    if (currentPath.isEmpty())
        return saveAs();
    return saveFile(currentPath);
}

bool MainWindow::saveAs()
{
    QString suggested;
    if (!currentPath.isEmpty())
        suggested = currentPath;
    else if (!lastDirectory.isEmpty())
        suggested = QDir(lastDirectory).filePath(QStringLiteral("untitled.txt"));
    else
        suggested = QDir::home().filePath(QStringLiteral("untitled.txt"));

    const QString path = QFileDialog::getSaveFileName(
        this,
        tr("Save As"),
        suggested,
        tr("All files (*);;Text files (*.txt *.md *.log)"));
    if (path.isEmpty())
        return false;

    return saveFile(path);
}

bool MainWindow::saveFile(const QString &path)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Save"),
                             tr("Could not save file:\n%1\n%2").arg(path, file.errorString()));
        return false;
    }

    {
        // Destroy the stream before commit() so its buffer is flushed.
        QTextStream out(&file);
        out.setEncoding(QStringConverter::Utf8);
        out << editor->toPlainText();
        if (out.status() != QTextStream::Ok) {
            file.cancelWriting();
            QMessageBox::warning(this, tr("Save"), tr("Could not write file:\n%1").arg(path));
            return false;
        }
    }

    if (!file.commit()) {
        QMessageBox::warning(this, tr("Save"),
                             tr("Could not save file:\n%1\n%2").arg(path, file.errorString()));
        return false;
    }

    setCurrentFile(QFileInfo(path).absoluteFilePath());
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), 3000);
    return true;
}

bool MainWindow::maybeSave()
{
    if (!editor->document()->isModified())
        return true;

    const auto answer = QMessageBox::warning(
        this,
        tr("Text Editor"),
        tr("The document has been modified.\nDo you want to save your changes?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (answer == QMessageBox::Save)
        return save();
    if (answer == QMessageBox::Cancel)
        return false;
    return true;
}

void MainWindow::setCurrentFile(const QString &path)
{
    currentPath = path;
    editor->document()->setModified(false);
    setWindowModified(false);
    modifiedLabel->setText(QString());

    if (!path.isEmpty()) {
        lastDirectory = QFileInfo(path).absolutePath();
        recentFiles.removeAll(path);
        recentFiles.prepend(path);
        while (recentFiles.size() > MaxRecentFiles)
            recentFiles.removeLast();
        updateRecentMenu();
    }

    setWindowFilePath(path);
    updateWindowTitle();
}

void MainWindow::updateWindowTitle()
{
    const QString name = currentPath.isEmpty()
                             ? tr("Untitled")
                             : QFileInfo(currentPath).fileName();
    setWindowTitle(tr("%1[*] - Text Editor").arg(name));
    setWindowModified(editor->document()->isModified());
}

void MainWindow::updateRecentMenu()
{
    recentMenu->clear();
    if (recentFiles.isEmpty()) {
        QAction *empty = recentMenu->addAction(tr("No recent files"));
        empty->setEnabled(false);
        return;
    }

    for (const QString &path : recentFiles) {
        QAction *action = recentMenu->addAction(QFileInfo(path).fileName());
        action->setToolTip(path);
        action->setStatusTip(path);
        connect(action, &QAction::triggered, this, [this, path]() {
            if (!maybeSave())
                return;
            loadFile(path);
        });
    }

    recentMenu->addSeparator();
    recentMenu->addAction(tr("Clear Recent"), this, [this]() {
        recentFiles.clear();
        updateRecentMenu();
    });
}

void MainWindow::updateStatus()
{
    const QTextCursor cursor = editor->textCursor();
    positionLabel->setText(tr("Ln %1, Col %2").arg(cursor.blockNumber() + 1).arg(cursor.positionInBlock() + 1));

    const int characters = qMax(0, editor->document()->characterCount() - 1);
    const int lines = editor->blockCount();
    if (lines == 1)
        statsLabel->setText(tr("1 line, %1 characters").arg(characters));
    else
        statsLabel->setText(tr("%1 lines, %2 characters").arg(lines).arg(characters));
}

void MainWindow::printDocument()
{
    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle(tr("Print"));
    if (editor->textCursor().hasSelection())
        dialog.setOption(QAbstractPrintDialog::PrintSelection);
    if (dialog.exec() != QDialog::Accepted)
        return;

    editor->print(&printer);
}

void MainWindow::openFindBar(bool replaceFocus)
{
    const QString selected = editor->textCursor().selectedText();
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator) && selected.size() <= 200)
        findEdit->setText(selected);

    findBar->show();
    if (replaceFocus) {
        replaceEdit->setFocus();
        replaceEdit->selectAll();
    } else {
        findEdit->setFocus();
        findEdit->selectAll();
    }
    updateSearchHighlights();
}

void MainWindow::hideFindBar()
{
    findBar->hide();
    editor->setSearchText(QString(), {});
    matchLabel->clear();
    editor->setFocus();
}

void MainWindow::findNext()
{
    if (!findBar->isVisible())
        findBar->show();
    findText(false);
}

void MainWindow::findPrevious()
{
    if (!findBar->isVisible())
        findBar->show();
    findText(true);
}

bool MainWindow::findText(bool backward)
{
    const QString text = findEdit->text();
    if (text.isEmpty()) {
        findEdit->setFocus();
        matchLabel->clear();
        return false;
    }

    QTextDocument::FindFlags flags = searchFlags();
    if (backward)
        flags |= QTextDocument::FindBackward;

    const QTextCursor original = editor->textCursor();
    if (editor->find(text, flags)) {
        statusBar()->clearMessage();
        return true;
    }

    QTextCursor wrapped = original;
    wrapped.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
    editor->setTextCursor(wrapped);
    if (editor->find(text, flags)) {
        statusBar()->showMessage(backward ? tr("Search wrapped to the end")
                                          : tr("Search wrapped to the start"),
                                 2000);
        return true;
    }

    editor->setTextCursor(original);
    statusBar()->showMessage(tr("No matches for \"%1\"").arg(text), 3000);
    return false;
}

void MainWindow::replace()
{
    const QString text = findEdit->text();
    if (text.isEmpty()) {
        openFindBar(true);
        return;
    }

    QTextCursor cursor = editor->textCursor();
    if (cursor.hasSelection()) {
        const QString selected = cursor.selectedText();
        const bool same = caseCheck->isChecked()
                              ? selected == text
                              : selected.compare(text, Qt::CaseInsensitive) == 0;
        if (same)
            cursor.insertText(replaceEdit->text());
    }
    findText(false);
}

void MainWindow::replaceAll()
{
    const QString text = findEdit->text();
    if (text.isEmpty()) {
        openFindBar(true);
        return;
    }

    const QString replacement = replaceEdit->text();
    const QTextDocument::FindFlags flags = searchFlags();

    ++suppressSearchHighlights;
    QTextCursor blocker(editor->document());
    blocker.beginEditBlock();

    int count = 0;
    QTextCursor found = editor->document()->find(text, 0, flags);
    while (!found.isNull()) {
        found.insertText(replacement);
        ++count;
        found = editor->document()->find(text, found, flags);
    }

    blocker.endEditBlock();
    --suppressSearchHighlights;

    undoAction->setEnabled(editor->document()->isUndoAvailable());
    redoAction->setEnabled(editor->document()->isRedoAvailable());
    updateStatus();
    updateSearchHighlights();

    if (count == 0)
        statusBar()->showMessage(tr("No matches for \"%1\"").arg(text), 3000);
    else if (count == 1)
        statusBar()->showMessage(tr("Replaced 1 occurrence"), 3000);
    else
        statusBar()->showMessage(tr("Replaced %1 occurrences").arg(count), 3000);
}

void MainWindow::updateSearchHighlights()
{
    bool hitLimit = false;
    const int count = editor->setSearchText(findEdit->text(), searchFlags(), &hitLimit);
    if (findEdit->text().isEmpty()) {
        matchLabel->clear();
        return;
    }
    if (hitLimit)
        matchLabel->setText(tr("%1+ matches").arg(count));
    else if (count == 1)
        matchLabel->setText(tr("1 match"));
    else
        matchLabel->setText(tr("%1 matches").arg(count));
}

QTextDocument::FindFlags MainWindow::searchFlags() const
{
    QTextDocument::FindFlags flags;
    if (caseCheck->isChecked())
        flags |= QTextDocument::FindCaseSensitively;
    if (wholeCheck->isChecked())
        flags |= QTextDocument::FindWholeWords;
    return flags;
}

void MainWindow::goToLine()
{
    bool ok = false;
    const int lineCount = qMax(1, editor->blockCount());
    const int line = QInputDialog::getInt(
        this,
        tr("Go to Line"),
        tr("Line number (1-%1):").arg(lineCount),
        editor->textCursor().blockNumber() + 1,
        1,
        lineCount,
        1,
        &ok);
    if (!ok)
        return;

    const QTextBlock block = editor->document()->findBlockByNumber(line - 1);
    if (!block.isValid())
        return;

    QTextCursor cursor(block);
    editor->setTextCursor(cursor);
    editor->centerCursor();
    editor->setFocus();
}

void MainWindow::chooseFont()
{
    bool ok = false;
    const QFont font = QFontDialog::getFont(&ok, editor->font(), this, tr("Editor Font"));
    if (!ok)
        return;

    baseFont = font;
    editor->setFont(font);
}

void MainWindow::zoomBy(int delta)
{
    QFont font = editor->font();
    int size = font.pointSize();
    if (size < 1)
        size = 12;
    size = qBound(6, size + delta, 96);
    font.setPointSize(size);
    editor->setFont(font);
}

void MainWindow::resetZoom()
{
    editor->setFont(baseFont);
}

void MainWindow::about()
{
    QMessageBox::about(
        this,
        tr("About Text Editor"),
        tr("<h3>Text Editor</h3>"
           "<p>A plain text editor written with Qt and C++.</p>"
           "<p>Files are opened and saved as UTF-8.</p>"
           "<p>Built with Qt %1.</p>")
            .arg(QStringLiteral(QT_VERSION_STR)));
}
