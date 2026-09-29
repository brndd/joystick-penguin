#pragma once

#include "joystick_penguin/config.hpp"

#include <QGroupBox>
#include <functional>

class QCheckBox;
class ButtonSelector;
class QComboBox;
class QSpinBox;
class QWidget;

// Edits one action inline. The owning action list applies result() when changed
// is called; this form never mutates the profile itself.
class ActionEditor : public QGroupBox {
public:
    ActionEditor(const joystick_penguin::Config& config, joystick_penguin::ControlKind input,
                 joystick_penguin::Action original, QWidget* parent);

    std::function<void()> changed;
    joystick_penguin::Action result() const;

private:
    void notifyChanged();
    void visible(QWidget* widget, bool show);
    void updateFields();
    void updateAxes();
    void updateButtons(bool preserveMissing = false);

    const joystick_penguin::Config& config_;
    QComboBox *type_, *device_, *buttonFormat_, *direction_, *mode_;
    QComboBox* outputAxis_;
    QComboBox* hatAxis_;
    ButtonSelector* button_;
    QSpinBox *buttonCode_, *axis_;
    QCheckBox* invert_;
    QCheckBox* literalHat_;
};
