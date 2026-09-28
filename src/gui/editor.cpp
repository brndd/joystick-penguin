#include "editor.hpp"
#include "control_browser.hpp"
#include "mapping_workspace.hpp"
#include "profile_document.hpp"
#include "setup_workspace.hpp"

#include <QAction>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QEnterEvent>
#include <QEvent>
#include <QFileDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QStatusBar>
#include <QSpinBox>
#include <QStyle>
#include <QTabWidget>
#include <QToolBar>

using namespace joystick_penguin;

namespace {
QString qs(const std::string& value) { return QString::fromStdString(value); }
std::string str(const QString& value) { return value.toStdString(); }
}

struct EditorWindow::State {
    ProfileDocument document;
    MappingWorkspace* mappings = nullptr;
    QLabel* error = nullptr;
    QTabWidget* workspaces = nullptr;
    QAction* save = nullptr;
    QAction *undo = nullptr, *redo = nullptr;
    QListWidget* issues = nullptr;
    SetupWorkspace *devices = nullptr, *conditions = nullptr;
};

EditorWindow::EditorWindow(QWidget* parent) : QMainWindow(parent), state_(std::make_unique<State>()) {
    auto& s = *state_;
    resize(1600, 900);
    qApp->installEventFilter(this);

    // Profile commands and their toolbar shortcuts.
    auto* newAction = new QAction("&New", this);
    connect(newAction, &QAction::triggered, this, [this] { if (confirmDiscard()) newProfile(); });
    newAction->setShortcut(QKeySequence::New);
    auto* openAction = new QAction("&Open…", this);
    connect(openAction, &QAction::triggered, this, [this] {
        if (!confirmDiscard()) return;
        const auto path = QFileDialog::getOpenFileName(this, "Open profile", {}, "YAML profiles (*.yaml *.yml);;All files (*)");
        if (!path.isEmpty()) openProfile(path);
    });
    openAction->setShortcut(QKeySequence::Open);
    auto* saveAction = new QAction("&Save", this);
    s.save = saveAction;
    connect(saveAction, &QAction::triggered, this, [this] { saveProfile(false); });
    saveAction->setShortcut(QKeySequence::Save);
    auto* saveAsAction = new QAction("Save &As…", this);
    connect(saveAsAction, &QAction::triggered, this, [this] { saveProfile(true); });
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    auto* quitAction = new QAction("&Quit", this);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);
    quitAction->setShortcut(QKeySequence::Quit);
    addAction(quitAction);
    auto* toolbar = addToolBar("Profile");
    toolbar->setObjectName("profileToolbar");
    toolbar->setContextMenuPolicy(Qt::NoContextMenu);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setMovable(false);
    const QList<QAction*> files{newAction, openAction, saveAction, saveAsAction};
    const QStringList icons{"document-new", "document-open", "document-save", "document-save-as"};
    for (int i = 0; i < files.size(); ++i) {
        files[i]->setIcon(QIcon::fromTheme(icons[i], style()->standardIcon(i == 1 ? QStyle::SP_DialogOpenButton : i == 0 ? QStyle::SP_FileIcon : QStyle::SP_DialogSaveButton)));
        files[i]->setToolTip(files[i]->text().remove('&') + " (" + files[i]->shortcut().toString() + ")");
        addAction(files[i]);
        toolbar->addAction(files[i]);
        toolbar->widgetForAction(files[i])->setAccessibleName(files[i]->text().remove('&'));
    }
    toolbar->addSeparator();
    auto historyMove = [this](bool redo) {
        auto& s = *state_;
        if (redo ? !s.document.canRedo() : !s.document.canUndo()) return;
        const int selected = s.mappings->selectedBinding();
        s.mappings->closeActionEditor();
        if (redo) s.document.redo(); else s.document.undo();
        s.mappings->refreshFilters();
        s.mappings->selectBinding(selected < s.mappings->bindingCount() ? selected : -1);
        s.devices->reset(); s.conditions->reset();
        validateProfile(); updateTitle();
    };
    auto navigationIcon = [this](const QString& name, QStyle::StandardPixmap fallback) {
        QPixmap image = style()->standardIcon(fallback).pixmap(64, 64);
        QPainter painter(&image);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(image.rect(), palette().color(QPalette::WindowText));
        painter.end();
        return QIcon::fromTheme(name, QIcon(image));
    };
    s.undo = new QAction(navigationIcon("edit-undo", QStyle::SP_ArrowBack), "&Undo", this);
    connect(s.undo, &QAction::triggered, this, [historyMove] { historyMove(false); });
    s.undo->setShortcut(QKeySequence::Undo);
    s.redo = new QAction(navigationIcon("edit-redo", QStyle::SP_ArrowForward), "&Redo", this);
    connect(s.redo, &QAction::triggered, this, [historyMove] { historyMove(true); });
    s.redo->setShortcut(QKeySequence::Redo);
    addAction(s.undo); addAction(s.redo);
    toolbar->addAction(s.undo); toolbar->addAction(s.redo);
    toolbar->widgetForAction(s.undo)->setAccessibleName("Undo");
    toolbar->widgetForAction(s.redo)->setAccessibleName("Redo");

    // Workspaces share one document; only navigation and file commands live here.
    s.workspaces = new QTabWidget(this);
    setCentralWidget(s.workspaces);
    s.mappings = new MappingWorkspace(s.document, s.workspaces);
    s.workspaces->addTab(s.mappings, "&Mappings");
    s.mappings->changed = [this] { validateProfile(); updateTitle(); };
    s.mappings->requestDeviceSetup = [this](bool virtualDevice) {
        state_->workspaces->setCurrentIndex(1);
        state_->devices->requestAdd(virtualDevice);
    };
    s.document.observe([this](ProfileDocument::Change change, int row, bool before) {
        state_->mappings->modelChange(change, row, before);
    });
    s.devices = new SetupWorkspace(s.document.config(), true, this);
    s.conditions = new SetupWorkspace(s.document.config(), false, this);
    s.workspaces->addTab(s.devices, "&Devices");
    s.workspaces->addTab(s.conditions, "Modes && modifiers");
    for (auto* workspace : {s.devices, s.conditions}) {
        workspace->committed = [this](Config config) {
            auto& s = *state_;
            const int selected = s.mappings->selectedBinding();
            if (!s.document.setup(std::move(config))) return;
            s.mappings->closeActionEditor();
            s.mappings->refreshFilters();
            s.mappings->selectBinding(selected < s.mappings->bindingCount() ? selected : -1);
            s.mappings->refreshBinding();
        };
        workspace->showControl = [this](Control input) {
            state_->workspaces->setCurrentIndex(0);
            state_->mappings->showControl(input);
        };
        workspace->showMapping = [this](int row) {
            state_->workspaces->setCurrentIndex(0);
            state_->mappings->showMapping(row);
        };
        workspace->error = [this](QString message) { showStatus(message); };
    }
    connect(s.workspaces, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 1) state_->devices->refresh();
        if (index == 2) state_->conditions->refresh();
        if (index == 0) state_->mappings->refreshControls();
    });
    auto* findAction = new QAction("&Find mappings", this);
    connect(findAction, &QAction::triggered, this, [this] {
        state_->workspaces->setCurrentIndex(0); state_->mappings->focusSearch();
    });
    findAction->setShortcut(QKeySequence::Find);
    addAction(findAction);

    // Issues are linked back to source binding rows by the mapping workspace.
    s.error = new QLabel(this);
    s.error->setObjectName("validationError");
    s.error->setTextInteractionFlags(Qt::TextSelectableByMouse);
    s.error->setToolTip("Click to open the Issues tab");
    s.error->installEventFilter(this);
    statusBar()->addPermanentWidget(s.error, 1);
    s.issues = new QListWidget(this);
    s.issues->setObjectName("profileIssues");
    s.workspaces->addTab(s.issues, "Issues");
    connect(s.issues, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (!item->data(Qt::UserRole).isValid()) { state_->workspaces->setCurrentIndex(1); return; }
        state_->devices->showMapping(item->data(Qt::UserRole).toInt());
    });
    validateProfile(); updateTitle();
}

