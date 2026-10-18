#pragma once
// SPDX-License-Identifier: Apache-2.0
// Reports stored in the project (Report entities whose file is an immutable asset): list,
// in-application preview (PDF pages through Qt PDF when available, HTML text otherwise), opening
// in the system viewer, saving a copy, and generation of every report type the modules provide —
// design conformance, registration, point-cloud provenance, schedule Gantt (PDF) and workbooks
// (XLSX), development quantities and grade–tonnage tables.

#include "../app/AppContext.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QListWidget;
class QStackedWidget;
class QTextBrowser;

namespace openmine::app {

class ReportsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ReportsPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Report kinds that can be generated now (depends on the selection and the project).
    QStringList availableKinds() const;
    /// Generates and stores a report; returns its id (nil on failure, reported).
    Uuid generate(const QString& kind, const QString& format = QStringLiteral("pdf"));
    void preview(const Uuid& reportId);
    bool openExternally(const Uuid& reportId);
    bool saveCopy(const Uuid& reportId, const QString& path);
    /// Writes the report file to a temporary location and returns its path.
    QString materialise(const Uuid& reportId) const;

private:
    void rebuild();
    void updateKinds();

    AppContext& ctx_;
    QListWidget* list_ = nullptr;
    QComboBox* kind_ = nullptr;
    QComboBox* format_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QTextBrowser* html_ = nullptr;
    QWidget* pdfView_ = nullptr;
    QLabel* info_ = nullptr;
    void* pdfDocument_ = nullptr;
};

} // namespace openmine::app
