#pragma once
#include "joystick_penguin/config.hpp"
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>

namespace mapping_ui {
QString actionName(const joystick_penguin::Config& config, const joystick_penguin::Action& action);
QString actionSummary(const joystick_penguin::Config& config, const joystick_penguin::Binding& binding);

class BindingModel : public QAbstractTableModel {
public:
    explicit BindingModel(const joystick_penguin::Config& config, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    void changed(int row);
    void beginChange(int kind, int row);
    void endChange(int kind, int row);
    void setError(int row, const QString& text);
private:
    const joystick_penguin::Config& config_;
    int errorRow_ = -1;
    QString errorText_;
};

class BindingFilter : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    QString search, device, kind, mode, modifier;
    // Physical selections match inputs; virtual selections match output actions.
    // An empty selection hides every mapping.
    bool inputFilter = false;
    std::vector<joystick_penguin::Control> inputs;
    const joystick_penguin::Config* config = nullptr;
    void refresh();
protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
};
}
