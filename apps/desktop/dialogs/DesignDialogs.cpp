// SPDX-License-Identifier: Apache-2.0
#include "DesignDialogs.h"

#include "DialogUtil.h"
#include "../app/EntityInfo.h"
#include "../viewport/Tools.h"

#include "openmine/geometry/Curves.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/mine_design/Headings.h"
#include "openmine/mine_design/Revisions.h"
#include "openmine/mine_design/Stopes.h"
#include "openmine/objects/EnumNames.h"
#include "openmine/project/ProjectEditor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <map>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>

namespace openmine::app {

namespace {
QDialogButtonBox* buttons(QDialog* d, const QString& okText, std::function<bool()> onOk) {
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, d);
    bb->button(QDialogButtonBox::Ok)->setText(okText);
    QObject::connect(bb, &QDialogButtonBox::accepted, d, [d, onOk] {
        if (onOk()) d->accept();
    });
    QObject::connect(bb, &QDialogButtonBox::rejected, d, &QDialog::reject);
    return bb;
}

bool isCurve(const ProjectModel& m, const Uuid& id, bool closed = false, std::size_t minVertices = 2) {
    const auto* g = m.geometry.find(id);
    if (!g || !geometry::isCurve(g->type)) return false;
    if (closed && !(g->closed || g->type == GeometryType::Circle)) return false;
    return g->vertices.size() >= minVertices || g->type == GeometryType::Circle || g->type == GeometryType::Arc;
}

Uuid firstSelectedCurve(AppContext& ctx) {
    for (const auto& id : ctx.selectedGeometry())
        if (isCurve(ctx.project().model(), id)) return id;
    return {};
}
} // namespace

// ---- heading template ----------------------------------------------------------------------------

HeadingTemplate::HeadingTemplate(AppContext& ctx, const QString& activityType, bool prefix, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    const auto& m = ctx_.project().model();
    auto* form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    id_ = new QLineEdit(this);
    id_->setPlaceholderText(prefix ? tr("prefix — a 2-digit sequence is appended when several headings are created") : tr("e.g. L675_XC03"));
    {
        // Suggest the first unused id for the heading type (the user can type any other).
        static const std::map<QString, QString> abbreviations = {{"Decline", "DEC"},     {"Ramp", "RMP"},          {"Level access", "LA"},
                                                                 {"Crosscut", "XC"},     {"Ore drive", "OD"},       {"Access drive", "AD"},
                                                                 {"Stockpile", "SP"},    {"Sump", "SMP"},           {"Ventilation drive", "VD"}};
        const auto it = abbreviations.find(activityType);
        const QString abbr = it == abbreviations.end() ? QStringLiteral("DEV") : it->second;
        auto used = [&](const QString& candidate) {
            for (const auto& d : m.developments.all())
                if (prefix ? qstr(d.headingId).startsWith(candidate) : qstr(d.headingId) == candidate) return true;
            return false;
        };
        for (int n = 1; n < 10000; ++n) {
            const QString candidate = prefix ? QStringLiteral("%1%2_").arg(abbr).arg(n) : QStringLiteral("%1%2").arg(abbr).arg(n, 2, 10, QChar('0'));
            if (!used(candidate)) {
                id_->setText(candidate);
                break;
            }
        }
    }
    name_ = new QLineEdit(this);
    level_ = new QComboBox(this);
    fillEntityCombo(level_, m, EntityKind::Level, {}, true);
    decline_ = new QComboBox(this);
    fillEntityCombo(decline_, m, EntityKind::Decline, {}, true);
    profile_ = new QComboBox(this);
    fillEntityCombo(profile_, m, EntityKind::Profile, m.profiles.empty() ? Uuid{} : m.profiles.all().front().id, true);
    material_ = new QComboBox(this);
    fillEntityCombo(material_, m, EntityKind::Material, m.materials.empty() ? Uuid{} : m.materials.all().front().id, true);
    support_ = new QComboBox(this);
    fillEntityCombo(support_, m, EntityKind::SupportClass, {}, true);
    zone_ = new QLineEdit(this);
    type_ = new QComboBox(this);
    type_->setEditable(true);
    type_->addItems({"Decline", "Ramp", "Level access", "Crosscut", "Ore drive", "Access drive", "Stockpile", "Sump", "Ventilation drive", "Development"});
    type_->setCurrentText(activityType);
    width_ = spin(0.5, 30, 5.0, 2, " m", this);
    height_ = spin(0.5, 30, 5.0, 2, " m", this);
    chainage_ = spin(-1e6, 1e6, 0, 3, " m", this);
    form->addRow(prefix ? tr("Heading id prefix") : tr("Heading id"), id_);
    form->addRow(tr("Name"), name_);
    form->addRow(tr("Level"), level_);
    form->addRow(tr("Decline"), decline_);
    form->addRow(tr("Profile"), profile_);
    form->addRow(tr("Material"), material_);
    form->addRow(tr("Support class"), support_);
    form->addRow(tr("Zone"), zone_);
    form->addRow(tr("Activity type"), type_);
    form->addRow(tr("Width × height (no profile)"), [&] {
        auto* w = new QWidget(this);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(width_);
        h->addWidget(height_);
        return w;
    }());
    form->addRow(tr("Start chainage"), chainage_);
}

