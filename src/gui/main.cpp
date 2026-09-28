#include "editor.hpp"

#include <QApplication>
#include <QMessageBox>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Joystick Penguin Profile Editor");
    if (argc > 2) {
        QMessageBox::critical(nullptr, "Profile editor", "Usage: joystick-penguin-gui [profile.yaml]");
        return 2;
    }
    EditorWindow window;
    if (argc == 2 && !window.openProfile(QString::fromLocal8Bit(argv[1]))) return 1;
    window.show();
    return app.exec();
}
