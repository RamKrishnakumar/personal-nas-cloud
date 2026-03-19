#include <QApplication>
#include "ui/MainWindow.h"
#include "ui/TrayManager.h"

int main(int argc, char* argv[])    {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);   //Keep running in tray when window closed

    MainWindow window;
    window.show();

    TrayManager tray(&window);

    return app.exec();
}