QString HeadingTemplate::idOrPrefix() const { return id_->text().trimmed(); }

mine_design::NewDevelopment HeadingTemplate::value() const {
    const auto& m = ctx_.project().model();
    mine_design::NewDevelopment d;
    d.headingId = sstr(id_->text().trimmed());
    d.name = sstr(name_->text().trimmed());
    d.levelId = comboUuid(level_);
    if (const auto* l = m.levels.find(d.levelId)) d.level = l->code.empty() ? l->name : l->code;
    d.declineId = comboUuid(decline_);
    if (const auto* dc = m.declines.find(d.declineId)) d.declineNumber = dc->number;
    d.profileId = comboUuid(profile_);
    d.materialId = comboUuid(material_);
    d.supportClassId = comboUuid(support_);
    d.zone = sstr(zone_->text().trimmed());
    d.activityType = sstr(type_->currentText());
    d.width = width_->value();
    d.height = height_->value();
    d.chainageFrom = chainage_->value();
    return d;
}

// ---- layouts ----------------------------------------------------------------------------------------

LayoutDialog::LayoutDialog(AppContext& ctx, ToolController& tools, Kind kind, Preview preview, QWidget* parent)
    : QDialog(parent), ctx_(ctx), tools_(tools), kind_(kind), preview_(std::move(preview)) {
    const char* titles[] = {"Decline designer", "Ramp between levels", "Alignment through PIs", "Ore drive along a contact", "Access drive"};
    setWindowTitle(tr(titles[static_cast<int>(kind)]));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(560, 760);
    auto* lay = new QVBoxLayout(this);
    auto* geo = new QGroupBox(tr("Layout"), this);
    auto* form = new QFormLayout(geo);
    const auto& m = ctx_.project().model();
    auto pointRow = [&](QDoubleSpinBox*& x, QDoubleSpinBox*& y, QDoubleSpinBox*& z, const QString& label, const QString& prompt) {
        x = spin(-1e8, 1e8, 0, 3, {}, this);
        y = spin(-1e8, 1e8, 0, 3, {}, this);
        z = spin(-1e5, 1e5, 0, 3, {}, this);
        auto* w = new QWidget(this);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(x);
        h->addWidget(y);
        h->addWidget(z);
        auto* pick = new QPushButton(tr("Pick"), w);
        h->addWidget(pick);
        connect(pick, &QPushButton::clicked, this, [this, x, y, z, prompt] {
            QPointer<LayoutDialog> self(this);
            tools_.requestPoint(prompt, [self, x, y, z](Vec3 p) {
                if (!self) return;
                x->setValue(p.x);
                y->setValue(p.y);
                z->setValue(p.z);
                self->raise();
                self->updatePreview();
            });
        });
        form->addRow(label, w);
    };
    const bool needsStart = kind == Kind::Decline || kind == Kind::Ramp || kind == Kind::AccessDrive;
    if (needsStart) pointRow(sx_, sy_, sz_, tr("Start (X, Y, Z)"), tr("LAYOUT: pick the start point"));
    else {
        sx_ = sy_ = sz_ = nullptr;
    }
    if (needsStart) {
        azimuth_ = spin(0, 360, 0, 2, "°", this);
        azimuth_->setToolTip(tr("Bearing of the first leg, clockwise from grid north (+Y)"));
        form->addRow(tr("Start bearing"), azimuth_);
    }
    if (kind == Kind::AccessDrive) pointRow(tx_, ty_, tz_, tr("Target (X, Y, Z)"), tr("ACCESS: pick the target point"));
    if (kind == Kind::Alignment || kind == Kind::OreDrive) {
        source_ = new QComboBox(this);
        fillEntityCombo(source_, m, EntityKind::Geometry, firstSelectedCurve(ctx_), false,
                        [&m, kind](const Uuid& id) { return isCurve(m, id, false, kind == Kind::Alignment ? 3 : 2); });
        form->addRow(kind == Kind::Alignment ? tr("PI polyline") : tr("Contact string"), source_);
    }
    if (kind == Kind::Decline) {
        style_ = new QComboBox(this);
        style_->addItem(tr("Switchback (straights + 180° turns)"), 0);
        style_->addItem(tr("Spiral (helix)"), 1);
        form->addRow(tr("Style"), style_);
    }
    if (kind == Kind::Decline || kind == Kind::Ramp || kind == Kind::Alignment || kind == Kind::OreDrive) {
        grade_ = spin(-50, 50, kind == Kind::OreDrive ? 1.0 : (kind == Kind::Alignment ? -1.0 : -14.3), 2, " %", this);
        grade_->setToolTip(tr("Negative = down. 1 in 7 = −14.3 %, 1 in 8 = −12.5 %"));
        form->addRow(tr("Grade"), grade_);
    }
    if (kind == Kind::AccessDrive) {
        grade_ = spin(0.1, 50, 15, 2, " %", this);
        form->addRow(tr("Max grade"), grade_);
    }
    if (kind == Kind::Decline || kind == Kind::Ramp) {
        straight_ = spin(0, 2000, kind == Kind::Decline ? 80 : 40, 2, " m", this);
        form->addRow(tr("Straight length (0 = spiral)"), straight_);
        targetZ_ = spin(-1e5, 1e5, -100, 3, " m", this);
        form->addRow(tr("Target elevation"), targetZ_);
        turn_ = new QComboBox(this);
        turn_->addItem(tr("Right"), static_cast<int>(mine_design::TurnDirection::Right));
        turn_->addItem(tr("Left"), static_cast<int>(mine_design::TurnDirection::Left));
        form->addRow(tr("Turn"), turn_);
    }
    if (kind != Kind::OreDrive) {
        radius_ = spin(1, 1000, kind == Kind::Alignment ? 20 : 25, 2, " m", this);
        form->addRow(tr("Curve radius"), radius_);
    }
    if (kind == Kind::Alignment) {
        useZ_ = new QCheckBox(tr("Use the PIs' own elevations instead of a constant grade"), this);
        form->addRow(useZ_);
    }
    if (kind == Kind::OreDrive) {
        offset_ = spin(0, 500, 5, 2, " m", this);
        turn_ = new QComboBox(this);
        turn_->addItem(tr("Left of the contact"), static_cast<int>(mine_design::Side::Left));
        turn_->addItem(tr("Right of the contact"), static_cast<int>(mine_design::Side::Right));
        useZ_ = new QCheckBox(tr("Follow the contact's elevations"), this);
        form->addRow(tr("Offset"), offset_);
        form->addRow(tr("Side"), turn_);
        form->addRow(useZ_);
    }
    // Continue from the end of a selected heading or curve (position and bearing).
    if (sx_) {
        std::vector<Vec3> line;
        for (const auto& s : ctx_.selection()) {
            if (const auto* d = m.developments.find(s.id)) line = mine_design::centrelinePoints(ctx_.project(), *d);
            else if (const auto* g = m.geometry.find(s.id); g && geometry::isCurve(g->type)) line = geometry::curvePolyline(*g);
            if (line.size() >= 2) break;
        }
        if (line.size() >= 2) {
            const Vec3 e = line.back(), p = line[line.size() - 2];
            sx_->setValue(e.x);
            sy_->setValue(e.y);
            sz_->setValue(e.z);
            double brg = std::atan2(e.x - p.x, e.y - p.y) * 180.0 / M_PI;
            if (brg < 0) brg += 360;
            azimuth_->setValue(brg);
            if (targetZ_) targetZ_->setValue(e.z - 50);
        }
    }
    lay->addWidget(geo);
    info_ = new QLabel(this);
    info_->setWordWrap(true);
    lay->addWidget(info_);
    auto* tg = new QGroupBox(tr("Headings"), this);
    auto* tl = new QVBoxLayout(tg);
    const char* types[] = {"Decline", "Ramp", "Development", "Ore drive", "Access drive"};
    templ_ = new HeadingTemplate(ctx_, types[static_cast<int>(kind)], true, tg);
    tl->addWidget(templ_);
    lay->addWidget(tg);
    auto* bb = buttons(this, tr("Create headings"), [this] { return !create().empty(); });
    auto* pv = bb->addButton(tr("Preview"), QDialogButtonBox::ActionRole);
    connect(pv, &QPushButton::clicked, this, &LayoutDialog::updatePreview);
    connect(this, &QDialog::finished, this, [this] {
        if (preview_) preview_({});
    });
    lay->addWidget(bb);
    for (auto* s : findChildren<QDoubleSpinBox*>()) connect(s, &QDoubleSpinBox::editingFinished, this, &LayoutDialog::updatePreview);
    for (auto* c : findChildren<QComboBox*>()) connect(c, &QComboBox::activated, this, &LayoutDialog::updatePreview);
}

