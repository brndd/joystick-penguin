#pragma once

#include "profile_document.hpp"
#include <QWidget>
#include <functional>

class BindingDetail;
class ControlBrowser;
class QComboBox;
class QLineEdit;
class QStackedWidget;
class QTableView;
namespace mapping_ui { class BindingModel; class BindingFilter; }

// Owns the mapping tab's browsing state and its binding presentation. Source
// rows are the only row identifiers exposed to the window.
class MappingWorkspace : public QWidget {
public:
    explicit MappingWorkspace(ProfileDocument& document, QWidget* parent = nullptr);
    ~MappingWorkspace() override;
    std::function<void()> changed;
    std::function<void(bool virtualDevice)> requestDeviceSetup;

    void modelChange(ProfileDocument::Change change, int row, bool before);
    void refreshFilters();
    void refreshBinding();
    void selectBinding(int row);
    void showControl(const joystick_penguin::Control& input);
    void showMapping(int row);
    void resetBrowsing(bool resetKind = false);
    void refreshControls();
    void focusSearch();
    void setIssue(const QString& message);
    void setTableError(int row, const QString& message);
    void closeActionEditor();
    int selectedBinding() const { return selected_; }
    int bindingCount() const;

private:
    ProfileDocument& document_;
    const joystick_penguin::Config& config_;
    int selected_ = -1;
    mapping_ui::BindingModel* model_;
    mapping_ui::BindingFilter* proxy_;
    QTableView* table_;
    QLineEdit* search_;
    QComboBox *deviceFilter_, *kindFilter_, *modeFilter_, *modifierFilter_;
    BindingDetail* detail_;
    ControlBrowser* controls_;
    QStackedWidget* rightStack_;
    QWidget *normalPage_, *setupPrompt_;
    void updateRightView();
    void notifyChanged();
};
