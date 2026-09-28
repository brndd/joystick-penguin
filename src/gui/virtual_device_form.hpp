#pragma once

#include "joystick_penguin/config.hpp"
#include <QWidget>
#include <functional>

class QComboBox;
class QFormLayout;
class QLineEdit;
class QSpinBox;
class QTableWidget;
class QVBoxLayout;

class VirtualDeviceForm : public QWidget {
public:
    using Commit = std::function<bool(joystick_penguin::Config, bool refreshAfter)>;
    using Error = std::function<void(QString)>;
    VirtualDeviceForm(const joystick_penguin::Config& config, std::string key, Commit commit,
                      Error error, QWidget* parent = nullptr);
private:
    void buildProfileFields(QVBoxLayout* layout, const joystick_penguin::Device& device);
    void buildAdvancedFields(QVBoxLayout* layout, const joystick_penguin::Device& device);
    void buildAxisTable(QFormLayout* form, QWidget* parent, const joystick_penguin::Device& device);
    joystick_penguin::AxisRange axisRowRange(int row) const;
    void updateAxisRow(int row);
    void resizeAxisTable();
    void connectEdits();
    joystick_penguin::Config proposed() const;
    const joystick_penguin::Config& config_;
    std::string key_;
    Commit commit_;
    Error error_;
    QLineEdit *name_, *display_;
    QComboBox* bus_;
    QSpinBox *vendor_, *product_;
    QTableWidget* axes_;
};