std::vector<mine_design::LayoutResult> LayoutDialog::layouts() const {
    const auto& m = ctx_.project().model();
    const Vec3 start = sx_ ? Vec3{sx_->value(), sy_->value(), sz_->value()} : Vec3{};
    switch (kind_) {
    case Kind::Decline: {
        if (style_->currentData().toInt() == 1)
            return {mine_design::spiralDecline(start, azimuth_->value(), grade_->value(), radius_->value(),
                                               static_cast<mine_design::TurnDirection>(turn_->currentData().toInt()), targetZ_->value())};
        mine_design::DeclineSpec s;
        s.start = start;
        s.startAzimuthDeg = azimuth_->value();
        s.gradePercent = grade_->value();
        s.straightLength = straight_->value();
        s.curveRadius = radius_->value();
        s.turn = static_cast<mine_design::TurnDirection>(turn_->currentData().toInt());
        s.targetElevation = targetZ_->value();
        return {mine_design::declineLayout(s)};
    }
    case Kind::Ramp: {
        mine_design::RampSpec s;
        s.start = start;
        s.startAzimuthDeg = azimuth_->value();
        s.targetElevation = targetZ_->value();
        s.gradePercent = grade_->value();
        s.radius = radius_->value();
        s.turn = static_cast<mine_design::TurnDirection>(turn_->currentData().toInt());
        s.straightLength = straight_->value();
        return {mine_design::rampLayout(s)};
    }
    case Kind::Alignment: {
        const auto* g = m.geometry.find(comboUuid(source_));
        if (!g) throw ValidationError("Choose the polyline of points of intersection");
        return {mine_design::horizontalAlignment(g->vertices, radius_->value(), grade_->value(), useZ_->isChecked())};
    }
    case Kind::OreDrive: {
        const auto* g = m.geometry.find(comboUuid(source_));
        if (!g) throw ValidationError("Choose the contact string");
        mine_design::OreDriveSpec s;
        s.offset = offset_->value();
        s.side = static_cast<mine_design::Side>(turn_->currentData().toInt());
        s.gradePercent = grade_->value();
        s.followContactElevation = useZ_->isChecked();
        return {mine_design::oreDriveLayout(geometry::curvePolyline(*g), s)};
    }
    case Kind::AccessDrive:
        return {mine_design::accessDriveLayout(start, azimuth_->value(), {tx_->value(), ty_->value(), tz_->value()}, radius_->value(), grade_->value())};
    }
    return {};
}

