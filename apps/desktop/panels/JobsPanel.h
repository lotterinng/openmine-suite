#pragma once
// SPDX-License-Identifier: Apache-2.0
// Background jobs: name, state, progress and stage of each job with a cancel button, elapsed
// time and the result (or error) once finished.

#include "../app/AppContext.h"

#include <QWidget>

class QTreeWidget;

namespace openmine::app {

class JobsPanel : public QWidget {
    Q_OBJECT
public:
    explicit JobsPanel(AppContext& ctx, QWidget* parent = nullptr);
    QTreeWidget* list() const { return list_; }

private:
    void rebuild();
    void refreshProgress(int id);

    AppContext& ctx_;
    QTreeWidget* list_ = nullptr;
};

} // namespace openmine::app
