#pragma once

#include "profile_document.hpp"
#include <QWidget>
#include <functional>
#include <optional>

class ActionEditor;
class QLabel;
class QTableWidget;
class QVBoxLayout;

class ActionList : public QWidget {
public:
    explicit ActionList(ProfileDocument& document, QWidget* parent = nullptr);
    std::function<void()> edited;
    void selectBinding(int row);
    void refresh();
    void closeEditor();
    void clearSelection();
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    enum class Branch { Immediate, Tap, Hold };
    struct Location {
        int binding;
        Branch branch;
        int index;
        bool operator==(const Location&) const = default;
    };
    std::optional<Location> location(int tableRow) const;
    int tableRow(Location location) const;
    const std::vector<joystick_penguin::Action>* actions(Location location) const;
    static std::vector<joystick_penguin::Action>& actions(joystick_penguin::Binding& binding, Branch branch);
    void openEditor(int tableRow);
    void add();
    void remove(int tableRow);
    void move(int tableRow, int offset);
    void changeBranch(int tableRow, Branch branch);
    void notifyEdit(bool rebuild);

    ProfileDocument& document_;
    int binding_ = -1;
    bool filling_ = false;
    QTableWidget* table_;
    QVBoxLayout* editorLayout_;
    QLabel* hint_;
    ActionEditor* editor_ = nullptr;
    std::optional<Location> editorLocation_;
};
