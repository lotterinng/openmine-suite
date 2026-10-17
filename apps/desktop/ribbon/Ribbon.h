#pragma once
// SPDX-License-Identifier: Apache-2.0
// Ribbon-style workspace selector: one tab per workspace, grouped tool buttons.
// Tools that are planned but not implemented are shown disabled and say so —
// they never pretend to run.

#include <QHBoxLayout>
#include <QTabWidget>

class QAction;

namespace openmine::app {

class RibbonGroup : public QWidget {
    Q_OBJECT
public:
    RibbonGroup(const QString& title, QWidget* parent = nullptr);
    void addAction(QAction* action);
    /// A disabled button for a tool that is not available in this build.
    void addPlanned(const QString& text, const QString& plannedIn);

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
