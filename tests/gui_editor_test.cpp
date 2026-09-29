#include "editor.hpp"
#include "control_browser.hpp"
#include "button_selector.hpp"
#include "setup_workspace.hpp"
#include "joystick_penguin/joystick_preset.hpp"

#include <linux/input-event-codes.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEnterEvent>
#include <QFileDialog>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPushButton>
#include <QProcess>
#include <QSpinBox>
#include <QSplitter>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QTabWidget>
#include <QTreeWidget>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QWheelEvent>
#include <QScrollBar>
#include <QScrollArea>
#include <QPalette>

#include <filesystem>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

using namespace joystick_penguin;

namespace {

std::atomic<int> wildcardDisconnectWarnings{0};
QtMessageHandler previousMessageHandler = nullptr;

void captureDisconnectWarnings(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (message.contains("QObject::disconnect: wildcard call")) ++wildcardDisconnectWarnings;
    if (previousMessageHandler) previousMessageHandler(type, context, message);
}

void check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

template<class T> T* named(QWidget& window, const char* name) {
    auto* result = window.findChild<T*>(name);
    check(result != nullptr, std::string("missing widget: ") + name);
    return result;
}

QPushButton* button(QWidget& window, const QString& text) {
    for (auto* candidate : window.findChildren<QPushButton*>())
        if (candidate->text() == text) return candidate;
    throw std::runtime_error("missing button: " + text.toStdString());
}

QToolButton* toolButton(QWidget& window, const QString& name) {
    for (auto* candidate : window.findChildren<QToolButton*>())
        if (candidate->accessibleName() == name && candidate->isVisible()) return candidate;
    throw std::runtime_error("missing tool button: " + name.toStdString());
}

QString actionText(QTableWidgetItem* item) { return item->data(Qt::AccessibleTextRole).toString(); }

QAction* action_named(QWidget& window, const QString& text) {
    for (auto* candidate : window.findChildren<QAction*>())
        if (candidate->text() == text) return candidate;
    throw std::runtime_error("missing action: " + text.toStdString());
}

void selectAllControls(QWidget& window) {
    auto* tree = named<QTreeWidget>(window, "controlTree");
    tree->selectAll();
    QApplication::processEvents();
}

void addAction(QWidget& window, int type, int code = 0) {
    const int previous = named<QTableWidget>(window, "actionList")->rowCount();
    button(window, "Add action")->click();
    QApplication::processEvents();
    auto* list = named<QTableWidget>(window, "actionList");
    const int addedRow = list->currentRow();
    check(list->rowCount() == previous + 1 && addedRow >= 0, "added action appears and is selected");
    auto* editor = named<QGroupBox>(window, "inlineActionEditor");
    auto* kinds = named<QComboBox>(*editor, "actionType");
    kinds->setCurrentIndex(kinds->findData(type));
    if (type == 0 && code) {
        auto* choice = named<QComboBox>(*editor, "actionButton");
        choice->setCurrentIndex(choice->findData(code));
    }
    if (type == 1) named<QSpinBox>(*editor, "actionAxis")->setValue(code);
    if (type == 2) {
        auto* axes = named<QComboBox>(*editor, "actionOutputAxis");
        axes->setCurrentIndex(axes->findData(code));
    }
    if (type == 3) named<QComboBox>(*editor, "actionMode")->setCurrentText("alternate");
    QApplication::processEvents();
    check(!actionText(list->item(addedRow, 0)).isEmpty() &&
          (type != 3 || actionText(list->item(addedRow, 0)).contains("alternate")) &&
          (type != 0 || actionText(list->item(addedRow, 0)).contains(QString("button %1").arg(code))),
          "action label updates with editor changes");
}

void finishEdit(QLineEdit* line) {
    QMetaObject::invokeMethod(line, "editingFinished");
    QApplication::processEvents();
}

void enterControl(QWidget* control) {
    const QPointF local = control->rect().center();
    QEnterEvent enter(local, local, control->mapToGlobal(local.toPoint()));
    QApplication::sendEvent(control, &enter);
}

void moveInsideControl(QWidget* control, int dx = 8) {
    const QPointF local = control->rect().center() + QPoint(dx, 0);
    QMouseEvent move(QEvent::MouseMove, local, control->mapToGlobal(local.toPoint()),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(control, &move);
}

void wheelControl(QWidget* control, int steps) {
    const QPointF local = control->rect().center();
    QWheelEvent wheel(local, control->mapToGlobal(local.toPoint()), {}, {0, steps * 120},
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(control, &wheel);
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "jp-gui-test-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        check(mkdtemp(buffer.data()) != nullptr, "temp directory");
        path = buffer.data();
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};

void newProfileFromGui() {
    TemporaryDirectory temporary;
    EditorWindow fresh;
    fresh.show();
    QApplication::processEvents();

    check(named<QPushButton>(fresh, "createPhysical")->isVisible(), "empty profile shows setup prompt");
    check(fresh.config().devices.empty(), "no default dummy devices");

    QTimer::singleShot(0, [] {
        auto* prompt = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        check(prompt != nullptr, "device name prompt opened");
        auto* ok = prompt->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        check(!ok->isEnabled(), "empty controller name disables OK");
        prompt->setTextValue("   ");
        check(!ok->isEnabled(), "whitespace controller name disables OK");
        prompt->setTextValue("physical");
        check(ok->isEnabled(), "named controller enables OK");
        prompt->accept();
    });
    button(fresh, "Create a physical device")->click();
    QApplication::processEvents();
    finishEdit(named<QLineEdit>(fresh, "controllerPath"));
    named<QLineEdit>(fresh, "controllerPath")->setText("/dev/input/by-path/test-stick-event-joystick");
    finishEdit(named<QLineEdit>(fresh, "controllerPath"));
    QCheckBox* exclusive = nullptr;
    for (auto* box : fresh.findChildren<QCheckBox*>())
        if (box->text() == "Exclusive mode") exclusive = box;
    check(exclusive && exclusive->text() == "Exclusive mode", "physical controller exclusive option exists");
    exclusive->setChecked(false);
    named<QLineEdit>(fresh, "controllerPath")->setText("/dev/input/by-path/test-stick-event-joystick-2");
    finishEdit(named<QLineEdit>(fresh, "controllerPath"));
    check(!fresh.config().devices.at("physical").grab &&
          fresh.config().devices.at("physical").path == "/dev/input/by-path/test-stick-event-joystick-2",
          "successive physical property commits preserve earlier edits");

    QTimer::singleShot(0, [] {
        auto* prompt = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        check(prompt != nullptr, "virtual device name prompt opened");
        auto* ok = prompt->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        check(!ok->isEnabled(), "empty virtual joystick name disables OK");
        named<QLineEdit>(*prompt, "newVirtualName")->setText("  ");
        check(!ok->isEnabled(), "whitespace virtual joystick name disables OK");
        named<QLineEdit>(*prompt, "newVirtualName")->setText("virtual");
        check(ok->isEnabled(), "named virtual joystick enables OK");
        check(named<QComboBox>(*prompt, "virtualSourceDevice")->currentIndex() == 0,
               "virtual joystick can use default ranges without a physical device");
        auto* source = named<QComboBox>(*prompt, "virtualSourceDevice");
        auto* mirror = named<QCheckBox>(*prompt, "mirrorVirtualLabels");
        check(source->count() == 2 && !mirror->isEnabled(), "configured controller is available as a label source");
        source->setCurrentIndex(1);
        check(mirror->isEnabled(), "mirroring can be selected for a configured controller");
        source->setCurrentIndex(0);
        prompt->accept();
    });
    button(fresh, "Create a virtual device")->click();
    QApplication::processEvents();
    named<QLineEdit>(fresh, "virtualDeviceName")->setText("Test joystick");
    finishEdit(named<QLineEdit>(fresh, "virtualDeviceName"));
    auto* bus = named<QComboBox>(fresh, "virtualBus");
    check(bus->currentText() == "usb", "new virtual joystick defaults to USB bus identity");
    bus->setCurrentText("virtual");
    check(fresh.config().devices.at("virtual").bus == VirtualBus::Virtual, "virtual bus remains selectable");
    bus->setCurrentText("usb");
    check(named<QTableWidget>(fresh, "virtualAxes")->isVisible(), "advanced virtual properties shown without a toggle");
    check(named<QPushButton>(fresh, "refreshVirtualDevice")->isEnabled(), "virtual joystick offers controller refresh");
    const auto beforeRefresh = fresh.config();
    QTimer::singleShot(0, [] {
        auto* prompt = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        check(prompt != nullptr, "refresh prompts for a configured physical controller");
        prompt->accept();
    });
    named<QPushButton>(fresh, "refreshVirtualDevice")->click();
    check(fresh.config() == beforeRefresh, "offline refresh leaves the entire virtual configuration intact");
    named<QSpinBox>(fresh, "vendorId")->setValue(0x1234);
    named<QSpinBox>(fresh, "productId")->setValue(0x5678);
    auto* axes = named<QTableWidget>(fresh, "virtualAxes");
    check(axes->width() < fresh.width(), "axis table does not stretch across virtual device properties");
    toolButton(fresh, "About virtual bus identity")->click();
    check(QToolTip::text().contains("Virtual or USB"), "bus help is concise");
    QToolTip::hideText();
    if (!qEnvironmentVariable("JP_GUI_AXIS_SCREENSHOT").isEmpty())
        check(fresh.grab().save(qEnvironmentVariable("JP_GUI_AXIS_SCREENSHOT")), "save virtual axis layout screenshot");
    check(fresh.config().devices.at("virtual").axes.at(0) == AxisRange{0, 4095, 2048},
          "new virtual joystick defaults to 12-bit middle-neutral axes");
    check(axes->columnCount() == 5 && axes->horizontalHeaderItem(4)->text() == "Advanced mode" &&
          static_cast<QComboBox*>(axes->cellWidget(0, 1))->count() == 9,
          "axis editor offers 8–16 bits with advanced mode on the right");
    const auto presetFields = axes->cellWidget(0, 3)->findChildren<QSpinBox*>();
    check(presetFields.size() == 3 && presetFields.at(0)->isVisible() && !presetFields.at(0)->isEnabled() &&
          presetFields.at(0)->value() == 0 && presetFields.at(1)->value() == 4095 &&
          presetFields.at(2)->value() == 2048,
          "preset numeric ranges start visible, synchronized and disabled");
    static_cast<QComboBox*>(axes->cellWidget(1, 2))->setCurrentIndex(1);
    check(fresh.config().devices.at("virtual").axes.at(1) == AxisRange{-2048, 2047, 0},
          "zero-neutral axis uses signed 12-bit range");
    static_cast<QComboBox*>(axes->cellWidget(0, 1))->setCurrentIndex(0);
    check(fresh.config().devices.at("virtual").axes.at(0) == AxisRange{0, 255, 128} &&
          presetFields.at(1)->value() == 255 && !presetFields.at(1)->isEnabled(),
          "8-bit preset updates its read-only numeric fields");
    auto* firstAdvanced = static_cast<QCheckBox*>(axes->cellWidget(0, 4));
    firstAdvanced->setChecked(true);
    check(presetFields.at(1)->isEnabled() && presetFields.at(1)->value() == 255,
          "advanced mode exposes the current preset values for editing");
    firstAdvanced->setChecked(false);
    check(!presetFields.at(1)->isEnabled() && presetFields.at(1)->isVisible(),
          "turning off advanced mode grays out, but retains, the numeric fields");
    static_cast<QCheckBox*>(axes->cellWidget(2, 4))->setChecked(true);
    const auto custom = axes->cellWidget(2, 3)->findChildren<QSpinBox*>();
    check(custom.at(0)->isEnabled(), "advanced checkbox enables numeric fields");
    custom.at(0)->setValue(0);
    custom.at(1)->setValue(255);
    custom.at(2)->setValue(0);
    QApplication::processEvents();

    auto* tabs = fresh.findChild<QTabWidget*>();
    tabs->setCurrentIndex(2);
    QApplication::processEvents();
    QTimer::singleShot(0, [] {
        auto* prompt = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        check(prompt != nullptr, "mode name prompt opened");
        prompt->setTextValue("alternate");
        prompt->accept();
    });
    toolButton(fresh, "Add persistent mode")->click();
    QApplication::processEvents();
    QTimer::singleShot(0, [] {
        auto* prompt = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        check(prompt != nullptr, "modifier name prompt opened");
        prompt->setTextValue("shift");
        prompt->accept();
    });
    toolButton(fresh, "Add held modifier")->click();
    QApplication::processEvents();
    named<QComboBox>(fresh, "modifierButton")->setCurrentIndex(3);
    QApplication::processEvents();
    auto* modifierName = named<QLineEdit>(*tabs->widget(2), "setupName");
    modifierName->setText("shifted");
    finishEdit(modifierName);
    check(fresh.config().modifiers.contains("shifted") && !fresh.config().modifiers.contains("shift") &&
          fresh.config().modifiers.at("shifted").front().code == -4,
          "modifier rename commits its edited control");
    auto* conditionsList = named<QListWidget>(*tabs->widget(2), "workspaceModesModifiers");
    for (int i = 0; i < conditionsList->count(); ++i)
        if (conditionsList->item(i)->data(Qt::UserRole).toString() == "shifted")
            conditionsList->setCurrentRow(i);
    QApplication::processEvents();
    modifierName = named<QLineEdit>(*tabs->widget(2), "setupName");
    modifierName->setText("shift");
    finishEdit(modifierName);
    check(fresh.config().modifiers.contains("shift"), "renamed modifier can be selected and renamed again");
    for (int i = 0; i < conditionsList->count(); ++i)
        if (conditionsList->item(i)->data(Qt::UserRole).toString() == "shift") conditionsList->setCurrentRow(i);
    QApplication::processEvents();
    button(*tabs->widget(2), "Add button")->click();
    QApplication::processEvents();
    check(fresh.config().modifiers.at("shift").size() == 2, "modifier accepts an additional button");
    auto inputs = tabs->widget(2)->findChildren<QComboBox*>("modifierButton");
    check(inputs.size() == 2, "each modifier button has its own editor");
    check(inputs.at(0)->count() == 255 && inputs.at(0)->toolTip().contains("unverified"),
          "offline physical selector permits unverified indexed buttons");
    inputs.at(1)->setCurrentIndex(1);
    check(fresh.config().modifiers.at("shift")[1].code == -2, "second modifier button is editable");
    auto* secondFormat = inputs.at(1)->parentWidget()->findChild<QComboBox*>("modifierButtonFormat");
    secondFormat->setCurrentIndex(1);
    auto* secondCode = named<QSpinBox>(*inputs.at(1)->parentWidget(), "modifierButtonCode");
    secondCode->setValue(BTN_TRIGGER);
    check(fresh.config().modifiers.at("shift")[1].code == BTN_TRIGGER, "modifier EV_KEY literal remains editable");
    secondFormat->setCurrentIndex(0);
    check(fresh.config().modifiers.at("shift")[1].code == -2, "indexed modifier selection survives format switching");
    auto removes = tabs->widget(2)->findChildren<QPushButton*>();
    for (auto* candidate : removes)
        if (candidate->text() == "Remove button" && candidate->isVisible()) {
            // Remove the second row without changing the first assignment.
            if (candidate->parentWidget()->findChild<QComboBox*>("modifierButton") == inputs.at(1)) candidate->click();
        }
    QApplication::processEvents();
    check(fresh.config().modifiers.at("shift").size() == 1 &&
          fresh.config().modifiers.at("shift").front().code == -4,
          "removing one modifier button keeps its other assignment");

    check(fresh.config().devices.at("physical").path == "/dev/input/by-path/test-stick-event-joystick-2" &&
          fresh.config().devices.at("virtual").bus == VirtualBus::Usb &&
          fresh.config().devices.at("virtual").virtual_name == "Test joystick" &&
          fresh.config().devices.at("virtual").vendor_id == 0x1234 &&
          fresh.config().devices.at("virtual").product_id == 0x5678 &&
          fresh.config().devices.at("virtual").axes.at(2) == AxisRange{0, 255, 0} &&
          fresh.config().modes.size() == 2 && fresh.config().modifiers.at("shift").front().code == -4,
          "New profile device, mode, modifier and axis settings applied");

    tabs->setCurrentIndex(1);
    QApplication::processEvents();
    auto* restoredAxes = named<QTableWidget>(fresh, "virtualAxes");
    check(static_cast<QCheckBox*>(restoredAxes->cellWidget(2, 4))->isChecked() &&
          restoredAxes->cellWidget(2, 3)->findChildren<QSpinBox*>().at(1)->isEnabled(),
          "custom axis remains in advanced mode when the form is rebuilt");

    tabs->setCurrentIndex(0);
    QApplication::processEvents();
    check(!named<QPushButton>(fresh, "createPhysical")->isVisible(), "setup prompt disappears once both devices exist");
    button(fresh, "Add")->click();
    QApplication::processEvents();
    check(fresh.config().bindings.size() == 1, "binding created for the selected control");
    validate_edited_config(fresh.config());

    const auto path = temporary.path / "new-profile.yaml";
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        check(dialog != nullptr, "Save As dialog opened");
        dialog->selectFile(QString::fromStdString(path.string()));
        static_cast<QDialog*>(dialog)->accept();
    });
    action_named(fresh, "Save &As…")->trigger();
    check(load_config_file(path.string()) == fresh.config(), "New profile saved from GUI");
    check(!action_named(fresh, "&Save")->isEnabled() && action_named(fresh, "Save &As…")->isEnabled(),
          "Save is disabled after writing; Save As stays available");
    tabs->setCurrentIndex(2);
    QApplication::processEvents();
    QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        check(dialog != nullptr, "remove confirmation opened");
        dialog->button(QMessageBox::Cancel)->click();
    });
    toolButton(*tabs->widget(2), "Remove shift")->click();
    check(fresh.config().modifiers.contains("shift"), "cancel preserves modifier");
    QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        check(dialog != nullptr, "remove confirmation reopened");
        dialog->button(QMessageBox::Yes)->click();
    });
    toolButton(*tabs->widget(2), "Remove shift")->click();
    check(!fresh.config().modifiers.contains("shift"), "confirmed row removal deletes modifier");
    QProcess checkProcess;
    checkProcess.start(CLI_PATH, {"--check", QString::fromStdString(path.string())});
    check(checkProcess.waitForFinished(10000) && checkProcess.exitCode() == 0,
          "New profile passes --check without YAML edits");
}

