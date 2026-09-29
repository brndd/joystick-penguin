#pragma once

#include "profile_document.hpp"
#include <QWidget>
#include <functional>

class ActionList;
class ButtonSelector;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QListWidget;
class QSpinBox;
class QToolButton;
class QVBoxLayout;

// Presents one binding. All form changes are proposed through ProfileDocument;
// the window only coordinates browsing and the surrounding model/status views.
class BindingDetail : public QWidget {
public:
    explicit BindingDetail(ProfileDocument& document, QWidget* parent = nullptr);
    std::function<void(joystick_penguin::Control)> inputChanging;
    std::function<void()> edited;

    void selectBinding(int row);
    void refresh();
    void updateSummary();
    void setIssue(const QString& message);
    void closeActionEditor();

private:
    void buildInputFields(QVBoxLayout* layout);
    void buildConditions(QVBoxLayout* layout);
    void buildTiming(QVBoxLayout* layout);
    void connectEdits();
    void inputChanged();
    void inputKindChanged();
    void updateAxisCodes();
    void showInputField(QWidget* field, bool show);
    void notifyEdited();

    ProfileDocument& document_;
    int selected_ = -1;
    bool filling_ = false;
    ActionList* actionList_;
    QLabel *summary_, *issue_;
    QFormLayout* inputForm_;
    QComboBox *inputDevice_, *inputKind_, *buttonFormat_, *hatDirection_, *inputAxis_;
    ButtonSelector* inputButton_;
    QSpinBox *inputButtonCode_, *threshold_, *tapMs_;
    QWidget* timingRow_;
    QListWidget *modes_, *modifiers_;
    QCheckBox* timed_;
};
