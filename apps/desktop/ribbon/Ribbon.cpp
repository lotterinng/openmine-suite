// SPDX-License-Identifier: Apache-2.0
#include "Ribbon.h"

#include "../theme/Theme.h"

#include <QAction>
#include <QFrame>
#include <QLabel>
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

void RibbonGroup::addPlanned(const QString& text, const QString& plannedIn) {
    auto* b = makeButton(this);
    b->setText(text);
    b->setIcon(toolIcon(text));
    b->setEnabled(false);
    b->setToolTip(QStringLiteral("<b>%1</b><br>Not available in this build.<br>Planned: %2").arg(text, plannedIn));
    // Disabled widgets do not show tooltips by default; keep the explanation reachable.
    b->setAttribute(Qt::WA_AlwaysShowToolTips);
    b->setProperty("planned", true);
    buttons_->addWidget(b);
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
    auto* p = new RibbonPage(this);
    p->setObjectName(workspace);
    addTab(p, workspace);
    return p;
}

RibbonPage* Ribbon::page(const QString& workspace) const {
    for (int i = 0; i < count(); ++i)
        if (tabText(i) == workspace) return qobject_cast<RibbonPage*>(widget(i));
    return nullptr;
}

} // namespace openmine::app