void offlineButtonChoices() {
    Config config;
    config.devices.emplace("stick", Device{DeviceKind::Evdev, "/dev/input/by-id/absent-button-selector", true, ""});
    config.devices.emplace("vjoy", Device{DeviceKind::Uinput, "", true, "joystick"});
    config.input_labels.push_back({{"stick", ControlKind::Button, -4}, "Trigger"});
    config.input_labels.push_back({{"stick", ControlKind::Button, BTN_TRIGGER}, "Raw trigger"});
    ButtonSelector physical(config, ButtonSelector::Target::Physical);
    physical.setDevice("stick", 4);
    check(physical.count() == 255 && physical.currentText() == "Trigger (Button 4)" &&
          physical.itemText(0) == "Button 1" && !physical.currentText().contains("Raw trigger"),
          "offline indexed choices use only their exact physical labels");
    physical.setNumber(255);
    check(physical.number() == 255, "offline dropdown allows the maximum supported indexed button");
    ButtonSelector output(config, ButtonSelector::Target::Virtual);
    config.output_labels.push_back({"vjoy", 79, "Auxiliary"});
    output.setDevice("vjoy", 79);
    check(output.count() == joystick_button_count && output.number() == 79 &&
           output.currentText() == "Auxiliary (Button 79)", "virtual selector uses virtual labels without physical labels");
}