void LayoutDialog::updatePreview() {
    try {
        const auto ls = layouts();
        std::vector<std::vector<Vec3>> lines;
        double len = 0, plan = 0;
        QStringList notes;
        for (const auto& l : ls) {
            lines.push_back(geometry::expandBulges(l.vertices, l.bulges, false, 0.05));
            len += l.length3d;
            plan += l.planLength;
            for (const auto& n : l.notes) notes << qstr(n);
        }
        info_->setText(tr("Length %1 m (plan %2 m)%3").arg(len, 0, 'f', 2).arg(plan, 0, 'f', 2).arg(notes.isEmpty() ? QString() : "\n" + notes.join("\n")));
        if (preview_) preview_(lines);
    } catch (const std::exception& e) {
        info_->setText(tr("<span style='color:#d32f2f'>%1</span>").arg(qstr(e.what()).toHtmlEscaped()));
        if (preview_) preview_({});
    }
}

std::vector<Uuid> LayoutDialog::create() {
    std::vector<Uuid> ids;
    const QString prefix = templ_->idOrPrefix();
    if (prefix.isEmpty()) {
        ctx_.report(tr("Enter a heading id (prefix)"), MessageLevel::Error);
        return ids;
    }
    const Uuid layer = ctx_.currentLayer();
    ctx_.tryEdit(tr("Create headings"), [&] {
        ids = mine_design::createHeadings(ctx_.project(), layouts(), templ_->value(), layer, sstr(prefix), sstr(windowTitle()));
    });
    if (!ids.empty()) {
        std::vector<SelectionItem> sel;
        for (const auto& id : ids) sel.push_back({EntityKind::Development, id});
        ctx_.selectMany(sel);
        ctx_.report(tr("%1 heading(s) created").arg(ids.size()));
    }
    return ids;
}

// ---- crosscuts ----------------------------------------------------------------------------------

