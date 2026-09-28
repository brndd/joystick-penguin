#include "mapping_model.hpp"
#include "control_browser.hpp"
#include "joystick_penguin/joystick_preset.hpp"
#include <QApplication>
#include <QStyle>
#include <algorithm>
#include <type_traits>

using namespace joystick_penguin;
namespace mapping_ui {
namespace {
QString qs(const std::string& text) { return QString::fromStdString(text); }
QString joined(const std::vector<std::string>& values) {
    QStringList names;
    for (const auto& value : values) names << qs(value);
    return names.join(", ");
}
}

QString actionName(const Action& action) {
    return std::visit([](const auto& value) -> QString {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ModeAction>) return "Switch mode → " + qs(value.mode);
        if constexpr (std::is_same_v<T, ButtonAction>) {
            int index = 0;
            for (int i = 1; i <= joystick_button_count; ++i) if (joystick_button_code(i) == value.code) index = i;
            return qs(value.device) + (index ? QString(" · button %1").arg(index) : QString(" · EV_KEY %1").arg(value.code));
        }
        if constexpr (std::is_same_v<T, AxisAction>)
            return qs(value.device) + " · " + inputName({"", ControlKind::AbsoluteAxis, value.code}) + (value.invert ? " (inverted)" : "");
        if constexpr (std::is_same_v<T, HatAction>)
            return qs(value.device) + " · " + inputName({"", ControlKind::HatDirection, value.code, value.direction});
        return {};
    }, action);
}

QString actionSummary(const Binding& binding) {
    auto branch = [](const auto& actions) {
        QStringList names;
        for (const auto& action : actions) names << actionName(action);
        return names.empty() ? QString("No output") : names.join(" + ");
    };
    if (binding.tap_hold)
        return QString("Tap → %1; Hold → %2 · after %3 ms").arg(branch(binding.tap_hold->tap), branch(binding.tap_hold->hold)).arg(binding.tap_hold->threshold_ms);
    return (binding.input.kind == ControlKind::AbsoluteAxis ? "Axis → " : "Press → ") + branch(binding.actions);
}

BindingModel::BindingModel(const Config& config, QObject* parent) : QAbstractTableModel(parent), config_(config) {}
int BindingModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(config_.bindings.size()); }
int BindingModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : 5; }
QVariant BindingModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return QStringList{"Controller", "Input", "In modes", "While held", "Output"}.value(section);
}
QVariant BindingModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount()) return {};
    if (index.row() == errorRow_ && role == Qt::DecorationRole && index.column() == 0)
        return QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);
    if (index.row() == errorRow_ && role == Qt::ToolTipRole) return errorText_;
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole) return {};
    const auto& binding = config_.bindings.at(index.row());
    switch (index.column()) {
    case 0: return qs(binding.input.device);
    case 1: return labeledInput(config_, binding.input);
    case 2: return joined(binding.modes);
    case 3: return joined(binding.modifiers);
    case 4: return actionSummary(binding);
    default: return {};
    }
}
void BindingModel::changed(int row) { emit dataChanged(index(row, 0), index(row, 4)); }
void BindingModel::beginChange(int kind, int row) {
    if (kind == 0) beginResetModel();
    else if (kind == 1) beginInsertRows({}, row, row);
    else if (kind == 2) beginRemoveRows({}, row, row);
}
void BindingModel::endChange(int kind, int row) {
    if (kind == 0) { errorRow_ = -1; errorText_.clear(); endResetModel(); }
    else if (kind == 1) endInsertRows();
    else if (kind == 2) endRemoveRows();
    else changed(row);
}
void BindingModel::setError(int row, const QString& text) {
    const int previous = errorRow_; errorRow_ = row; errorText_ = text;
    if (previous >= 0 && previous < rowCount()) changed(previous);
    if (row >= 0 && row != previous && row < rowCount()) changed(row);
}

void BindingFilter::refresh() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange(); endFilterChange(Direction::Rows);
#else
    invalidateFilter();
#endif
}
bool BindingFilter::filterAcceptsRow(int row, const QModelIndex& parent) const {
    if (!config) return true;
    const auto& binding = config->bindings.at(row);
    if (inputFilter && std::find(inputs.begin(), inputs.end(), binding.input) == inputs.end()) return false;
    if (!device.isEmpty() && device != qs(binding.input.device)) return false;
    if (kind == "Axis" && binding.input.kind != ControlKind::AbsoluteAxis) return false;
    if (kind == "Hat" && binding.input.kind != ControlKind::HatDirection) return false;
    if (kind == "Button" && (binding.input.kind != ControlKind::Button || binding.input.code >= 0)) return false;
    if (kind == "EV_KEY" && (binding.input.kind != ControlKind::Button || binding.input.code < 0)) return false;
    auto contains = [](const auto& values, const QString& name) {
        return std::find(values.begin(), values.end(), name.toStdString()) != values.end();
    };
    if (!mode.isEmpty() && !contains(binding.modes, mode)) return false;
    if (!modifier.isEmpty() && !contains(binding.modifiers, modifier)) return false;
    if (QString("%1 %2").arg(binding.input.kind == ControlKind::Button ? "EV_KEY" : "EV_ABS").arg(binding.input.code).contains(search, Qt::CaseInsensitive)) return true;
    for (int column = 0; column < sourceModel()->columnCount(); ++column)
        if (sourceModel()->data(sourceModel()->index(row, column, parent)).toString().contains(search, Qt::CaseInsensitive)) return true;
    return false;
}
}