EditorWindow::~EditorWindow() {
    qApp->removeEventFilter(this);
    // Views can request data while QWidget tears down. Destroy them before the
    // document referenced by their models, rather than in QMainWindow's base dtor.
    hide();
    delete takeCentralWidget();
}

const Config& EditorWindow::config() const { return state_->document.config(); }

bool EditorWindow::eventFilter(QObject* watched, QEvent* event) {
    if (auto* widget = qobject_cast<QWidget*>(watched); widget && widget->window() == this) {
        QWidget* control = widget;
        while (control && !qobject_cast<QAbstractSpinBox*>(control) && !qobject_cast<QComboBox*>(control))
            control = control->parentWidget();
        if (control && event->type() == QEvent::Enter) {
            widget->setMouseTracking(true);
            if (control == widget || !control->property("wheelEntry").isValid()) {
                control->setProperty("wheelEntry", static_cast<QEnterEvent*>(event)->globalPosition());
                control->setProperty("wheelArmed", false);
            }
        } else if (control && control == widget && event->type() == QEvent::Leave) {
            control->setProperty("wheelEntry", QVariant{});
            control->setProperty("wheelArmed", false);
        } else if (control && event->type() == QEvent::MouseMove) {
            const auto entry = control->property("wheelEntry");
            const auto* move = static_cast<QMouseEvent*>(event);
            if (entry.isValid() &&
                control->rect().contains(control->mapFromGlobal(move->globalPosition().toPoint())) &&
                (move->globalPosition() - entry.toPointF()).manhattanLength() >= 4)
                control->setProperty("wheelArmed", true);
        } else if (control && event->type() == QEvent::Wheel && !control->property("wheelArmed").toBool()) {
            for (auto* parent = control->parentWidget(); parent; parent = parent->parentWidget()) {
                if (auto* scroll = qobject_cast<QAbstractScrollArea*>(parent)) {
                    QApplication::sendEvent(scroll->viewport(), event);
                    break;
                }
            }
            return true;
        }
    }
    if (watched == state_->error && event->type() == QEvent::MouseButtonRelease) {
        if (!state_->document.issues().empty()) state_->workspaces->setCurrentIndex(3);
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void EditorWindow::validateProfile() {
    auto& s = *state_;
    const auto& config = s.document.config();
    s.issues->clear();
    for (const auto& issue : s.document.issues()) {
        if (issue.bindings.empty()) new QListWidgetItem(qs(issue.message), s.issues);
        for (auto row : issue.bindings) {
            auto* item = new QListWidgetItem(QString("Mapping %1 · %2 · %3").arg(row + 1)
                .arg(labeledInput(config, config.bindings[row].input)).arg(qs(issue.message)), s.issues);
            item->setData(Qt::UserRole, static_cast<int>(row));
        }
    }
    if (s.document.issues().empty()) {
        s.error->setText("Valid profile");
        s.error->setToolTip({});
        s.error->setStyleSheet({});
        s.mappings->setTableError(-1, {});
    } else {
        const auto& first = s.document.issues().front();
        const QString message = qs(first.message);
        s.error->setText(message.left(100) + (message.size() > 100 ? "… · See Issues" : " · See Issues"));
        s.error->setToolTip(message);
        s.error->setStyleSheet("color: #b00020;");
        s.mappings->setTableError(first.bindings.empty() ? -1 : static_cast<int>(first.bindings.front()), message);
    }
    QString detailIssue;
    for (const auto& issue : s.document.issues())
        for (auto row : issue.bindings)
            if (static_cast<int>(row) == s.mappings->selectedBinding()) detailIssue = qs(issue.message);
    s.mappings->setIssue(detailIssue);
}

void EditorWindow::showStatus(const QString& message) {
    state_->error->setText(message.isEmpty() ? "Valid profile" : message);
    state_->error->setToolTip(message);
    state_->error->setStyleSheet(message.isEmpty() ? QString{} : "color: #b00020;");
}

void EditorWindow::updateTitle() {
    const auto& s = *state_;
    const bool unsaved = s.document.dirty();
    setWindowTitle(QString("%1%2 — Joystick Penguin Profile Editor")
                        .arg(s.document.path().isEmpty() ? "Untitled" : s.document.path()).arg(unsaved ? " *" : ""));
    s.save->setEnabled(unsaved);
    s.undo->setEnabled(s.document.canUndo()); s.redo->setEnabled(s.document.canRedo());
}

QMenu* EditorWindow::createPopupMenu() { return nullptr; }

void EditorWindow::newProfile() {
    auto& s = *state_;
    s.document.replace(ProfileDocument{}.config());
    s.mappings->resetBrowsing();
    validateProfile(); updateTitle();
    s.devices->reset(); s.conditions->reset(); s.workspaces->setCurrentIndex(0);
}

bool EditorWindow::openProfile(const QString& path) {
    try {
        auto loaded = load_config_file(str(path));
        auto& s = *state_;
        s.mappings->closeActionEditor();
        s.document.replace(std::move(loaded), path);
        s.mappings->resetBrowsing(true);
        validateProfile(); updateTitle();
        s.devices->reset(); s.conditions->reset(); s.workspaces->setCurrentIndex(0);
        return true;
    } catch (const ConfigError& error) {
        QMessageBox::critical(this, "Cannot open profile", qs(error.what()));
        return false;
    }
}

bool EditorWindow::saveProfile(bool as) {
    auto& s = *state_;
    QString path = s.document.path();
    if (as || path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, "Save profile", path, "YAML profiles (*.yaml *.yml);;All files (*)");
        if (path.isEmpty()) return false;
    }
    try {
        save_config_file(s.document.config(), str(path));
        s.document.saved(path);
        updateTitle();
        return true;
    } catch (const ConfigError& error) {
        const QString message = qs(error.what());
        validateProfile();
        QMessageBox::warning(this, "Profile is not saved", message);
        return false;
    }
}

bool EditorWindow::confirmDiscard() {
    if (!state_->document.dirty()) return true;
    const auto result = QMessageBox::warning(this, "Unsaved changes", "Save changes to this profile?",
                                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (result == QMessageBox::Cancel) return false;
    if (result == QMessageBox::Save) return saveProfile(false);
    return true;
}

void EditorWindow::closeEvent(QCloseEvent* event) {
    if (confirmDiscard()) event->accept();
    else event->ignore();
}
