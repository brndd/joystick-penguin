#pragma once
#include "joystick_penguin/config.hpp"
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

QString inputName(const joystick_penguin::Control& input);
QString labeledInput(const joystick_penguin::Config& config, const joystick_penguin::Control& input);
QString labeledOutputButton(const joystick_penguin::Config& config, const std::string& device, int number);

// Owns only read-only observation. It never touches the runtime or grabs a device.
class ControlBrowser : public QWidget {
public:
    explicit ControlBrowser(const joystick_penguin::Config& config, QWidget* parent = nullptr);
    ~ControlBrowser() override;
    void refresh();
    void resetBrowsing();
    void select(const joystick_penguin::Control& input);
    std::optional<joystick_penguin::Control> current() const;
    std::vector<joystick_penguin::Control> selectedControls() const;
    std::function<void(const std::vector<joystick_penguin::Control>&)> activated;
    std::function<void(joystick_penguin::Control, QString)> labelChanged;
private:
    struct State;
    std::unique_ptr<State> s_;
    void connectDevices(bool force);
    void rebuild();
    void displayActivity();
    void emitSelection();
    void selectVisible();
};
