#pragma once
// SPDX-License-Identifier: Apache-2.0
// Ribbon-style workspace selector: one tab per workspace, grouped tool buttons (large buttons
// for primary tools, stacked small buttons for secondary ones). Actions that cannot run in the
// current state or build are disabled by their owner and say why in their tooltip.

#include <QHBoxLayout>
#include <QTabWidget>

class QAction;

namespace openmine::app {

class RibbonGroup : public QWidget {
    Q_OBJECT
public:
    RibbonGroup(const QString& title, QWidget* parent = nullptr);
    void addAction(QAction* action);
    /// Small buttons stacked three per column.
    void addSmall(const QList<QAction*>& actions);

private:
    QHBoxLayout* buttons_;
};

class RibbonPage : public QWidget {
    Q_OBJECT
public:
    explicit RibbonPage(QWidget* parent = nullptr);
    RibbonGroup* addGroup(const QString& title);
    void finish();

private:
    QHBoxLayout* layout_;
};

class Ribbon : public QTabWidget {
    Q_OBJECT
public:
    explicit Ribbon(QWidget* parent = nullptr);
    RibbonPage* addPage(const QString& workspace);
    RibbonPage* page(const QString& workspace) const;
};

} // namespace openmine::app
