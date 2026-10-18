// SPDX-License-Identifier: Apache-2.0
#include "JobsPanel.h"

#include "../theme/Theme.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QProgressBar>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace openmine::app {

namespace {
QString stateText(jobs::JobState s) {
    switch (s) {
    case jobs::JobState::Queued: return QObject::tr("Queued");
    case jobs::JobState::Running: return QObject::tr("Running");
    case jobs::JobState::Succeeded: return QObject::tr("Done");
    case jobs::JobState::Failed: return QObject::tr("Failed");
    case jobs::JobState::Cancelled: return QObject::tr("Cancelled");
    }
    return {};
}
QString elapsed(const JobInfo& j) {
    const auto end = j.finished.isValid() ? j.finished : QDateTime::currentDateTime();
    const qint64 ms = j.started.msecsTo(end);
    return ms < 60000 ? QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 1) : QStringLiteral("%1 min").arg(ms / 60000.0, 0, 'f', 1);
}
} // namespace

JobsPanel::JobsPanel(AppContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    auto* bar = new QHBoxLayout;
    auto* cancelAll = new QPushButton(tr("Cancel all"), this);
    auto* clear = new QPushButton(tr("Clear finished"), this);
    bar->addWidget(cancelAll);
    bar->addWidget(clear);
    bar->addStretch(1);
    lay->addLayout(bar);
    list_ = new QTreeWidget(this);
    list_->setColumnCount(6);
    list_->setHeaderLabels({tr("Job"), tr("State"), tr("Progress"), tr("Stage"), tr("Time"), tr("Result")});
    list_->setRootIsDecorated(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(5, QHeaderView::Stretch);
    list_->setColumnWidth(2, 150);
    lay->addWidget(list_, 1);
    connect(cancelAll, &QPushButton::clicked, this, [this] { ctx_.jobs().cancelAll(); });
    connect(clear, &QPushButton::clicked, this, [this] { ctx_.jobs().clearFinished(); });
    connect(&ctx_.jobs(), &JobManager::jobsChanged, this, &JobsPanel::rebuild);
    connect(&ctx_.jobs(), &JobManager::jobProgress, this, &JobsPanel::refreshProgress);
    rebuild();
}

void JobsPanel::rebuild() {
    list_->clear();
    for (const auto& j : ctx_.jobs().jobs()) {
        auto* it = new QTreeWidgetItem(list_);
        it->setText(0, j.name);
        it->setToolTip(0, j.category);
        it->setData(0, Qt::UserRole, j.id);
        it->setText(1, stateText(j.state()));
        it->setText(3, j.handle ? qstr(j.handle->progress->stage()) : QString());
        it->setText(4, elapsed(j));
        it->setText(5, j.resultText);
        it->setToolTip(5, j.resultText);
        if (j.state() == jobs::JobState::Failed) it->setForeground(5, Theme::colors().warning);
        auto* bar = new QProgressBar;
        bar->setRange(0, 1000);
        bar->setValue(j.done() ? (j.state() == jobs::JobState::Succeeded ? 1000 : static_cast<int>(j.handle->progress->fraction() * 1000))
                               : static_cast<int>(j.handle->progress->fraction() * 1000));
        bar->setTextVisible(true);
        bar->setFormat(QStringLiteral("%p%"));
        auto* cell = new QWidget;
        auto* h = new QHBoxLayout(cell);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(bar, 1);
        if (!j.done()) {
            auto* c = new QToolButton;
            c->setIcon(toolIcon("stop"));
            c->setToolTip(tr("Cancel — nothing is applied to the project"));
            const int id = j.id;
            connect(c, &QToolButton::clicked, this, [this, id] { ctx_.jobs().cancel(id); });
            h->addWidget(c);
        }
        list_->setItemWidget(it, 2, cell);
    }
}

void JobsPanel::refreshProgress(int id) {
    const auto* j = ctx_.jobs().find(id);
    if (!j) return;
    for (int i = 0; i < list_->topLevelItemCount(); ++i) {
        auto* it = list_->topLevelItem(i);
        if (it->data(0, Qt::UserRole).toInt() != id) continue;
        it->setText(1, stateText(j->state()));
        it->setText(3, qstr(j->handle->progress->stage()));
        it->setText(4, elapsed(*j));
        if (auto* cell = list_->itemWidget(it, 2))
            if (auto* bar = cell->findChild<QProgressBar*>()) bar->setValue(static_cast<int>(j->handle->progress->fraction() * 1000));
    }
}

} // namespace openmine::app
