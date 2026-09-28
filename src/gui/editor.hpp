#pragma once

#include "joystick_penguin/config.hpp"

#include <QMainWindow>
#include <QString>

#include <memory>

class QCloseEvent;
class QEvent;
class QMenu;

class EditorWindow : public QMainWindow {
public:
    explicit EditorWindow(QWidget* parent = nullptr);
    ~EditorWindow() override;

    bool openProfile(const QString& path);
    const joystick_penguin::Config& config() const;

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    QMenu* createPopupMenu() override;

private:
    struct State;
    std::unique_ptr<State> state_;
    void newProfile();
    bool saveProfile(bool as);
    bool confirmDiscard();
    void validateProfile();
    void updateTitle();
    void showStatus(const QString& message);
};
