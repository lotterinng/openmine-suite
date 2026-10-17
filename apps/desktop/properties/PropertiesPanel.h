#pragma once
// SPDX-License-Identifier: Apache-2.0

#include "../AppContext.h"

#include <QStringList>
#include <QTableWidget>

namespace openmine::app {

/// Shows and edits the selected entity. Every edit is validated and committed as an
/// undoable command; invalid input is rejected with a message and the old value restored.
class PropertiesPanel : public QTableWidget {
    Q_OBJECT
public:
    explicit PropertiesPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Programmatic edit of a property by key (used by the console and the smoke test).
    /// Returns false (and reports why) if the edit was rejected.
    bool setProperty(const QString& key, const QString& value);
    /// Current displayed value for a key (empty if absent).
    QString value(const QString& key) const;
    QStringList keys() const;
    /// Re-read the selection and theme colours.
    void refresh() { rebuild(); }

private:
    enum class Kind { ReadOnly, Text, Number, Bool, Choice, Date };
    struct Row {
        QString key, label, value;
        Kind kind = Kind::ReadOnly;
        QStringList choices;
        QString tooltip;
    };
    void rebuild();
    void scheduleRebuild();
    void section(const QString& title);
    void row(const QString& key, const QString& label, const QString& value, Kind kind = Kind::ReadOnly,
             const QStringList& choices = {}, const QString& tooltip = {});
    void readOnly(const QString& label, const QString& value, const QString& tooltip = {});
    bool commit(const QString& key, const QString& value);

    AppContext& ctx_;
    std::vector<Row> rows_;
    bool building_ = false;
    bool pending_ = false;
};

} // namespace openmine::app
