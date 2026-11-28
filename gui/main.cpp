#include <QApplication>
#include <QIcon>

#include "mainwindow.h"

// Використання: p2p-gui [порт]. Порт має збігатися з портом демона p2pd (за замовчуванням 9000).
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("p2p-messenger");
    // Ім'я .desktop-файлу: за ним панель завдань пов'язує вікно з іконкою ярлика
    QGuiApplication::setDesktopFileName("p2p-messenger");
    app.setWindowIcon(QIcon::fromTheme("p2p-messenger"));

    int port = 9000;
    if (argc >= 2) port = QString(argv[1]).toInt();

    MainWindow w(port);
    w.show();
    return app.exec();
}
