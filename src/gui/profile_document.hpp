#pragma once

#include "document.hpp"
#include <QString>
#include <functional>

// All edits are proposed against a copy; observers see a consistent model
// transition and the live Config keeps its address for the lifetime of a view.
class ProfileDocument {
public:
    enum class Change { Reset, Insert, Remove, Update };
    using Observer = std::function<void(Change, int, bool before)>;

    ProfileDocument();
    const joystick_penguin::Config& config() const { return config_; }
    const std::vector<joystick_penguin::ConfigIssue>& issues() const { return issues_; }
    const QString& path() const { return path_; }
    bool dirty() const { return config_ != saved_; }
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }
    void observe(Observer observer) { observer_ = std::move(observer); }
    void replace(joystick_penguin::Config config, QString path = {});
    bool editMapping(int row, const std::function<void(joystick_penguin::Binding&)>& edit);
    bool editLabels(const std::function<void(joystick_penguin::Config&)>& edit);
    bool setup(joystick_penguin::Config config);
    bool insert(joystick_penguin::Binding binding);
    bool erase(int row);
    bool undo();
    bool redo();
    void saved(QString path);
    void breakMappingSession() { history_.breakCoalescing(); }
private:
    bool apply(joystick_penguin::Config config, Change change, int row, bool coalesce);
    void publish(joystick_penguin::Config config, Change change, int row);
    joystick_penguin::Config config_;
    joystick_penguin::Config saved_;
    QString path_;
    ProfileHistory history_;
    std::vector<joystick_penguin::ConfigIssue> issues_;
    Observer observer_;
};