CrosscutDialog::CrosscutDialog(AppContext& ctx, const Uuid& developmentId, QWidget* parent) : QDialog(parent), ctx_(ctx), dev_(developmentId) {
    const auto* d = ctx_.project().model().developments.find(developmentId);
    setWindowTitle(tr("Crosscuts off %1").arg(d ? qstr(d->headingId) : QString()));
    auto* form = new QFormLayout(this);
    chainages_ = new QLineEdit(this);
    chainages_->setPlaceholderText(tr("explicit chainages, e.g. 25, 60, 95 (empty = regular interval)"));
    interval_ = spin(1, 1000, 25, 2, " m", this);
    first_ = spin(0, 10000, 10, 2, " m", this);
    length_ = spin(1, 1000, 30, 2, " m", this);
    angle_ = spin(1, 179, 90, 1, "°", this);
    grade_ = spin(-20, 20, 1, 2, " %", this);
    offset_ = spin(0, 20, d ? d->designWidth / 2 : 2.5, 2, " m", this);
    side_ = new QComboBox(this);
    side_->addItem(tr("Right"), static_cast<int>(mine_design::Side::Right));
    side_->addItem(tr("Left"), static_cast<int>(mine_design::Side::Left));
    side_->addItem(tr("Both"), static_cast<int>(mine_design::Side::Both));
    form->addRow(tr("Chainages"), chainages_);
    form->addRow(tr("Interval"), interval_);
    form->addRow(tr("First at"), first_);
    form->addRow(tr("Plan length"), length_);
    form->addRow(tr("Angle to the drive"), angle_);
    form->addRow(tr("Drainage grade"), grade_);
    form->addRow(tr("Start offset from centreline"), offset_);
    form->addRow(tr("Side"), side_);
    form->addRow(buttons(this, tr("Create"), [this] { return !create().empty(); }));
}

std::vector<Uuid> CrosscutDialog::create() {
    mine_design::CrosscutSpec s;
    for (const auto& part : chainages_->text().split(',', Qt::SkipEmptyParts)) {
        bool ok = false;
        const double v = part.trimmed().toDouble(&ok);
        if (ok) s.chainages.push_back(v);
    }
    s.interval = interval_->value();
    s.firstChainage = first_->value();
    s.length = length_->value();
    s.angleDeg = angle_->value();
    s.gradePercent = grade_->value();
    s.startOffset = offset_->value();
    s.side = static_cast<mine_design::Side>(side_->currentData().toInt());
    std::vector<Uuid> ids;
    const Uuid layer = ctx_.currentLayer();
    ctx_.tryEdit(tr("Create crosscuts"), [&] { ids = mine_design::createCrosscuts(ctx_.project(), dev_, s, layer); });
    if (!ids.empty()) ctx_.report(tr("%1 crosscuts created").arg(ids.size()));
    return ids;
}

// ---- heading from geometry ----------------------------------------------------------------------

HeadingDialog::HeadingDialog(AppContext& ctx, const Uuid& geometryId, QWidget* parent) : QDialog(parent), ctx_(ctx), geometry_(geometryId) {
    const auto* g = ctx_.project().model().geometry.find(geometryId);
    setWindowTitle(tr("Development heading from %1").arg(g ? displayName(ctx_.project().model(), {EntityKind::Geometry, geometryId}) : QString()));
    auto* lay = new QVBoxLayout(this);
    templ_ = new HeadingTemplate(ctx_, "Development", false, this);
    lay->addWidget(templ_);
    auto* row = new QHBoxLayout;
    activity_ = new QCheckBox(tr("Create a linked, rate-driven schedule activity at"), this);
    rate_ = spin(0.01, 1000, 4.0, 2, tr(" m/day"), this);
    row->addWidget(activity_);
    row->addWidget(rate_);
    lay->addLayout(row);
    lay->addWidget(buttons(this, tr("Create"), [this] { return !create().isNil(); }));
}

Uuid HeadingDialog::create() {
    auto spec = templ_->value();
    if (spec.headingId.empty()) {
        ctx_.report(tr("Enter a heading id"), MessageLevel::Error);
        return {};
    }
    Uuid id;
    ctx_.tryEdit(tr("Create heading"), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), "Create heading " + spec.headingId);
        id = mine_design::createDevelopment(p, geometry_, spec);
        if (activity_->isChecked()) mine_design::createLinkedActivity(p, id, rate_->value());
        macro.commit();
    });
    if (!id.isNil()) ctx_.select(EntityKind::Development, id);
    return id;
}

// ---- stopes -------------------------------------------------------------------------------------------

