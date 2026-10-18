#pragma once
// SPDX-License-Identifier: Apache-2.0
// Property editor for every entity type, driven by the entity documents of the persistence layer
// (so it covers all columns of all types without per-type forms). Shows:
//   * the document's columns with editors chosen by field metadata (text, numbers with units,
//     booleans, enumerations, references to other entities, dates, colours, JSON);
//   * derived, read-only values computed by the modules (lengths, volumes and tonnes, metrics of
//     stopes and block models, point-cloud provenance, the activity's CPM timings, …);
//   * custom attributes validated against the attribute definitions;
//   * a vertex table for curves.
// Several selected entities of one type are edited together (differing values shown as
// “(varies)”); every edit is one validated undo step. Fields that protect survey data, computed
// results, approved designs and baselines are read-only and say why.

#include "../app/AppContext.h"

#include <QWidget>

#include <nlohmann/json.hpp>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QTableWidget;
class QLineEdit;
class QPushButton;

namespace openmine::app {

class PropertiesPanel : public QWidget {
    Q_OBJECT
public:
    explicit PropertiesPanel(AppContext& ctx, QWidget* parent = nullptr);

    /// Programmatic edit of a column of the selected entities (console, smoke test). Values are
    /// given as text and converted by the field's kind. Returns false (and reports why) if rejected.
    bool setField(const QString& column, const QString& value);
    /// Displayed value of a column or derived quantity (by label) of the primary selection.
    QString value(const QString& columnOrLabel) const;
    QStringList columns() const;
    void refresh() { rebuild(); }

private:
    void scheduleRebuild();
    void rebuild();
    void buildFields(const std::string& type, const std::vector<nlohmann::json>& docs);
    void buildDerived(const SelectionItem& item);
    void buildAttributes(const std::string& type, const std::vector<nlohmann::json>& docs);
    void buildVertices(const GeometryObject& g);
    QTreeWidgetItem* section(const QString& title);
    void addReadOnly(QTreeWidgetItem* parent, const QString& label, const QString& value, const QString& tooltip = {});
    bool commit(const std::string& column, const nlohmann::json& value, const QString& what);
    bool commitAttribute(const std::string& key, const std::optional<std::string>& value);
    bool commitVertices();
    QWidget* editorFor(const std::string& type, const std::string& column, const nlohmann::json& value, bool varies);
    std::vector<SelectionItem> editedItems() const;

    AppContext& ctx_;
    QLabel* header_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QTableWidget* vertices_ = nullptr;
    QWidget* vertexBox_ = nullptr;
    std::string type_;
    std::vector<nlohmann::json> docs_;
    std::map<QString, QString> displayed_;   ///< column/label → shown value (for value())
    bool pending_ = false;
    bool building_ = false;
};

} // namespace openmine::app
