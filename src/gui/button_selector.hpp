#pragma once

#include "joystick_penguin/config.hpp"

#include <QComboBox>
#include <string>

// Indexed button choices for a configured controller. Item data is always the
// positive, one-based number shown in the profile (never an EV_KEY code).
class ButtonSelector : public QComboBox {
public:
    enum class Target { Physical, Virtual };

    ButtonSelector(const joystick_penguin::Config& config, Target target, QWidget* parent = nullptr);
    void setDevice(const std::string& name, int number = 1, bool preserveMissing = false);
    void setNumber(int number, bool preserveMissing = false);
    int number() const { return currentData().toInt(); }

private:
    const joystick_penguin::Config& config_;
    Target target_;
};
