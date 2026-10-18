#pragma once
// SPDX-License-Identifier: Apache-2.0
// Project data services: coordinate reference systems (search the PROJ database, declare, set
// the project CRS — which never transforms coordinates), local mine grids fitted to control
// pairs, explicit transformation of objects into copies with a transformation record, the plugin
// manager, and the shared PostgreSQL/PostGIS repository (publish, pull, history, locks).

#include "../app/AppContext.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class QTreeWidget;

namespace openmine::app {

class CrsDialog : public QDialog {
    Q_OBJECT
public:
    explicit CrsDialog(AppContext& ctx, QWidget* parent = nullptr);
    static bool available();
    /// Declares a CRS by definition (EPSG code, WKT or PROJ string); returns its id.
    Uuid declare(const QString& definition, bool makeProjectCrs);

private:
    void search();
    AppContext& ctx_;
    QLineEdit* query_;
    QTableWidget* results_;
    QLabel* details_;
    QCheckBox* setProject_;
};

class LocalGridDialog : public QDialog {
    Q_OBJECT
public:
    explicit LocalGridDialog(AppContext& ctx, QWidget* parent = nullptr);
    bool fit();
    Uuid store();

private:
    AppContext& ctx_;
    QLineEdit* name_;
    QComboBox* base_;
    QComboBox* method_;
    QTableWidget* pairs_;
    QLabel* result_;
};

class TransformDialog : public QDialog {
    Q_OBJECT
public:
    explicit TransformDialog(AppContext& ctx, QWidget* parent = nullptr);
    std::vector<Uuid> apply();

private:
    void operations();
    AppContext& ctx_;
    QComboBox* source_;
    QComboBox* target_;
    QComboBox* operation_;
    QComboBox* layer_;
    QCheckBox* ballpark_;
    QLabel* info_;
};

class PluginsDialog : public QDialog {
    Q_OBJECT
public:
    explicit PluginsDialog(AppContext& ctx, QWidget* parent = nullptr);
    void refresh();
    bool runCommand(const QString& name, const QString& argumentsJson);

private:
    AppContext& ctx_;
    QTreeWidget* list_;
    QComboBox* command_;
    QLineEdit* args_;
    QPlainTextEdit* log_;
};

class RepositoryDialog : public QDialog {
    Q_OBJECT
public:
    explicit RepositoryDialog(AppContext& ctx, QWidget* parent = nullptr);
    static bool available();

private:
    void connectRepo();
    void showStatus();
    AppContext& ctx_;
    QLineEdit* dsn_;
    QLineEdit* author_;
    QLineEdit* message_;
    QComboBox* policy_;
    QPlainTextEdit* log_;
    QTreeWidget* history_;
    std::shared_ptr<void> repo_; ///< repository::PgRepository
};

} // namespace openmine::app
