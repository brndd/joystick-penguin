#pragma once

#include "joystick_penguin/config.hpp"
#include <QWidget>
#include <functional>

class QComboBox;
class QLineEdit;
class QSpinBox;

using SetupCommit = std::function<bool(joystick_penguin::Config, bool refreshAfter)>;

class ModeForm : public QWidget {
public:
    ModeForm(const joystick_penguin::Config& config, std::string key, SetupCommit commit,
             QWidget* parent = nullptr);
};

class ModifierForm : public QWidget {
public:
    ModifierForm(const joystick_penguin::Config& config, std::string key, SetupCommit commit,
                 std::function<void(joystick_penguin::Control)> showControl, QWidget* parent = nullptr);
private:
    joystick_penguin::Config proposed() const;
    const joystick_penguin::Config& config_;
    std::string key_;
    SetupCommit commit_;
    QLineEdit* name_;
    QComboBox *controller_, *format_;
    QSpinBox* code_;
};