StopeDialog::StopeDialog(AppContext& ctx, const Uuid& geometryId, QWidget* parent) : QDialog(parent), ctx_(ctx), geometry_(geometryId) {
    const auto& m = ctx_.project().model();
    const auto* g = m.geometry.find(geometryId);
    outline_ = g && geometry::isCurve(g->type);
    setWindowTitle(outline_ ? tr("Stope from outline") : tr("Stope from shape"));
    auto* form = new QFormLayout(this);
    name_ = new QLineEdit(g && !g->name.empty() ? qstr(g->name) : tr("Stope %1").arg(m.stopes.size() + 1), this);
    code_ = new QLineEdit(this);
    level_ = new QComboBox(this);
    fillEntityCombo(level_, m, EntityKind::Level, {}, true);
    material_ = new QComboBox(this);
    fillEntityCombo(material_, m, EntityKind::Material, m.materials.empty() ? Uuid{} : m.materials.all().front().id, true);
    method_ = new QComboBox(this);
    for (const auto& n : enumNames<StopeMethod>()) method_->addItem(qstr(n));
    zone_ = new QLineEdit(this);
    grade_ = spin(0, 1e6, 0, 3, {}, this);
    unit_ = new QLineEdit("g/t", this);
    dilution_ = spin(0, 200, 10, 1, " %", this);
    recovery_ = spin(0, 100, 95, 1, " %", this);
    double zmin = 0;
    if (g) {
        if (auto b = geometry::objectBounds(*g)) zmin = b->min.z;
    }
    floor_ = spin(-1e5, 1e5, zmin, 3, " m", this);
    back_ = spin(-1e5, 1e5, zmin + 20, 3, " m", this);
    form->addRow(tr("Name"), name_);
    form->addRow(tr("Code"), code_);
    form->addRow(tr("Level"), level_);
    form->addRow(tr("Material"), material_);
    form->addRow(tr("Method"), method_);
    form->addRow(tr("Zone"), zone_);
    form->addRow(tr("Grade"), grade_);
    form->addRow(tr("Grade unit"), unit_);
    form->addRow(tr("Dilution"), dilution_);
    form->addRow(tr("Recovery"), recovery_);
    if (outline_) {
        form->addRow(tr("Floor elevation"), floor_);
        form->addRow(tr("Back elevation"), back_);
    } else {
        floor_->hide();
        back_->hide();
    }
    form->addRow(buttons(this, tr("Create"), [this] { return !create().isNil(); }));
}

Uuid StopeDialog::create() {
    mine_design::NewStope s;
    s.name = sstr(name_->text().trimmed());
    s.code = sstr(code_->text().trimmed());
    s.levelId = comboUuid(level_);
    s.materialId = comboUuid(material_);
    s.method = enumFromString<StopeMethod>(sstr(method_->currentText())).value_or(StopeMethod::LongholeOpenStope);
    s.zone = sstr(zone_->text().trimmed());
    s.grade = grade_->value();
    s.gradeUnit = sstr(unit_->text());
    s.dilutionPercent = dilution_->value();
    s.recoveryPercent = recovery_->value();
    s.layerId = ctx_.currentLayer();
    Uuid id;
    ctx_.tryEdit(tr("Create stope"), [&] {
        auto& p = ctx_.project();
        if (outline_) {
            const auto* g = p.model().geometry.find(geometry_);
            if (!g) throw ValidationError("The outline no longer exists");
            geometry::Ring ring;
            auto pts = geometry::curvePolyline(*g);
            if (pts.size() > 1 && distance(pts.front(), pts.back()) < 1e-9) pts.pop_back();
            for (const auto& v : pts) ring.push_back(xy(v));
            id = mine_design::createStopeFromOutline(p, ring, floor_->value(), back_->value(), s);
        } else {
            id = mine_design::createStopeFromGeometry(p, geometry_, s);
        }
    });
    if (!id.isNil()) ctx_.select(EntityKind::Stope, id);
    return id;
}

SubdivideDialog::SubdivideDialog(AppContext& ctx, const Uuid& stopeId, QWidget* parent) : QDialog(parent), ctx_(ctx), stope_(stopeId) {
    const auto& m = ctx_.project().model();
    const auto* st = m.stopes.find(stopeId);
    setWindowTitle(tr("Subdivide %1 into mining blocks").arg(st ? qstr(st->name) : QString()));
    auto* form = new QFormLayout(this);
    outline_ = new QComboBox(this);
    fillEntityCombo(outline_, m, EntityKind::Geometry, firstSelectedCurve(ctx_), false, [&m](const Uuid& id) { return isCurve(m, id, true, 3); });
    double z0 = 0, z1 = 20;
    if (st)
        if (const auto* g = m.geometry.find(st->geometryId))
            if (auto b = geometry::objectBounds(*g)) {
                z0 = b->min.z;
                z1 = b->max.z;
            }
    floor_ = spin(-1e5, 1e5, z0, 3, " m", this);
    back_ = spin(-1e5, 1e5, z1, 3, " m", this);
    strike_ = spin(0, 360, 0, 2, "°", this);
    panel_ = spin(0.5, 1000, 15, 2, " m", this);
    lift_ = spin(0, 1000, 0, 2, " m", this);
    form->addRow(tr("Stope outline (closed polyline)"), outline_);
    form->addRow(tr("Floor"), floor_);
    form->addRow(tr("Back"), back_);
    form->addRow(tr("Strike bearing"), strike_);
    form->addRow(tr("Panel length along strike"), panel_);
    form->addRow(tr("Lift height (0 = one lift)"), lift_);
    form->addRow(buttons(this, tr("Subdivide"), [this] { return !create().empty(); }));
}

