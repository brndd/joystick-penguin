#include "profile_document.hpp"

using namespace joystick_penguin;

namespace {
Config blankProfile() {
    Config config;
    config.initial_mode = "default";
    config.modes = {"default"};
    return config;
}
}

ProfileDocument::ProfileDocument() : config_(blankProfile()), saved_(config_), issues_(config_issues(config_)) {
    history_.reset(config_);
}

void ProfileDocument::publish(Config config, Change change, int row) {
    if (observer_) observer_(change, row, true);
    config_ = std::move(config);
    issues_ = config_issues(config_);
    if (observer_) observer_(change, row, false);
}

bool ProfileDocument::apply(Config config, Change change, int row, bool coalesce) {
    if (config == config_) return false;
    if (coalesce) history_.commitCoalesced(config, row);
    else history_.commit(config);
    publish(std::move(config), change, row);
    return true;
}

void ProfileDocument::replace(Config config, QString path) {
    publish(std::move(config), Change::Reset, -1);
    path_ = std::move(path);
    saved_ = config_;
    history_.reset(config_);
}

bool ProfileDocument::editMapping(int row, const std::function<void(Binding&)>& edit) {
    if (row < 0 || row >= static_cast<int>(config_.bindings.size())) return false;
    auto proposed = config_;
    edit(proposed.bindings[row]);
    return apply(std::move(proposed), Change::Update, row, true);
}

bool ProfileDocument::editLabels(const std::function<void(Config&)>& edit) {
    auto proposed = config_;
    edit(proposed);
    return apply(std::move(proposed), Change::Reset, -1, false);
}

bool ProfileDocument::setup(Config config) { return apply(std::move(config), Change::Reset, -1, false); }

bool ProfileDocument::insert(Binding binding) {
    auto proposed = config_;
    const int row = static_cast<int>(proposed.bindings.size());
    proposed.bindings.push_back(std::move(binding));
    return apply(std::move(proposed), Change::Insert, row, false);
}

bool ProfileDocument::erase(int row) {
    if (row < 0 || row >= static_cast<int>(config_.bindings.size())) return false;
    auto proposed = config_;
    proposed.bindings.erase(proposed.bindings.begin() + row);
    return apply(std::move(proposed), Change::Remove, row, false);
}

bool ProfileDocument::undo() {
    if (!canUndo()) return false;
    history_.breakCoalescing();
    publish(history_.undo(), Change::Reset, -1);
    return true;
}

bool ProfileDocument::redo() {
    if (!canRedo()) return false;
    history_.breakCoalescing();
    publish(history_.redo(), Change::Reset, -1);
    return true;
}

void ProfileDocument::saved(QString path) {
    history_.breakCoalescing();
    path_ = std::move(path);
    saved_ = config_;
}