void virtualCreationMirrorsLabels() {
    TemporaryDirectory temporary;
    auto config = load_config_file((std::filesystem::path(EXAMPLES_DIR) / "basic.yaml").string());
    config.input_labels.push_back({{"physical", ControlKind::Button, -2}, "Trigger"});
    config.devices.at("physical").path = "/dev/input/by-id/absent-mirror-source";
    const auto path = temporary.path / "mirroring.yaml";
    save_config_file(config, path.string());
    EditorWindow window;
    window.show();
    check(window.openProfile(QString::fromStdString(path.string())), "open profile with physical labels for virtual creation");
    auto* tabs = window.findChild<QTabWidget*>();
    tabs->setCurrentIndex(1);
    QTimer::singleShot(0, [] {
        auto* prompt = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        check(prompt != nullptr, "virtual creation prompt opened for mirroring");
        named<QLineEdit>(*prompt, "newVirtualName")->setText("mirror");
        named<QComboBox>(*prompt, "virtualSourceDevice")->setCurrentIndex(1);
        named<QCheckBox>(*prompt, "mirrorVirtualLabels")->setChecked(true);
        prompt->accept();
    });
    toolButton(*tabs->widget(1), "Add virtual joystick")->click();
    QApplication::processEvents();
    check(window.config().output_labels == std::vector<OutputLabel>{{"mirror", 2, "Trigger"}} &&
          window.config().devices.at("mirror").axes == joystick_axes(),
          "creation mirrors saved labels even when the configured source is offline");
    validate_edited_config(window.config());
}