std::vector<Uuid> SubdivideDialog::create() {
    std::vector<Uuid> ids;
    ctx_.tryEdit(tr("Subdivide stope"), [&] {
        auto& p = ctx_.project();
        const auto* g = p.model().geometry.find(comboUuid(outline_));
        if (!g) throw ValidationError("Choose the stope outline");
        geometry::Ring ring;
        auto pts = geometry::curvePolyline(*g);
        if (pts.size() > 1 && distance(pts.front(), pts.back()) < 1e-9) pts.pop_back();
        for (const auto& v : pts) ring.push_back(xy(v));
        ids = mine_design::subdivideStope(p, stope_, ring, floor_->value(), back_->value(), strike_->value(), panel_->value(), lift_->value());
    });
    if (!ids.empty()) ctx_.report(tr("%1 mining blocks created").arg(ids.size()));
    return ids;
}

// ---- levels -------------------------------------------------------------------------------------------

LevelDialog::LevelDialog(AppContext& ctx, QWidget* parent) : QDialog(parent), ctx_(ctx) {
    setWindowTitle(tr("New level"));
    auto* form = new QFormLayout(this);
    name_ = new QLineEdit(this);
    code_ = new QLineEdit(this);
    zone_ = new QLineEdit(this);
    elevation_ = spin(-1e5, 1e5, 0, 3, " m", this);
    assign_ = new QCheckBox(tr("Assign developments to their nearest level afterwards"), this);
    assign_->setChecked(true);
    tolerance_ = spin(0, 100, 5, 2, " m", this);
    form->addRow(tr("Name"), name_);
    form->addRow(tr("Code"), code_);
    form->addRow(tr("Floor RL"), elevation_);
    form->addRow(tr("Zone"), zone_);
    form->addRow(assign_);
    form->addRow(tr("Tolerance"), tolerance_);
    form->addRow(buttons(this, tr("Create"), [this] { return !create().isNil(); }));
}

Uuid LevelDialog::create() {
    MineLevel l;
    l.name = sstr(name_->text().trimmed());
    l.code = sstr(code_->text().trimmed());
    l.zone = sstr(zone_->text().trimmed());
    l.elevation = elevation_->value();
    if (l.name.empty()) l.name = l.code.empty() ? "Level " + std::to_string(static_cast<int>(l.elevation)) : l.code + " Level";
    Uuid id;
    ctx_.tryEdit(tr("Create level"), [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), "Create level " + l.name);
        id = ProjectEditor(p).add(l);
        if (assign_->isChecked()) {
            const int n = mine_design::assignDevelopmentsToLevels(p, tolerance_->value());
            ctx_.report(tr("%1 developments assigned to levels").arg(n));
        }
        macro.commit();
    });
    return id;
}

// ---- revisions ------------------------------------------------------------------------------------

RevisionDialog::RevisionDialog(AppContext& ctx, const Uuid& entityId, QWidget* parent) : QDialog(parent), ctx_(ctx), entity_(entityId) {
    const auto& m = ctx_.project().model();
    std::string current = "A";
    if (const auto* d = m.developments.find(entityId)) current = d->designRevision;
    if (const auto* s = m.stopes.find(entityId)) current = s->designRevision;
    setWindowTitle(tr("Capture design revision"));
    auto* form = new QFormLayout(this);
    // Suggest the next letter after the current revision.
    QString next = qstr(current);
    bool hasRevision = false;
    for (const auto& r : m.designRevisions.all()) hasRevision = hasRevision || r.entityId == entityId;
    if (hasRevision && next.size() == 1 && next[0].isLetter() && next[0] < 'Z') next = QString(QChar(next[0].unicode() + 1));
    code_ = new QLineEdit(next, this);
    comment_ = new QPlainTextEdit(this);
    form->addRow(tr("Revision code"), code_);
    form->addRow(tr("Comment"), comment_);
    form->addRow(new QLabel(tr("The snapshot (entity and geometry) is immutable; approval is a separate, audited step."), this));
    form->addRow(buttons(this, tr("Capture"), [this] { return !capture().isNil(); }));
}

Uuid RevisionDialog::capture() {
    Uuid id;
    ctx_.tryEdit(tr("Capture revision"), [&] { id = mine_design::captureRevision(ctx_.project(), entity_, sstr(code_->text().trimmed()), sstr(comment_->toPlainText())); });
    return id;
}

