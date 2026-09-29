#pragma once
#include "joystick_penguin/config.hpp"
#include <QWidget>
#include <functional>
#include <string>

class QListWidget;
class QVBoxLayout;
class QHideEvent;
class QShowEvent;

// Setup shares the document's transaction boundary and reference-update helpers.
// Edits apply as they are made; there is no Done/Cancel step.
class SetupWorkspace : public QWidget {
public:
    SetupWorkspace(const joystick_penguin::Config& config, bool devices, QWidget* parent = nullptr);
    void refresh();
    void reset() { refresh(); }
    // Creates a new device of the requested kind, as if the matching button was pressed.
    void requestAdd(bool virtualDevice);
    std::function<void(joystick_penguin::Config)> committed;
    std::function<void(joystick_penguin::Control)> showControl;
    std::function<void(int)> showMapping;
    std::function<void(QString)> error;
protected:
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;
private:
    const joystick_penguin::Config& config_;
    bool devices_;
    QListWidget* list_;
    QWidget* properties_;
    QVBoxLayout* detail_;
    QString usageKey_;
    int usageKind_ = 0;
    int usageScroll_ = 0;
    bool usageHidden_ = false;
    QString renamedSelection_;
    void select();
    void clearDetail();
    void showUsage(const std::string& key, bool modifier, int scrollPosition);
    bool commit(joystick_penguin::Config config, bool refreshAfter, QString renamedKey = {});
    void add(int kind);
    void remove(const QString& key, int kind);
    void setStartupMode(const QString& key);
};
