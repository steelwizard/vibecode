#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QFont>
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QTextDocument>

class CodeEditor;
class QAction;
class QCheckBox;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QMenu;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    bool loadFile(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void createActions();
    void createMenus();
    void createFindBar();
    void createStatusBar();
    void readSettings();
    void writeSettings();

    void newFile();
    void open();
    bool save();
    bool saveAs();
    bool saveFile(const QString &path);
    bool maybeSave();
    void setCurrentFile(const QString &path);
    void updateWindowTitle();
    void updateRecentMenu();
    void updateStatus();

    void printDocument();
    void openFindBar(bool replaceFocus);
    void hideFindBar();
    void findNext();
    void findPrevious();
    bool findText(bool backward);
    void replace();
    void replaceAll();
    void updateSearchHighlights();
    QTextDocument::FindFlags searchFlags() const;
    void goToLine();
    void chooseFont();
    void zoomBy(int delta);
    void resetZoom();
    void about();

    static constexpr int MaxRecentFiles = 8;

    CodeEditor *editor = nullptr;
    QWidget *findBar = nullptr;
    QLineEdit *findEdit = nullptr;
    QLineEdit *replaceEdit = nullptr;
    QCheckBox *caseCheck = nullptr;
    QCheckBox *wholeCheck = nullptr;
    QLabel *matchLabel = nullptr;
    QLabel *positionLabel = nullptr;
    QLabel *statsLabel = nullptr;
    QLabel *modifiedLabel = nullptr;
    QMenu *recentMenu = nullptr;

    QAction *undoAction = nullptr;
    QAction *redoAction = nullptr;
    QAction *cutAction = nullptr;
    QAction *copyAction = nullptr;
    QAction *wordWrapAction = nullptr;

    QString currentPath;
    QString lastDirectory;
    QStringList recentFiles;
    QFont baseFont;
    int suppressSearchHighlights = 0;
};

#endif