QString revisionComparisonText(const ProjectModel& m, const Uuid& fromId, const std::optional<Uuid>& toId, const Project& p) {
    (void)m;
    const auto c = mine_design::compareRevisions(p, fromId, toId);
    QString text = QObject::tr("Revision %1 → %2\nLength change %3 m\nVolume change %4 m³\nMax vertex deviation %5 m\nVertices %6 → %7")
                       .arg(qstr(c.fromCode), qstr(c.toCode))
                       .arg(c.lengthChange, 0, 'f', 3)
                       .arg(c.volumeChange, 0, 'f', 2)
                       .arg(c.maxVertexDeviation, 0, 'f', 3)
                       .arg(c.vertexCountFrom)
                       .arg(c.vertexCountTo);
    for (const auto& a : c.attributeChanges) text += "\n" + qstr(a);
    return text;
}

// ---- gradients and rules --------------------------------------------------------------------------

GradientDialog::GradientDialog(AppContext& ctx, const Uuid& geometryId, QWidget* parent) : QDialog(parent), ctx_(ctx), geometry_(geometryId) {
    setWindowTitle(tr("Gradient profile and design rules — %1").arg(displayName(ctx_.project().model(), {EntityKind::Geometry, geometryId})));
    resize(760, 520);
    auto* lay = new QVBoxLayout(this);
    auto* rules = new QHBoxLayout;
    maxGrade_ = spin(0, 100, 15, 2, " %", this);
    maxCurveGrade_ = spin(0, 100, 12.5, 2, " %", this);
    minRadius_ = spin(0, 1000, 20, 2, " m", this);
    maxChange_ = spin(0, 100, 5, 2, " %", this);
    rules->addWidget(new QLabel(tr("Max grade"), this));
    rules->addWidget(maxGrade_);
    rules->addWidget(new QLabel(tr("on curves"), this));
    rules->addWidget(maxCurveGrade_);
    rules->addWidget(new QLabel(tr("Min radius"), this));
    rules->addWidget(minRadius_);
    rules->addWidget(new QLabel(tr("Max change"), this));
    rules->addWidget(maxChange_);
    lay->addLayout(rules);
    table_ = new QTableWidget(this);
    table_->setColumnCount(7);
    table_->setHorizontalHeaderLabels({tr("Segment"), tr("From (m)"), tr("To (m)"), tr("Plan (m)"), tr("Rise (m)"), tr("Grade"), tr("Radius (m)")});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    lay->addWidget(table_, 1);
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    lay->addWidget(summary_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(bb);
    for (auto* s : {maxGrade_, maxCurveGrade_, minRadius_, maxChange_}) connect(s, &QDoubleSpinBox::editingFinished, this, &GradientDialog::recompute);
    recompute();
}

void GradientDialog::recompute() {
    const auto* g = ctx_.project().model().geometry.find(geometry_);
    if (!g) return;
    const auto segs = mine_design::gradientProfile(g->vertices, g->bulges);
    table_->setRowCount(static_cast<int>(segs.size()));
    for (int i = 0; i < static_cast<int>(segs.size()); ++i) {
        const auto& s = segs[static_cast<std::size_t>(i)];
        const QStringList cells = {QString::number(s.index + 1), QString::number(s.chainageFrom, 'f', 2), QString::number(s.chainageTo, 'f', 2), QString::number(s.planLength, 'f', 2),
                                   QString::number(s.rise, 'f', 3), QStringLiteral("%1 %  %2").arg(s.gradePercent, 0, 'f', 2).arg(qstr(s.oneIn())),
                                   s.radius > 0 ? QString::number(s.radius, 'f', 2) : QString()};
        for (int c = 0; c < cells.size(); ++c) table_->setItem(i, c, new QTableWidgetItem(cells[c]));
    }
    mine_design::DesignRules r;
    r.maxGradePercent = maxGrade_->value();
    r.maxGradeOnCurvesPercent = maxCurveGrade_->value();
    r.minRadius = minRadius_->value();
    r.maxGradeChangePercent = maxChange_->value();
    const auto v = mine_design::checkDesignRules(g->vertices, g->bulges, r);
    if (v.empty()) {
        summary_->setText(tr("No design-rule violations."));
        return;
    }
    QStringList lines;
    for (const auto& x : v) lines << tr("%1 at %2 m: %3 (limit %4)").arg(qstr(x.rule)).arg(x.chainage, 0, 'f', 1).arg(x.value, 0, 'f', 2).arg(x.limit, 0, 'f', 2);
    summary_->setText(tr("<b>%1 violations</b><br>%2").arg(v.size()).arg(lines.join("<br>")));
}

} // namespace openmine::app
