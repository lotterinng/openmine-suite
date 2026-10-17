// SPDX-License-Identifier: Apache-2.0
#include "AppContext.h"

#include "openmine/persistence/ProjectFile.h"

#include <QFileInfo>
#include <QLocale>
#include <QTimer>

namespace openmine::app {

QString qstr(const std::string& s) { return QString::fromStdString(s); }
std::string sstr(const QString& s) { return s.toStdString(); }
QString formatNumber(double v, int decimals) { return QString::number(v, 'f', decimals); }

AppContext::AppContext(QObject* parent) : QObject(parent) {}
AppContext::~AppContext() = default;

void AppContext::attach() {
    project_->commands().onChanged = [this] { emit undoStackChanged(); };
    token_ = project_->events().subscribe([this](const ChangeEvent& e) {
        if (e.type == ChangeType::Removed && e.id == selection_.id) {
            selection_ = {};
            emit selectionChanged();
        }
        emit modelChanged(static_cast<int>(e.kind), static_cast<int>(e.type), qstr(e.id.toString()));
        // Coalesce bursts (macros) into one CPM run on the next event-loop turn.
        if (!recalcPending_) {
            recalcPending_ = true;
            QTimer::singleShot(0, this, [this] {
                recalcPending_ = false;
                recalculateSchedule();
            });
        }
    });
    selection_ = {};
    recalculateSchedule();
    emit projectReplaced();
    emit selectionChanged();
    emit undoStackChanged();
}

void AppContext::recalculateSchedule() {
    if (!project_) return;
    schedule_ = scheduling::calculateSchedule(*project_);
    emit scheduleRecalculated();
}

void AppContext::newProject(const QString& name, Date start) {
    closeProject();
    project_ = Project::createNew(sstr(name), start);
    attach();
    report(QStringLiteral("New project '%1' created (schedule start %2).").arg(name, qstr(start.toString())));
}

void AppContext::openProject(const QString& path) {
    auto loaded = persistence::loadProject(std::filesystem::path(path.toStdU16String()));
    closeProject();
    project_ = std::move(loaded);
    attach();
    report(QStringLiteral("Opened %1").arg(QFileInfo(path).fileName()));
}

void AppContext::saveProject(const QString& path) {
    persistence::saveProject(*project_, std::filesystem::path(path.toStdU16String()));
    report(QStringLiteral("Saved %1").arg(QFileInfo(path).fileName()));
}

void AppContext::closeProject() {
    if (!project_) return;
    project_->events().unsubscribe(token_);
    project_->commands().onChanged = nullptr;
    project_.reset();
    selection_ = {};
    schedule_ = {};
    emit projectReplaced();
    emit selectionChanged();
}

void AppContext::select(EntityKind kind, const Uuid& id) {
    Selection s{kind, id};
    if (s == selection_) return;
    selection_ = s;
    emit selectionChanged();
}

} // namespace openmine::app