void multiControllerBrowsing() {
    TemporaryDirectory temporary;
    auto config = load_config_file((std::filesystem::path(EXAMPLES_DIR) / "basic.yaml").string());
    config.devices.at("physical").path = "/dev/input/by-id/absent-multi-browser";
    config.devices.emplace("other", Device{DeviceKind::Evdev, "/dev/input/by-id/absent-other"});
    config.devices.emplace("other-vjoy", config.devices.at("virtual"));
    config.bindings.push_back({{"other", ControlKind::Button, -2}, {"default"}, {},
                               {ButtonAction{"other-vjoy", joystick_button_code(2)}}, std::nullopt});
    config.bindings.push_back({{"physical", ControlKind::Button, -3}, {"alternate"}, {}, {},
                               TapHold{200, 50, {ButtonAction{"other-vjoy", joystick_button_code(3)}},
                                       {ButtonAction{"other-vjoy", joystick_button_code(2)}}}});
    const auto path = temporary.path / "browse.yaml";
    save_config_file(config, path.string());
    EditorWindow window;
    window.show();
    check(window.openProfile(QString::fromStdString(path.string())), "open multi-controller profile");
    auto* tree = named<QTreeWidget>(window, "controlTree");
    auto* table = named<QTableView>(window, "bindingTable");
    auto* combo = named<QComboBox>(window, "controllerBrowser");
    check(combo->currentText() == "All" && combo->count() == 7 &&
          combo->itemText(1) == "Physical only" && combo->itemText(2) == "Virtual only" &&
          tree->topLevelItemCount() == 4 && table->model()->rowCount() == 4,
          "All defaults to all configured controllers and all bindings");
    auto root = [&](const QString& name) -> QTreeWidgetItem* {
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(0).startsWith(name + " ·")) return tree->topLevelItem(i);
        throw std::runtime_error("missing controller root: " + name.toStdString());
    };
    auto* virtualRoot = root("other-vjoy");
    check(virtualRoot->child(0)->childCount() == joystick_button_count,
          "virtual preset buttons appear without running hardware");
    virtualRoot->setExpanded(false);
    combo->setCurrentIndex(1);
    check(tree->topLevelItemCount() == 2 && table->model()->rowCount() == 4 &&
          root("physical") && root("other"), "Physical only shows both physical controllers");
    combo->setCurrentIndex(2);
    check(tree->topLevelItemCount() == 2 && table->model()->rowCount() == 4 &&
          root("virtual") && root("other-vjoy"), "Virtual only shows both virtual controllers: " +
          std::to_string(tree->topLevelItemCount()) + " roots, " + std::to_string(table->model()->rowCount()) + " mappings");
    tree->setCurrentItem(root("virtual")->child(0)->child(1));
    tree->selectAll();
    check(table->model()->rowCount() == 4 && root("other-vjoy")->child(0)->child(1)->isSelected(),
          "Select All includes controls in collapsed controller branches");
    combo->setCurrentText("physical");
    check(tree->topLevelItemCount() == 1, "controller dropdown narrows the tree");
    combo->setCurrentText("All");
    check(!root("other-vjoy")->isExpanded(), "controller expansion survives filtering");
    auto* button2 = root("other-vjoy")->child(0)->child(1);
    tree->setCurrentItem(button2);
    check(table->model()->rowCount() == 2 && button2->text(1) == "2",
           "virtual button selects immediate and Hold output mappings");
    button2->setText(0, "Flight trigger");
    QApplication::processEvents();
    check(window.config().output_labels == std::vector<OutputLabel>{{"other-vjoy", 2, "Flight trigger"}} &&
          table->model()->rowCount() == 2, "inline virtual label preserves output filtering");
    check(table->model()->index(0, 4).data().toString().contains("Flight trigger"),
          "output summaries use virtual labels");
    action_named(window, "&Undo")->trigger();
    check(window.config().output_labels.empty(), "virtual label edit is undoable");
    action_named(window, "&Redo")->trigger();
    check(window.config().output_labels.size() == 1, "virtual label edit is redoable");
    button2 = root("other-vjoy")->child(0)->child(1);
    button2->setText(0, "Button 2");
    QApplication::processEvents();
    check(window.config().output_labels.empty(), "clearing virtual label retains its button identity");
    QLineEdit* controlSearch = nullptr;
    for (auto* line : window.findChildren<QLineEdit*>())
        if (line->placeholderText() == "Find control…") controlSearch = line;
    check(controlSearch != nullptr, "control search is available");
    controlSearch->setText("Button 3 ");
    check(root("other-vjoy")->child(0)->childCount() == 1 &&
          root("other-vjoy")->child(0)->child(0)->text(0) == "Button 3",
          "control search matches virtual button numbers");
    check(table->model()->rowCount() == 2, "control search does not discard hidden selections");
    controlSearch->clear();
    button2 = root("other-vjoy")->child(0)->child(1);
    check(button2->isSelected(), "hidden selection reappears after clearing control search");
    auto* button3 = root("other-vjoy")->child(0)->child(2);
    tree->setCurrentItem(button3);
    check(table->model()->rowCount() == 1 && button3->text(1) == "1",
          "Tap output mapping is visible from its virtual button");
    tree->setCurrentItem(button2);
    auto* physicalRoot = root("physical");
    tree->setCurrentItem(physicalRoot->child(0)->child(0), 0, QItemSelectionModel::Select);
    check(table->model()->rowCount() == 4,
          "Ctrl-style selection combines physical input and virtual output mappings without duplicates");
    table->setCurrentIndex(table->model()->index(0, 0));
    check(table->model()->rowCount() == 4 && button2->isSelected(),
          "opening a mapping does not discard multiple control selections");
    tree->setCurrentItem(root("other-vjoy")->child(0));
    check(table->model()->rowCount() == 2, "clicking a group selects all its output controls");
    tree->setCurrentItem(root("other"));
    check(table->model()->rowCount() == 1, "clicking a physical controller selects its inputs");
}

void existingActionSelection() {
    TemporaryDirectory temporary;
    auto config = load_config_file((std::filesystem::path(EXAMPLES_DIR) / "basic.yaml").string());
    config.devices.at("physical").path = "/dev/input/by-id/absent-action-selection";
    config.bindings.push_back({{"physical", ControlKind::AbsoluteAxis, ABS_X}, {"default"}, {},
                               {AxisAction{"virtual", ABS_RY, true}}, std::nullopt});
    config.bindings.push_back({{"physical", ControlKind::HatDirection, ABS_HAT0X, -1}, {"default"}, {},
                               {HatAction{"virtual", ABS_HAT0Y, 1}}, std::nullopt});
    const auto path = temporary.path / "actions.yaml";
    save_config_file(config, path.string());
    EditorWindow window;
    window.show();
    check(window.openProfile(QString::fromStdString(path.string())), "open action selection profile");
    selectAllControls(window);
    auto* bindings = named<QTableView>(window, "bindingTable");
    auto* actions = named<QTableWidget>(window, "actionList");
    auto select = [&](int row) {
        bindings->setCurrentIndex(bindings->model()->index(row, 0));
        QApplication::processEvents();
    };
    select(2);
    check(actions->currentRow() == 0, "selecting an axis binding selects its first action");
    auto* axisEditor = named<QGroupBox>(window, "inlineActionEditor");
    check(named<QComboBox>(*axisEditor, "actionType")->currentData().toInt() == 2 &&
          named<QComboBox>(*axisEditor, "actionOutputAxis")->currentData().toInt() == ABS_RY &&
          named<QCheckBox>(*axisEditor, "actionInvert")->isChecked(),
          "existing axis action initializes its type, declared output axis and inversion");
    select(3);
    check(actions->currentRow() == 0, "selecting a hat binding selects its first action");
    auto* hatEditor = named<QGroupBox>(window, "inlineActionEditor");
    check(named<QComboBox>(*hatEditor, "actionType")->currentData().toInt() == 1 &&
          named<QSpinBox>(*hatEditor, "actionAxis")->value() == ABS_HAT0Y,
          "existing hat action initializes its type and component");
    select(0);
    check(actions->currentRow() == 0 && named<QGroupBox>(window, "inlineActionEditor"),
          "selecting a button binding opens its first action");
    check(window.config() == config, "browsing existing actions leaves the profile unchanged");
}

