#pragma once
#include "joystick_penguin/config.hpp"

// Snapshot history keeps setup reference rewrites and behavior changes atomic.
// Consecutive edits to the same mapping coalesce into one undo step so that
// filling in a mapping does not require many undos to reverse.
class ProfileHistory {
public:
    void reset(const joystick_penguin::Config& config) {
        states_ = {config};
        cursor_ = 0;
        coalescing_ = false;
        key_ = -1;
    }
    void commit(const joystick_penguin::Config& config) {
        coalescing_ = false;
        key_ = -1;
        push(config);
    }
    void commitCoalesced(const joystick_penguin::Config& config, int key) {
        if (coalescing_ && key_ == key && cursor_ + 1 == states_.size()) {
            if (states_.at(cursor_) != config) states_[cursor_] = config;
            return;
        }
        if (states_.empty()) { reset(config); return; }
        if (states_.at(cursor_) == config) return;
        push(config);
        coalescing_ = true;
        key_ = key;
    }
    void breakCoalescing() { coalescing_ = false; key_ = -1; }
    bool canUndo() const { return cursor_ > 0; }
    bool canRedo() const { return cursor_ + 1 < states_.size(); }
    joystick_penguin::Config undo() { if (canUndo()) --cursor_; return states_.at(cursor_); }
    joystick_penguin::Config redo() { if (canRedo()) ++cursor_; return states_.at(cursor_); }
private:
    void push(const joystick_penguin::Config& config) {
        if (states_.empty()) { states_ = {config}; cursor_ = 0; return; }
        if (states_.at(cursor_) == config) return;
        states_.resize(cursor_ + 1);
        states_.push_back(config);
        ++cursor_;
    }
    std::vector<joystick_penguin::Config> states_;
    std::size_t cursor_ = 0;
    bool coalescing_ = false;
    int key_ = -1;
};