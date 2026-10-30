#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("Vibecode"));
    QApplication::setApplicationName(QStringLiteral("TextEditor"));
    QApplication::setApplicationDisplayName(QStringLiteral("Text Editor"));

    MainWindow window;
    const QStringList args = QApplication::arguments();
    if (args.size() > 1)
        window.loadFile(args.at(1));

    window.show();
    return app.exec();
}