void overhaul() {
    EditorWindow window;
    window.resize(1024, 768);
    window.show();
    TemporaryDirectory fixture;
    auto offline = load_config_file((std::filesystem::path(EXAMPLES_DIR) / "star_citizen.yaml").string());
    for (auto& [name, device] : offline.devices)
        if (device.kind == DeviceKind::Evdev) device.path = "/dev/input/by-id/absent-gui-test-" + name;
    save_config_file(offline, (fixture.path / "offline.yaml").string());
    const auto path = QString::fromStdString((fixture.path / "offline.yaml").string());
    check(window.openProfile(path), "open large overhaul fixture");
    auto* toolbar = named<QToolBar>(window, "profileToolbar");
    check(toolbar->actions().size() >= 7 && !toolbar->actions().front()->icon().isNull(), "icon-oriented document toolbar");
    check(!toolbar->isMovable(), "toolbar cannot be rearranged");
    check(toolbar->contextMenuPolicy() == Qt::NoContextMenu, "toolbar visibility menu disabled");
    check(!action_named(window, "&Save")->isEnabled(), "unchanged profile cannot be saved again");
    auto* tree = named<QTreeWidget>(window, "controlTree");
    check(tree->topLevelItemCount() == static_cast<int>(window.config().devices.size()),
          "all configured controllers appear as roots");
    for (int d = 0; d < tree->topLevelItemCount(); ++d) {
        auto* root = tree->topLevelItem(d);
        check(root->childCount() == 3 && root->child(0)->text(0) == "Buttons" &&
              root->child(1)->text(0) == "Hats" && root->child(2)->text(0) == "Axes",
              "controls are grouped within each controller");
        for (int g = 0; g < root->childCount(); ++g)
            for (int i = 0; i < root->child(g)->childCount(); ++i)
                check(!root->child(g)->child(i)->text(2).isEmpty(), "every listed control has a passive indicator");
    }
    check(named<QLabel>(window, "monitorStatus")->text().contains("Offline"), "absent hardware has visible offline feedback");
    selectAllControls(window);
    auto* table = named<QTableView>(window, "bindingTable");
    check(table->height() >= table->horizontalHeader()->height() + 3 * 2 * table->fontMetrics().height(),
          "mappings overview can show three rows by default");
    check(named<QListWidget>(window, "bindingModes")->height() > 100 &&
          named<QListWidget>(window, "bindingModifiers")->height() > 100,
          "condition lists can show five entries");
    bool timed = false;
    for (int i = 0; i < table->model()->rowCount(); ++i) {
        const auto summary = table->model()->index(i, 4).data().toString();
        if (summary.contains("Tap →")) { timed = true; check(summary.contains("Hold →") && summary.contains("button"), "tap/hold overview shows outputs rather than counts"); }
    }
    check(timed, "large fixture exposes tap and hold summaries");
    const auto input = window.config().bindings.front().input;
    QTreeWidgetItem* first = nullptr;
    for (int d = 0; d < tree->topLevelItemCount(); ++d)
        if (tree->topLevelItem(d)->text(0).startsWith(QString::fromStdString(input.device) + " ·"))
            first = tree->topLevelItem(d)->child(2)->child(0);
    check(first != nullptr, "offline axis present");
    tree->setCurrentItem(first);
    selectAllControls(window);
    table->setCurrentIndex(table->model()->index(1, 0));
    check(table->currentIndex().isValid() && table->currentIndex().row() == 1,
          "mapping selected before editing a control label");
    first->setText(0, "Flight roll");
    QApplication::processEvents();
    check(table->currentIndex().isValid() && table->currentIndex().row() == 1,
          "editing a control label preserves the selected mapping row: " +
          std::to_string(table->currentIndex().row()));
    check(window.config().input_labels.size() == 1, "label stored once per physical identity");
    check(action_named(window, "&Save")->isEnabled(), "editing enables Save");
    check(window.config().bindings.front().input == input, "label does not change input identity");
    selectAllControls(window);
    auto* search = named<QLineEdit>(window, "bindingSearch");
    search->setText("Flight roll");
    check(table->model()->rowCount() > 0, "mappings search includes labels");
    auto* undo = action_named(window, "&Undo");
    auto* redo = action_named(window, "&Redo");
    undo->trigger();
    check(window.config().input_labels.empty() && search->text() == "Flight roll", "undo label preserves search context");
    redo->trigger();
    check(window.config().input_labels.size() == 1 && table->model()->rowCount() > 0, "redo label restores searchable name");
    search->clear();
    selectAllControls(window);
    table->setCurrentIndex(table->model()->index(0, 0));
    QApplication::processEvents();
    const auto before = window.config();
    addAction(window, 0, 3);
    check(window.config() != before, "adding an action edits the mapping inline");
    undo->trigger();
    check(window.config() == before, "mapping edits coalesce into one undo step");
    // A mapping field edit is undoable as a single transaction.
    selectAllControls(window);
    table->setCurrentIndex(table->model()->index(0, 0));
    QApplication::processEvents();
    const auto beforeField = window.config();
    auto* inputAxis = named<QComboBox>(window, "inputAxis");
    const int oldAxis = inputAxis->currentIndex();
    inputAxis->setCurrentIndex((oldAxis + 1) % inputAxis->count());
    QApplication::processEvents();
    check(window.config() != beforeField, "input edit applies immediately");
    undo->trigger();
    check(window.config() == beforeField, "input edit is one undoable transaction");
    button(window, "Duplicate")->click();
    auto* issues = named<QListWidget>(window, "profileIssues");
    check(issues->count() >= 2, "conflict panel links both mappings");
    check(!issues->item(0)->text().contains("%2") && issues->item(0)->text().contains("conflict"),
          "issue location includes its message without QString placeholder warnings");
    button(window, "Delete")->click();
    check(issues->count() == 0, "repair clears structured issues");
    auto* error = named<QLabel>(window, "validationError");
    check(!error->text().isEmpty(), "validation statusbar always reports state");
    check(error->toolTip().isEmpty(), "valid status has no issues-navigation tooltip");
    auto* tabs = window.findChild<QTabWidget*>();
    tabs->setCurrentIndex(1);
    QApplication::processEvents();
    auto* devices = named<QListWidget>(window, "workspaceDevices");
    devices->setCurrentRow(1);
    QApplication::processEvents();
    auto* availability = named<QLabel>(*tabs->widget(1), "controllerAvailability");
    check(!availability->text().isEmpty() && availability->width() > 0, "availability is visible for offline controller");
    if (!qEnvironmentVariable("JP_GUI_DEVICE_SCREENSHOT").isEmpty())
        check(window.grab().save(qEnvironmentVariable("JP_GUI_DEVICE_SCREENSHOT")), "save device layout screenshot");
    auto* controllerPath = named<QLineEdit>(*tabs->widget(1), "controllerPath");
    controllerPath->setText("/dev/input/by-id/offline-overhaul-test");
    finishEdit(controllerPath);
    check(window.config().devices.at("left").path == "/dev/input/by-id/offline-overhaul-test", "first-class device workspace commits inline properties");
    undo->trigger();
    QApplication::processEvents();
    check(named<QLineEdit>(*tabs->widget(1), "controllerPath")->text().contains("absent-gui-test"), "undo refreshes visible setup properties");
    redo->trigger();
    QApplication::processEvents();
    check(named<QLineEdit>(*tabs->widget(1), "controllerPath")->text() == "/dev/input/by-id/offline-overhaul-test", "redo refreshes visible setup properties");
    auto* physicalName = named<QLineEdit>(*tabs->widget(1), "setupName");
    physicalName->setText("z-left");
    finishEdit(physicalName);
    check(devices->currentItem()->data(Qt::UserRole).toString() == "z-left" &&
          named<QLineEdit>(*tabs->widget(1), "setupName")->text() == "z-left",
          "renaming a controller keeps its detail page selected");
    for (int i = 0; i < devices->count(); ++i)
        if (devices->item(i)->data(Qt::UserRole).toString() == "left-vjoy") devices->setCurrentRow(i);
    QApplication::processEvents();
    auto* virtualName = named<QLineEdit>(*tabs->widget(1), "setupName");
    virtualName->setText("z-left-vjoy");
    finishEdit(virtualName);
    check(devices->currentItem()->data(Qt::UserRole).toString() == "z-left-vjoy" &&
          named<QLineEdit>(*tabs->widget(1), "setupName")->text() == "z-left-vjoy",
          "renaming a virtual joystick keeps its detail page selected");
    tabs->setCurrentIndex(2);
    QApplication::processEvents();
    auto* conditions = named<QListWidget>(window, "workspaceModesModifiers");
    check(conditions->count() > 2, "modes and modifiers have section headers");
    toolButton(*tabs->widget(2), "Set Auxiliary Mode as default mode")->click();
    QApplication::processEvents();
    check(window.config().initial_mode == "Auxiliary Mode", "star changes default mode");
    check(action_named(window, "&Save")->isEnabled(), "default mode change enables Save");
    undo->trigger();
    QApplication::processEvents();
    check(window.config().initial_mode != "Auxiliary Mode" &&
          toolButton(*tabs->widget(2), "Set Auxiliary Mode as default mode")->isVisible(),
          "undo restores the setup value and its visible form");
    redo->trigger();
    QApplication::processEvents();
    check(window.config().initial_mode == "Auxiliary Mode", "redo reapplies setup transaction");
    conditions->setMaximumHeight(130);
    conditions->verticalScrollBar()->setValue(conditions->verticalScrollBar()->maximum());
    const int scrollPosition = conditions->verticalScrollBar()->value();
    check(scrollPosition > 0, "mode list can scroll");
    auto* usage = named<QTableWidget>(*tabs->widget(2), "setupUsage");
    check(usage->rowCount() > 0, "selected mode has linked mappings");
    check(usage->isVisible(), "usage table is visible after refreshing mode: " +
          std::to_string(usage->width()) + "x" + std::to_string(usage->height()) +
          " parent visible " + std::to_string(usage->parentWidget()->isVisible()));
    if (!qEnvironmentVariable("JP_GUI_MODES_SCREENSHOT").isEmpty())
        check(window.grab().save(qEnvironmentVariable("JP_GUI_MODES_SCREENSHOT")), "save mode layout screenshot");
    usage->verticalScrollBar()->setValue(usage->verticalScrollBar()->maximum());
    const int usageScroll = usage->verticalScrollBar()->value();
    check(usageScroll > 0, "mode usage table can scroll");
    int mappingIndex = usage->item(0, 0)->data(Qt::UserRole).toInt();
    const auto expectedOutput = usage->item(0, 4)->text();
    search->setText("no-mapping-matches-this-search");
    check(table->model()->rowCount() == 0, "search hides the usage mapping before navigation");
    usage->cellClicked(0, 0);
    QApplication::processEvents();
    check(tabs->currentIndex() == 0, "one click on usage opens Mappings");
    auto* mappingTable = named<QTableView>(window, "bindingTable");
    check(search->text().isEmpty(), "usage navigation clears a search that hides its mapping");
    check(mappingTable->currentIndex().isValid() &&
          mappingTable->model()->index(mappingTable->currentIndex().row(), 4).data().toString() == expectedOutput &&
          mappingTable->currentIndex().data().toString() == QString::fromStdString(window.config().bindings[mappingIndex].input.device),
          "usage selects the referenced mapping");
    tabs->setCurrentIndex(2);
    QApplication::processEvents();
    check(conditions->verticalScrollBar()->value() == scrollPosition, "returning to modes retains list scroll position");
    auto* restoredUsage = named<QTableWidget>(*tabs->widget(2), "setupUsage");
    const int restoredUsageScroll = restoredUsage->verticalScrollBar()->value();
    check(restoredUsageScroll == std::min(usageScroll, restoredUsage->verticalScrollBar()->maximum()),
          "returning to modes retains right-hand mapping scroll position: expected " +
              std::to_string(usageScroll) + ", got " + std::to_string(restoredUsageScroll) +
              ", maximum " + std::to_string(restoredUsage->verticalScrollBar()->maximum()));
    conditions->setMaximumHeight(QWIDGETSIZE_MAX);
    tabs->setCurrentIndex(0);
    selectAllControls(window);
    table->setCurrentIndex(table->model()->index(17, 0));
    QApplication::processEvents();
    auto* actionList = named<QTableWidget>(window, "actionList");
    auto* actionSplit = named<QSplitter>(window, "actionSplit");
    check(actionSplit->orientation() == Qt::Horizontal,
          "actions list and editor are side by side");
    check(actionSplit->sizes().front() >= 230, "action list pane has space for labels and row buttons");
    const int originalActions = actionList->rowCount();
    addAction(window, 0, 3);
    check(actionList->rowCount() == originalActions + 1, "adding an action updates the visible list");
    auto* originalRow = actionList->cellWidget(0, 0);
    auto* rowLabel = named<QLabel>(*originalRow, "actionRowLabel");
    check(actionList->item(0, 0)->text().isEmpty() && !rowLabel->text().isEmpty(),
          "action text is drawn only by the row label");
    check(rowLabel->geometry().right() < toolButton(*originalRow, "Move action 1 down")->geometry().left(),
          "action label stays clear of its buttons");
    QMouseEvent selectRow(QEvent::MouseButtonPress, QPointF(6, 6), QPointF(6, 6), QPointF(6, 6),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(originalRow, &selectRow);
    check(actionList->currentRow() == 0 && named<QGroupBox>(window, "inlineActionEditor")->isVisible(),
          "single click on action row selects its editor");
    int addedRow = -1;
    for (int row = 0; row < actionList->rowCount(); ++row)
        if (actionText(actionList->item(row, 0)).contains("button 3")) addedRow = row;
    check(addedRow >= 0, "new action is present before moving");
    const int offset = addedRow ? -1 : 1;
    toolButton(window, QString("Move action %1 %2").arg(addedRow + 1).arg(offset < 0 ? "up" : "down"))->click();
    QApplication::processEvents();
    check(actionText(actionList->item(addedRow + offset, 0)).contains("button 3"), "row arrow reorders actions");
    toolButton(window, QString("Remove action %1").arg(addedRow + 1))->click();
    QApplication::processEvents();
    check(actionList->rowCount() == originalActions, "row trash button removes the chosen action");
    if (!qEnvironmentVariable("JP_GUI_SCREENSHOT").isEmpty())
        check(window.grab().save(qEnvironmentVariable("JP_GUI_SCREENSHOT")), "save visual review screenshot");
    check(window.width() == 1024, "workspace supports 1024 logical pixel width");
}

void deviceRemovalFromGui() {
    TemporaryDirectory temporary;
    auto config = load_config_file((std::filesystem::path(EXAMPLES_DIR) / "basic.yaml").string());
    config.devices.emplace("second", config.devices.at("virtual"));
    config.bindings.front().actions.push_back(ButtonAction{"second", 290});
    const auto profile = temporary.path / "remove.yaml";
    save_config_file(config, profile.string());
    EditorWindow window;
    window.show();
    check(window.openProfile(QString::fromStdString(profile.string())), "open device-removal profile");
    auto* tabs = window.findChild<QTabWidget*>();
    tabs->setCurrentIndex(1);
    QApplication::processEvents();
    QTimer::singleShot(0, [] {
        auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        check(warning && warning->text().contains("2 mapping(s)") &&
              warning->text().contains("modifier") && warning->text().contains("unassigned"),
              "physical removal warns about mappings and orphaned modifiers");
        warning->button(QMessageBox::Cancel)->click();
    });
    toolButton(*tabs->widget(1), "Remove physical")->click();
    check(window.config() == config, "canceling device removal preserves the profile");
    QTimer::singleShot(0, [] {
        auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        check(warning && warning->text().contains("2 mapping(s)"), "physical removal consequence is visible");
        warning->button(QMessageBox::Yes)->click();
    });
    toolButton(*tabs->widget(1), "Remove physical")->click();
    QApplication::processEvents();
    check(window.config().bindings.empty() && window.config().modifiers.at("shift").empty() &&
          !window.config().devices.contains("physical"), "physical removal keeps modifier orphaned");
    validate_edited_config(window.config());
    action_named(window, "&Undo")->trigger();
    QApplication::processEvents();
    check(window.config() == config, "device removal is one undoable transaction");

    QTimer::singleShot(0, [] {
        auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        check(warning && warning->text().contains("output action(s)") &&
              warning->text().contains("1 mapping(s)"), "virtual removal warns about changed and deleted mappings");
        warning->button(QMessageBox::Yes)->click();
    });
    toolButton(*tabs->widget(1), "Remove virtual")->click();
    QApplication::processEvents();
    check(window.config().bindings.size() == 1 &&
          window.config().bindings.front().actions == std::vector<Action>{ButtonAction{"second", 290}} &&
          !window.config().devices.contains("virtual"), "virtual removal keeps unaffected mapping actions");
    validate_edited_config(window.config());
    QTimer::singleShot(0, [] {
        auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        check(warning != nullptr, "physical cleanup confirmation reopened");
        warning->button(QMessageBox::Yes)->click();
    });
    toolButton(*tabs->widget(1), "Remove physical")->click();
    QApplication::processEvents();
    QTimer::singleShot(0, [] {
        auto* prompt = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        check(prompt != nullptr, "replacement controller name prompt opened");
        prompt->setTextValue("replacement");
        prompt->accept();
    });
    toolButton(*tabs->widget(1), "Add controller")->click();
    tabs->setCurrentIndex(2);
    QApplication::processEvents();
    auto* modifiers = named<QListWidget>(*tabs->widget(2), "workspaceModesModifiers");
    for (int i = 0; i < modifiers->count(); ++i)
        if (modifiers->item(i)->data(Qt::UserRole).toString() == "shift") modifiers->setCurrentRow(i);
    QApplication::processEvents();
    check(window.config().modifiers.at("shift").empty(), "orphan remains available for reassignment");
    button(*tabs->widget(2), "Add button")->click();
    QApplication::processEvents();
    check(window.config().modifiers.at("shift").size() == 1 &&
          window.config().modifiers.at("shift").front().device == "replacement",
          "orphaned modifier can be repointed to a replacement controller");
    validate_edited_config(window.config());
}

void run() {
    offlineButtonChoices();
    virtualCreationMirrorsLabels();
    multiControllerBrowsing();
    existingActionSelection();
    { EditorWindow defaultWindow; check(defaultWindow.size() == QSize(1600, 900), "initial window is 1600x900"); }
    overhaul();
    newProfileFromGui();
    deviceRemovalFromGui();
    {
        EditorWindow blank;
        check(blank.config().devices.empty() && blank.config().bindings.empty(), "blank profile has no default devices");
        validate_edited_config(blank.config());
    }
    EditorWindow window;
    window.show();
    auto* table = named<QTableView>(window, "bindingTable");
    auto* error = named<QLabel>(window, "validationError");
    auto* search = named<QLineEdit>(window, "bindingSearch");

    // All shipped profiles must survive loading into the editor unchanged.
    TemporaryDirectory checkedExamples;
    for (const auto* filename : {"basic.yaml", "gestures.yaml", "controls.yaml", "modes.yaml",
                                 "hardware.yaml", "star_citizen.yaml"}) {
        const auto path = (std::filesystem::path(EXAMPLES_DIR) / filename).string();
        check(window.openProfile(QString::fromStdString(path)), std::string("open ") + filename);
        check(window.config() == load_config_file(path), std::string("unchanged ") + filename);
        check(load_config(serialize_config(window.config())) == window.config(),
              std::string("round trip ") + filename);
        const auto saved = checkedExamples.path / filename;
        save_config_file(window.config(), saved.string());
        QProcess cli;
        cli.start(CLI_PATH, {"--check", QString::fromStdString(saved.string())});
        check(cli.waitForFinished(10000) && cli.exitCode() == 0, std::string("saved example passes --check: ") + filename);
    }

    TemporaryDirectory temporary;
    auto profile = temporary.path / "edited.yaml";
    std::filesystem::copy_file(std::filesystem::path(EXAMPLES_DIR) / "basic.yaml", profile);
    check(window.openProfile(QString::fromStdString(profile.string())), "open editable profile");
    selectAllControls(window);
    auto* physicalButtons = named<QComboBox>(window, "inputButton");
    check(physicalButtons->count() == 255 && physicalButtons->toolTip().contains("unverified"),
          "offline mapping selector exposes the full unverified indexed range");
    check(table->model()->rowCount() == 2, "initial bindings");
    search->setText("button 1");
    check(table->model()->rowCount() == 2, "search by control");
    search->setText("unmatched");
    check(table->model()->rowCount() == 0, "search filters rows");
    search->clear();
    table->setCurrentIndex(table->model()->index(0, 0));
    QApplication::processEvents();
    physicalButtons->setCurrentIndex(1);
    QApplication::processEvents();
    check(window.config().bindings[0].input.code == -2, "one-based physical button edited");
    named<QComboBox>(window, "inputButtonFormat")->setCurrentIndex(1);
    named<QSpinBox>(window, "inputButtonCode")->setValue(BTN_TRIGGER);
    check(window.config().bindings[0].input.code == BTN_TRIGGER, "mapping EV_KEY literal remains editable");
    named<QComboBox>(window, "inputButtonFormat")->setCurrentIndex(0);
    check(window.config().bindings[0].input.code == -2, "mapping indexed choice survives format switching");
    button(window, "Duplicate")->click();
    QApplication::processEvents();
    check(table->model()->rowCount() == 3 && error->text().contains("conflicts"),
          "duplicate immediately reports precedence conflict");
    QTimer::singleShot(0, [] {
        auto* rejection = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        check(rejection != nullptr, "invalid profile rejection missing");
        rejection->button(QMessageBox::Ok)->click();
    });
    action_named(window, "&Save")->trigger();
    check(load_config_file(profile.string()) == load_config_file((std::filesystem::path(EXAMPLES_DIR) / "basic.yaml").string()),
          "invalid GUI save preserves original profile");
    button(window, "Delete")->click();
    QApplication::processEvents();
    check(table->model()->rowCount() == 2 && error->text() == "Valid profile", "delete restores validity");
    selectAllControls(window);
    table->setCurrentIndex(table->model()->index(0, 0));
    QApplication::processEvents();
    addAction(window, 0, 3);
    {
        auto* editor = named<QGroupBox>(window, "inlineActionEditor");
        auto* outputButtons = named<QComboBox>(*editor, "actionButton");
        check(outputButtons->count() == joystick_button_count && outputButtons->currentData() == 3,
              "virtual output selector contains exactly the preset buttons");
        named<QComboBox>(*editor, "actionButtonFormat")->setCurrentIndex(1);
        named<QSpinBox>(*editor, "actionButtonCode")->setValue(BTN_TRIGGER);
        check(std::get<ButtonAction>(window.config().bindings[0].actions.back()).code == BTN_TRIGGER,
              "action EV_KEY literal remains editable");
        named<QComboBox>(*editor, "actionButtonFormat")->setCurrentIndex(0);
        check(std::get<ButtonAction>(window.config().bindings[0].actions.back()).code == joystick_button_code(3),
              "output indexed choice survives format switching");
    }
    addAction(window, 3);
    check(window.config().bindings[0].actions.size() == 3, "multiple outputs and mode action");
    check(named<QTableWidget>(window, "actionList")->item(0, 1)->text() == "Immediate",
          "untimed actions show Immediate in the type column");
    auto* timing = named<QCheckBox>(window, "tapHold");
    timing->click();
    check(window.config().bindings[0].tap_hold.has_value(), "convert immediate binding to timed");
    check(named<QSpinBox>(window, "tapDuration")->isVisible() &&
          named<QSpinBox>(window, "holdThreshold")->width() < window.width() / 2,
          "tap duration is always visible and timing fields are compact");
    auto* threshold = named<QSpinBox>(window, "holdThreshold");
    enterControl(threshold);
    moveInsideControl(threshold, 1);
    wheelControl(threshold, -1);
    check(threshold->value() == 200, "entry motion does not arm spin box wheel input");
    auto* detailScroll = threshold->parentWidget();
    while (detailScroll && !qobject_cast<QScrollArea*>(detailScroll)) detailScroll = detailScroll->parentWidget();
    check(detailScroll != nullptr, "timing controls live in a scroll area");
    auto* scrollbar = static_cast<QScrollArea*>(detailScroll)->verticalScrollBar();
    if (scrollbar->maximum() > 0) {
        scrollbar->setValue(0);
        wheelControl(threshold, -1);
        check(scrollbar->value() > 0 && threshold->value() == 200,
              "blocked spin box wheel input scrolls the parent instead");
    }
    auto* spinEditor = threshold->findChild<QLineEdit*>();
    check(spinEditor != nullptr, "spin box has an embedded line edit");
    moveInsideControl(spinEditor);
    wheelControl(threshold, -1);
    check(threshold->value() == 199, "wheel changes spin box after moving across its embedded editor");
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(threshold, &leave);
    enterControl(threshold);
    wheelControl(threshold, -1);
    check(threshold->value() == 199, "leaving and re-entering disarms spin box wheel input");
    QComboBox* kindFilter = nullptr;
    for (auto* combo : window.findChildren<QComboBox*>())
        if (combo->findText("All") >= 0 && combo->findText("EV_KEY") >= 0) kindFilter = combo;
    check(kindFilter != nullptr, "control-type filter available for wheel test");
    kindFilter->setCurrentIndex(0);
    enterControl(kindFilter);
    wheelControl(kindFilter, -1);
    check(kindFilter->currentIndex() == 0, "wheel on a newly entered dropdown does not change selection");
    moveInsideControl(kindFilter);
    wheelControl(kindFilter, -1);
    check(kindFilter->currentIndex() == 1, "wheel changes dropdown after moving inside it");
    kindFilter->setCurrentIndex(0);
    toolButton(window, "About tap pulse duration")->click();
    check(QToolTip::text().contains("Tap outputs"), "tap duration has its own explanation");
    QToolTip::hideText();
    addAction(window, 0, 4); // Hold branch, so both branches can be represented.
    addAction(window, 0, 5);
    auto* actions = named<QTableWidget>(window, "actionList");
    check(actions->columnCount() == 2 && actions->horizontalHeaderItem(0)->text() == "Name" &&
          actions->horizontalHeaderItem(1)->text() == "Type" && actions->rowCount() == 5,
          "tap and hold actions share a two-column list");
    auto* typeChoice = qobject_cast<QComboBox*>(actions->cellWidget(actions->rowCount() - 1, 1));
    check(typeChoice != nullptr && typeChoice->currentText() == "Hold", "new timed action defaults to Hold");
    typeChoice->setCurrentIndex(0);
    QApplication::processEvents();
    check(window.config().bindings[0].tap_hold->tap.size() == 1 &&
          window.config().bindings[0].tap_hold->hold.size() == 4, "both branches edited");
    check(actions->currentRow() == 0 && named<QGroupBox>(window, "inlineActionEditor")->isVisible(),
          "moved action is selected for editing after rebuilding the list");
    named<QComboBox>(*named<QGroupBox>(window, "inlineActionEditor"), "actionButton")->setCurrentIndex(6);
    check(std::get<ButtonAction>(window.config().bindings[0].tap_hold->tap.front()).code == joystick_button_code(7),
          "editing the selected moved action writes to its new branch");
    check(actions->rowCount() == 5 &&
          qobject_cast<QComboBox*>(actions->cellWidget(0, 1))->currentText() == "Tap" &&
          qobject_cast<QComboBox*>(actions->cellWidget(1, 1))->currentText() == "Hold",
          "type column shows both branches together");
    check(window.findChild<QListWidget*>("otherBranchActions") == nullptr,
          "timed actions use one list without a second branch list");
    actions->setCurrentCell(0, 0);
    addAction(window, 0, 6);
    check(window.config().bindings[0].tap_hold->tap.size() == 2 &&
          qobject_cast<QComboBox*>(actions->cellWidget(1, 1))->currentText() == "Tap",
          "adding to a selected Tap row adds another Tap action");
    toolButton(window, "Remove action 2")->click();
    check(window.config().bindings[0].tap_hold->tap.size() == 1 && actions->rowCount() == 5,
          "removing a Tap row leaves Hold actions intact");
    qobject_cast<QComboBox*>(actions->cellWidget(1, 1))->setCurrentIndex(0);
    QApplication::processEvents();
    check(window.config().bindings[0].tap_hold->tap.size() == 2 &&
          window.config().bindings[0].tap_hold->hold.size() == 3,
          "type selector moves an existing Hold action into Tap");
    qobject_cast<QComboBox*>(actions->cellWidget(1, 1))->setCurrentIndex(1);
    QApplication::processEvents();
    check(window.config().bindings[0].tap_hold->tap.size() == 1 &&
          window.config().bindings[0].tap_hold->hold.size() == 4,
          "type selector can move an action back to Hold");
    if (!qEnvironmentVariable("JP_GUI_TIMED_SCREENSHOT").isEmpty())
        check(window.grab().save(qEnvironmentVariable("JP_GUI_TIMED_SCREENSHOT")), "save combined action layout screenshot");
    check(error->text() == "Valid profile", "timed binding valid");
    QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        check(dialog != nullptr, "tap/hold protection message missing");
        dialog->button(QMessageBox::Ok)->click();
    });
    timing->click();
    check(window.config().bindings[0].tap_hold.has_value() && timing->isChecked(),
          "switching off tap/hold cannot silently discard a branch");
    const auto beforeInputChange = window.config().bindings[0];
    QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        check(dialog != nullptr, "input change confirmation missing");
        dialog->button(QMessageBox::Cancel)->click();
    });
    named<QComboBox>(window, "inputKind")->setCurrentIndex(1);
    check(window.config().bindings[0] == beforeInputChange &&
          named<QComboBox>(window, "inputKind")->currentIndex() == 0,
          "canceling an incompatible input change keeps both action branches and the form selection");
    action_named(window, "&Save")->trigger();
    check(load_config_file(profile.string()) == window.config(), "GUI saved edited profile");
    QProcess checkProcess;
    checkProcess.start(CLI_PATH, {"--check", QString::fromStdString(profile.string())});
    check(checkProcess.waitForFinished(10000) && checkProcess.exitCode() == 0,
          "saved profile passes joystick-penguin --check");

    auto modes = (std::filesystem::path(EXAMPLES_DIR) / "modes.yaml").string();
    check(window.openProfile(QString::fromStdString(modes)), "open mixed profile");
    selectAllControls(window);
    table->setCurrentIndex(table->model()->index(2, 0));
    QApplication::processEvents();
    addAction(window, 2, 1);
    check(std::holds_alternative<AxisAction>(window.config().bindings[2].actions.back()), "axis action edited");
    check(error->text() == "Valid profile", "axis action remains valid");
    table->setCurrentIndex(table->model()->index(0, 0));
    QApplication::processEvents();
    named<QComboBox>(window, "inputKind")->setCurrentIndex(2);
    QApplication::processEvents();
    check(window.config().bindings[0].input.kind == ControlKind::HatDirection, "hat direction input edited");
    // Restore a valid profile, changing a hat binding with a hat action.
    window.openProfile(QString::fromStdString((std::filesystem::path(EXAMPLES_DIR) / "controls.yaml").string()));
    named<QComboBox>(window, "controllerBrowser")->setCurrentText("right");
    QApplication::processEvents();
    table->setCurrentIndex(table->model()->index(0, 0));
    QApplication::processEvents();
    addAction(window, 1, ABS_HAT0Y);
    check(std::holds_alternative<HatAction>(window.config().bindings[3].actions.back()), "hat action edited");
    validate_edited_config(window.config());

    const auto large = temporary.path / "large.yaml";
    std::filesystem::copy_file(std::filesystem::path(EXAMPLES_DIR) / "star_citizen.yaml", large);
    const auto originalLarge = load_config_file(large.string());
    check(window.openProfile(QString::fromStdString(large.string())), "open large profile for edit");
    named<QComboBox>(window, "controllerBrowser")->setCurrentText("left");
    QApplication::processEvents();
    selectAllControls(window);
    table->setCurrentIndex(table->model()->index(1, 0));
    QApplication::processEvents();
    named<QTableWidget>(window, "actionList")->setCurrentCell(0, 0);
    QApplication::processEvents();
    check(named<QGroupBox>(window, "inlineActionEditor")->isVisible(), "selecting an action opens its editor");
    {
        auto* editor = named<QGroupBox>(window, "inlineActionEditor");
        for (auto* box : editor->findChildren<QCheckBox*>())
            if (box->text() == "Invert output axis") box->setChecked(false);
    }
    QApplication::processEvents();
    check(window.config().bindings.size() == originalLarge.bindings.size() &&
          !std::get<AxisAction>(window.config().bindings[1].actions[0]).invert,
          "large profile edit preserves remaining bindings");
    validate_edited_config(window.config());
    action_named(window, "&Save")->trigger();
    check(load_config_file(large.string()) == window.config(), "large profile saved intact");
    checkProcess.start(CLI_PATH, {"--check", QString::fromStdString(large.string())});
    check(checkProcess.waitForFinished(10000) && checkProcess.exitCode() == 0,
          "large profile passes joystick-penguin --check");
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    previousMessageHandler = qInstallMessageHandler(captureDisconnectWarnings);
    if (qEnvironmentVariableIsSet("JP_GUI_DARK")) {
        QPalette palette;
        palette.setColor(QPalette::Window, QColor("#29292d"));
        palette.setColor(QPalette::Base, QColor("#202024"));
        palette.setColor(QPalette::Button, QColor("#353539"));
        palette.setColor(QPalette::PlaceholderText, QColor("#aaaaaa"));
        for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) palette.setColor(role, QColor("#eeeeee"));
        app.setPalette(palette);
    }
    try {
        run();
        check(wildcardDisconnectWarnings == 0, "setup selection emitted wildcard disconnect warnings");
        std::cout << "GUI editor tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "GUI editor test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
