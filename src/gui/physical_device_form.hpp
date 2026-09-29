#pragma once

#include "joystick_penguin/config.hpp"
#include <QWidget>
#include <functional>

class QCheckBox;
class QLineEdit;

// Edits one physical controller; each proposal starts with the current profile,
// so a later field change cannot revert an earlier successful commit.
class PhysicalDeviceForm : public QWidget {
public:
    using Commit = std::function<bool(joystick_penguin::Config, bool refreshAfter, QString renamedKey)>;
    PhysicalDeviceForm(const joystick_penguin::Config& config, std::string key, Commit commit,
                       QWidget* parent = nullptr);
private:
    joystick_penguin::Config proposed() const;
    const joystick_penguin::Config& config_;
    std::string key_;
    Commit commit_;
    QLineEdit *name_, *path_;
    QCheckBox* grab_;
};
