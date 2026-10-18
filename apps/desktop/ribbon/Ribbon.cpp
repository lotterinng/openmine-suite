// SPDX-License-Identifier: Apache-2.0
#include "Ribbon.h"

#include "../theme/Theme.h"

#include <QAction>
#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace openmine::app {

namespace {
QToolButton* makeButton(QWidget* parent) {
    auto* b = new QToolButton(parent);
    b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    b->setIconSize(QSize(28, 28));
    b->setAutoRaise(true);
    b->setMinimumWidth(52);
    return b;
}
} // namespace

RibbonGroup::RibbonGroup(const QString& title, QWidget* parent) : QWidget(parent) {
    auto* frame = new QFrame(this);
    frame->setObjectName("RibbonGroup");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(frame);
    auto* v = new QVBoxLayout(frame);
    v->setContentsMargins(4, 2, 6, 2);
    v->setSpacing(1);
    buttons_ = new QHBoxLayout;
    buttons_->setSpacing(1);
    v->addLayout(buttons_, 1);
    auto* label = new QLabel(title, frame);
    label->setObjectName("RibbonGroupTitle");
    label->setAlignment(Qt::AlignHCenter);
    v->addWidget(label);
}

void RibbonGroup::addAction(QAction* action) {
    auto* b = makeButton(this);
    b->setDefaultAction(action);
    buttons_->addWidget(b);
}

void RibbonGroup::addSmall(const QList<QAction*>& actions) {
    QVBoxLayout* col = nullptr;
    for (int i = 0; i < actions.size(); ++i) {
        if (i % 3 == 0) {
            col = new QVBoxLayout;
            col->setSpacing(0);
            buttons_->addLayout(col);
        }
        auto* b = new QToolButton(this);
        b->setDefaultAction(actions[i]);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setIconSize(QSize(16, 16));
        b->setAutoRaise(true);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        col->addWidget(b);
    }
    if (col)
        while (col->count() < 3) col->addStretch(1);
}

RibbonPage::RibbonPage(QWidget* parent) : QWidget(parent) {
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(4, 2, 4, 2);
    layout_->setSpacing(2);
}

RibbonGroup* RibbonPage::addGroup(const QString& title) {
    auto* g = new RibbonGroup(title, this);
    layout_->addWidget(g);
    return g;
}

void RibbonPage::finish() { layout_->addStretch(1); }

Ribbon::Ribbon(QWidget* parent) : QTabWidget(parent) {
    setObjectName("Ribbon");
    setDocumentMode(true);
    setFixedHeight(104);
}

RibbonPage* Ribbon::addPage(const QString& workspace) {
    // Pages scroll horizontally when the window is narrower than their tools.
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* p = new RibbonPage(scroll);
    p->setObjectName(workspace);
    scroll->setWidget(p);
    addTab(scroll, workspace);
    return p;
}

RibbonPage* Ribbon::page(const QString& workspace) const {
    for (int i = 0; i < count(); ++i)
        if (tabText(i) == workspace)
            if (auto* s = qobject_cast<QScrollArea*>(widget(i))) return qobject_cast<RibbonPage*>(s->widget());
    return nullptr;
}

} // namespace openmine::app
