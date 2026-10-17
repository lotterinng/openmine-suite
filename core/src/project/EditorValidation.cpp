// SPDX-License-Identifier: Apache-2.0
// Per-entity validation rules used by ProjectEditor::add/update. Each rule throws
// ValidationError with a message suitable for the user.
#include "openmine/project/ProjectEditor.h"

#include "openmine/geometry/Curves.h"
#include "openmine/geometry/Polygon2D.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <set>

namespace openmine {

namespace {
bool finite(double v) { return std::isfinite(v); }
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
[[noreturn]] void fail(const std::string& m) { throw ValidationError(m); }
void require(bool ok, const std::string& m) {
    if (!ok) fail(m);
}
void requirePositive(double v, const std::string& what) { require(finite(v) && v > 0, what + " must be greater than zero"); }
void requireNonNegative(double v, const std::string& what) { require(finite(v) && v >= 0, what + " must be zero or positive"); }
template <class Store> void requireRef(const Store& s, const Uuid& id, const std::string& what, bool optional = true) {
    if (id.isNil()) {
        require(optional, what + " is required");
        return;
    }
    require(s.contains(id), what + " does not exist");
}
template <class Store, class Get>
void requireUnique(const Store& s, const Uuid& self, const std::string& value, Get get, const std::string& what) {
    for (const auto& e : s.all())
        if (e.id != self && get(e) == value) fail(what + " '" + value + "' is already used");
}
} // namespace

// ---- Project-level ------------------------------------------------------------------------------
template <> void ProjectEditor::validate(const CoordinateReferenceSystem& c) const {
    require(!c.name.empty(), "CRS name must not be empty");
    if (c.kind == CrsKind::LocalGrid) requireRef(p_.model().localGrids, c.localGridId, "Local grid definition", false);
    require(c.linearUnit == "metre", "Only metre-based coordinate systems are supported for project data");
}

template <> void ProjectEditor::validate(const LocalGridDefinition& g) const {
    require(!g.name.empty(), "Local grid name must not be empty");
    requireRef(p_.model().crs, g.baseCrsId, "Base CRS", false);
    require(g.localToBase.inverse().has_value(), "Local grid transformation must be invertible");
    requireNonNegative(g.rmsResidual, "RMS residual");
}

template <> void ProjectEditor::validate(const TransformationRecord& t) const {
    require(!t.sourceCrs.empty() && !t.targetCrs.empty(), "Transformation needs source and target CRS");
    if (p_.model().transformations.contains(t.id)) fail("Transformation records are immutable");
}

template <> void ProjectEditor::validate(const Asset& a) const {
    require(!a.relativePath.empty(), "Asset path must not be empty");
    require(a.relativePath.find("..") == std::string::npos && a.relativePath.front() != '/',
            "Asset paths must be relative to the asset folder");
    require(a.sizeBytes >= 0, "Asset size must not be negative");
    if (const auto* old = p_.model().assets.find(a.id))
        require(old->sha256 == a.sha256 && old->relativePath == a.relativePath, "Assets are immutable once stored");
}

template <> void ProjectEditor::validate(const Attachment& a) const {
    require(!a.fileName.empty(), "Attachment needs a file name");
    requireRef(p_.model().assets, a.asset.id, "Attachment asset", false);
}

template <> void ProjectEditor::validate(const Material& m) const {
    require(!m.name.empty(), "Material name must not be empty");
    requireUnique(p_.model().materials, m.id, m.name, [](const auto& x) { return x.name; }, "Material name");
    requirePositive(m.density, "Density");
    require(finite(m.swellFactor) && m.swellFactor >= 1.0, "Swell factor must be at least 1.0");
}

template <> void ProjectEditor::validate(const AnalysisResult& a) const {
    require(!a.name.empty(), "Analysis result needs a name");
    if (!a.payloadAsset.isNull()) requireRef(p_.model().assets, a.payloadAsset.id, "Analysis payload asset", false);
}

template <> void ProjectEditor::validate(const Report& r) const {
    require(!r.name.empty(), "Report needs a name");
    requireRef(p_.model().assets, r.asset.id, "Report file", false);
}

template <> void ProjectEditor::validate(const AttributeDefinition& def) const {
    const auto& types = attributeEntityTypes();
    require(std::find(types.begin(), types.end(), def.entityType) != types.end(),
            "Custom attributes are supported on: GeometryObject, MineDevelopment, Stope, ScheduleActivity");
    require(isValidAttributeKey(def.key), "Invalid attribute key '" + def.key + "'");
    for (const auto& d : p_.model().attributeDefinitions.all())
        if (d.id != def.id && d.entityType == def.entityType && d.key == def.key) fail("Attribute '" + def.key + "' already defined");
    if (!def.defaultValue.empty()) {
        auto err = validateAttributeValue(def, def.defaultValue);
        if (!err.empty()) fail("Default value: " + err);
    }
}

// ---- CAD -------------------------------------------------------------------------------------------
template <> void ProjectEditor::validate(const CadLayer& l) const {
    require(!l.name.empty(), "Layer name must not be empty");
    if (const auto* other = findLayerByName(l.name); other && other->id != l.id) fail("Layer '" + l.name + "' already exists");
    require(finite(l.transparency) && l.transparency >= 0 && l.transparency <= 1, "Transparency must be between 0 and 1");
    requirePositive(l.lineWidth, "Line width");
    static const std::set<std::string> types{"Continuous", "Dashed", "Dotted", "DashDot"};
    require(types.count(l.lineType) == 1, "Line type must be Continuous, Dashed, Dotted or DashDot");
}

template <> void ProjectEditor::validate(const GeometryObject& g) const {
    const auto& m = p_.model();
    const auto* layer = m.layers.find(g.layerId);
    require(layer != nullptr, "Geometry must be on an existing layer");
    require(!layer->locked, "Layer '" + layer->name + "' is locked");
    for (const auto& v : g.vertices) require(finite(v), "Vertex coordinates must be finite");
    requireRef(m.materials, g.materialId, "Material");
    if (g.transparency) require(*g.transparency >= 0 && *g.transparency <= 1, "Transparency must be between 0 and 1");
    const auto nv = g.vertices.size();
    switch (g.type) {
    case GeometryType::Point: require(nv == 1, "A point has exactly one vertex"); break;
    case GeometryType::Line:
        require(nv == 2, "A line has exactly two vertices");
        require(distance(g.vertices[0], g.vertices[1]) > 1e-9, "A line needs two distinct end points");
        break;
    case GeometryType::Polyline:
        require(nv >= 2, "A polyline needs at least two vertices");
        require(!g.closed || nv >= 3, "A closed polyline needs at least three vertices");
        require(g.bulges.empty() || g.bulges.size() == nv, "Polyline bulges must have one value per vertex");
        for (double b : g.bulges) require(finite(b) && std::abs(b) < 1e6, "Polyline bulge values must be finite");
        break;
    case GeometryType::Arc:
    case GeometryType::Circle: {
        require(finite(g.arc.center) && finite(g.arc.radius) && g.arc.radius > 0, "Arc/circle radius must be greater than zero");
        require(g.arc.normal.length() > 1e-12, "Arc/circle plane normal must not be zero");
        if (g.type == GeometryType::Arc) {
            const double sweep = g.arc.endAngle - g.arc.startAngle;
            require(finite(sweep) && sweep > 1e-12 && sweep <= 2 * std::numbers::pi + 1e-12, "Arc sweep must be in (0°, 360°]");
        }
        break;
    }
    case GeometryType::Spline: require(nv >= 2, "A spline needs at least two fit points"); break;
    case GeometryType::Text:
        require(nv == 1, "Text needs one insertion point");
        require(!g.text.text.empty(), "Text must not be empty");
        requirePositive(g.text.height, "Text height");
        break;
    case GeometryType::Dimension: {
        std::size_t need = 0;
        switch (g.dimension.kind) {
        case DimensionKind::Aligned:
        case DimensionKind::Horizontal:
        case DimensionKind::Vertical: need = 3; break;
        case DimensionKind::Angular: need = 4; break;
        case DimensionKind::Radius:
        case DimensionKind::Diameter: need = 2; break;
        case DimensionKind::Elevation: need = 1; break;
        }
        require(nv == need, std::string(toString(g.dimension.kind)) + " dimension needs " + std::to_string(need) + " definition points");
        requirePositive(g.dimension.textHeight, "Dimension text height");
        require(g.dimension.precision >= 0 && g.dimension.precision <= 8, "Dimension precision must be 0–8");
        break;
    }
    case GeometryType::Hatch: {
        static const std::set<std::string> patterns{"SOLID", "ANSI31", "LINES", "CROSS", "DOTS"};
        require(patterns.count(g.hatch.pattern) == 1, "Unknown hatch pattern '" + g.hatch.pattern + "'");
        requirePositive(g.hatch.spacing, "Hatch spacing");
        std::vector<std::uint32_t> starts = g.hatch.loopStarts.empty() ? std::vector<std::uint32_t>{0} : g.hatch.loopStarts;
        require(starts.front() == 0, "First hatch loop must start at vertex 0");
        for (std::size_t i = 0; i < starts.size(); ++i) {
            const std::size_t b = starts[i], e = i + 1 < starts.size() ? starts[i + 1] : nv;
            require(e > b && e - b >= 3, "Each hatch boundary loop needs at least three vertices");
            std::vector<Vec2> ring;
            for (std::size_t k = b; k < e; ++k) ring.push_back(xy(g.vertices[k]));
            require(geometry::area(ring) > 1e-12, "Hatch boundary loops must enclose an area");
        }
        break;
    }
    case GeometryType::Mesh:
        require(g.mesh && !g.mesh->triangles.empty(), "A mesh needs at least one triangle");
        for (const auto& t : g.mesh->triangles)
            for (auto i : t) require(i < g.mesh->vertices.size(), "Mesh triangle refers to a missing vertex");
        for (const auto& v : g.mesh->vertices) require(finite(v), "Mesh vertices must be finite");
        require(g.mesh->vertexScalars.empty() || g.mesh->vertexScalars.size() == g.mesh->vertices.size(),
                "Mesh scalars must have one value per vertex");
        break;
    case GeometryType::Solid:
        requireRef(m.assets, g.brepAsset.id, "Solid B-rep asset", false);
        require(g.mesh && !g.mesh->triangles.empty(), "A solid needs a display tessellation");
        break;
    }
    auto errs = validateAttributes("GeometryObject", g.attributes);
    if (!errs.empty()) fail(errs.front());
}

// ---- Survey ----------------------------------------------------------------------------------------
template <> void ProjectEditor::validate(const SurveyControlPoint& c) const {
    require(!c.name.empty(), "Control point name must not be empty");
    requireUnique(p_.model().controlPoints, c.id, c.name, [](const auto& x) { return x.name; }, "Control point name");
    require(finite(c.position), "Control coordinates must be finite");
    requireNonNegative(c.horizontalSigma, "Horizontal accuracy");
    requireNonNegative(c.verticalSigma, "Vertical accuracy");
}

template <> void ProjectEditor::validate(const PointCloudDataset& d) const {
    const auto& m = p_.model();
    require(!d.name.empty(), "Dataset name must not be empty");
    requireRef(m.assets, d.asset.id, "Point-cloud asset", false);
    require(d.pointCount >= 0, "Point count must not be negative");
    require(d.transform.inverse().has_value(), "Dataset transform must be invertible");
    requireRef(m.pointClouds, d.parentDatasetId, "Parent dataset");
    require(d.parentDatasetId != d.id, "A dataset cannot be its own parent");
    requireRef(m.developments, d.developmentId, "Development");
    requirePositive(d.pointSize, "Point size");
}

template <> void ProjectEditor::validate(const Registration& r) const {
    const auto& m = p_.model();
    requireRef(m.pointClouds, r.datasetId, "Registered dataset", false);
    if (r.method == RegistrationMethod::IcpPointToPoint || r.method == RegistrationMethod::IcpPointToPlane)
        requireRef(m.pointClouds, r.targetDatasetId, "ICP target dataset", false);
    require(r.transform.inverse().has_value(), "Registration transform must be invertible");
    if (!r.allowScale) require(r.transform.isRigid(1e-6), "A rigid registration must not contain scale or shear");
    for (const auto& pr : r.pairs) {
        require(finite(pr.source) && finite(pr.target), "Registration pair coordinates must be finite");
        requireRef(m.controlPoints, pr.targetControlId, "Control point of pair '" + pr.name + "'");
    }
    if (const auto* old = m.registrations.find(r.id); old && old->applied)
        require(old->transform == r.transform && old->pairs == r.pairs, "An applied registration cannot be changed");
}

template <> void ProjectEditor::validate(const AsBuiltModel& a) const {
    const auto& m = p_.model();
    require(!a.name.empty(), "As-built model needs a name");
    requireRef(m.pointClouds, a.sourceDatasetId, "Source dataset");
    requireRef(m.registrations, a.registrationId, "Registration");
    requireRef(m.developments, a.developmentId, "Development");
    const auto* g = m.geometry.find(a.geometryId);
    require(g && (g->type == GeometryType::Mesh || g->type == GeometryType::Solid), "As-built geometry must be a mesh or solid");
}

template <> void ProjectEditor::validate(const DesignConformanceResult& c) const {
    const auto& m = p_.model();
    requireRef(m.asBuiltModels, c.asBuiltId, "As-built model", false);
    requireRef(m.developments, c.developmentId, "Development", false);
    requireRef(m.analysisResults, c.analysisResultId, "Per-station result");
    requirePositive(c.stationInterval, "Station interval");
}

// ---- Design ----------------------------------------------------------------------------------------
template <> void ProjectEditor::validate(const TunnelProfile& t) const {
    require(!t.name.empty(), "Profile name must not be empty");
    requireUnique(p_.model().profiles, t.id, t.name, [](const auto& x) { return x.name; }, "Profile name");
    requirePositive(t.width, "Profile width");
    requirePositive(t.height, "Profile height");
    switch (t.shape) {
    case ProfileShape::ShoulderArch:
        requirePositive(t.shoulderRadius, "Shoulder radius");
        require(t.shoulderRadius <= t.width / 2 && t.shoulderRadius < t.height, "Shoulder radius must be ≤ half the width and < height");
        break;
    case ProfileShape::SemicircularArch: require(t.width / 2 < t.height, "Semicircular arch needs height > width/2"); break;
    case ProfileShape::SegmentalArch:
        requirePositive(t.archRise, "Arch rise");
        require(t.archRise <= t.width / 2 && t.archRise < t.height, "Arch rise must be ≤ half the width and < height");
        break;
    case ProfileShape::Horseshoe:
        require(t.wallBatter >= 0 && t.wallBatter < 1, "Wall batter must be in [0, 1)");
        require(t.width / 2 < t.height, "Horseshoe profile needs height > width/2");
        break;
    case ProfileShape::Custom: {
        require(t.customPoints.size() >= 3, "A custom profile needs at least three points");
        require(geometry::area(t.customPoints) > 1e-6, "A custom profile must enclose an area");
        break;
    }
    case ProfileShape::Rectangular: break;
    }
    require(t.arcSegments >= 2 && t.arcSegments <= 64, "Arc segments must be 2–64");
}

template <> void ProjectEditor::validate(const MineLevel& l) const {
    require(!l.name.empty(), "Level name must not be empty");
    require(finite(l.elevation), "Level elevation must be finite");
    if (!l.code.empty()) requireUnique(p_.model().levels, l.id, l.code, [](const auto& x) { return x.code; }, "Level code");
}

template <> void ProjectEditor::validate(const Decline& d) const {
    require(!d.name.empty(), "Decline name must not be empty");
    require(finite(d.designGradientPercent) && std::abs(d.designGradientPercent) <= 50, "Decline gradient must be within ±50 %");
    requirePositive(d.minCurveRadius, "Minimum curve radius");
    requireRef(p_.model().profiles, d.profileId, "Profile");
}

template <> void ProjectEditor::validate(const GroundSupportClass& s) const {
    require(!s.name.empty(), "Support class name must not be empty");
    requireNonNegative(s.boltLength, "Bolt length");
    requirePositive(s.boltSpacing, "Bolt spacing");
    requireNonNegative(s.shotcreteThickness, "Shotcrete thickness");
    requireNonNegative(s.installMinutesPerBolt, "Bolt install time");
}

template <> void ProjectEditor::validate(const MineDevelopment& d) const {
    const auto& m = p_.model();
    require(!d.headingId.empty(), "Heading ID must not be empty");
    const auto* g = m.geometry.find(d.geometryId);
    require(g != nullptr, "Development must reference existing centreline geometry");
    require(geometry::isCurve(g->type), "A development centreline must be a line, polyline, arc or spline");
    requirePositive(d.designWidth, "Design width");
    requirePositive(d.designHeight, "Design height");
    require(finite(d.chainageFrom), "Chainage must be finite");
    requireRef(m.levels, d.levelId, "Level");
    requireRef(m.declines, d.declineId, "Decline");
    requireRef(m.profiles, d.profileId, "Tunnel profile");
    requireRef(m.materials, d.materialId, "Material");
    requireRef(m.supportClasses, d.supportClassId, "Ground support class");
    for (const auto& other : m.developments.all()) {
        if (other.id == d.id) continue;
        if (other.headingId == d.headingId) fail("Heading ID '" + d.headingId + "' is already used");
        if (other.geometryId == d.geometryId) fail("That geometry is already the centreline of heading '" + other.headingId + "'");
    }
    auto errs = validateAttributes("MineDevelopment", d.attributes);
    if (!errs.empty()) fail(errs.front());
}

template <> void ProjectEditor::validate(const Stope& s) const {
    const auto& m = p_.model();
    require(!s.name.empty(), "Stope name must not be empty");
    const auto* g = m.geometry.find(s.geometryId);
    require(g && (g->type == GeometryType::Mesh || g->type == GeometryType::Solid), "A stope needs a mesh or solid shape");
    requireRef(m.levels, s.levelId, "Level");
    requireRef(m.materials, s.materialId, "Material");
    require(finite(s.dilutionPercent) && s.dilutionPercent >= 0 && s.dilutionPercent <= 100, "Dilution must be 0–100 %");
    require(finite(s.recoveryPercent) && s.recoveryPercent > 0 && s.recoveryPercent <= 100, "Recovery must be in (0, 100] %");
    requireNonNegative(s.grade, "Grade");
    auto errs = validateAttributes("Stope", s.attributes);
    if (!errs.empty()) fail(errs.front());
}

template <> void ProjectEditor::validate(const MiningBlock& b) const {
    require(!b.name.empty(), "Block name must not be empty");
    requireRef(p_.model().stopes, b.stopeId, "Stope", false);
    requireRef(p_.model().geometry, b.geometryId, "Block geometry", false);
}

template <> void ProjectEditor::validate(const DesignRevision& r) const {
    require(!r.revisionCode.empty(), "Revision code must not be empty");
    require(r.entityType == "MineDevelopment" || r.entityType == "Stope", "Revisions are kept for developments and stopes");
    if (const auto* old = p_.model().designRevisions.find(r.id)) {
        require(old->snapshotJson == r.snapshotJson && old->revisionCode == r.revisionCode && old->entityId == r.entityId,
                "Design revisions are immutable snapshots (only approval can change)");
        require(!old->approved || r.approved, "An approved revision cannot be un-approved");
    } else {
        require(p_.model().developments.contains(r.entityId) || p_.model().stopes.contains(r.entityId),
                "Revised design object does not exist");
        for (const auto& o : p_.model().designRevisions.all())
            if (o.entityId == r.entityId && o.revisionCode == r.revisionCode) fail("Revision '" + r.revisionCode + "' already exists");
    }
}

// ---- Scheduling ------------------------------------------------------------------------------------
template <> void ProjectEditor::validate(const WorkCalendar& c) const {
    require(!c.name.empty(), "Calendar name must not be empty");
    require(!c.shifts.empty(), "A calendar needs at least one shift");
    require(finite(c.standardHoursPerDay) && c.standardHoursPerDay > 0 && c.standardHoursPerDay <= 24,
            "Standard hours per day must be in (0, 24]");
    // Weekly pattern: shifts must not overlap (minutes of the week, wrapping Sunday → Monday).
    std::vector<std::pair<int, int>> spans;
    for (const auto& s : c.shifts) {
        require(s.startMinute >= 0 && s.startMinute < 1440, "Shift '" + s.name + "' start must be within the day");
        require(s.durationMinutes > 0 && s.durationMinutes <= 1440, "Shift '" + s.name + "' must last 1 minute to 24 hours");
        require((s.weekdays & 0x7F) != 0, "Shift '" + s.name + "' must work on at least one weekday");
        for (int d = 0; d < 7; ++d)
            if (s.weekdays & (1 << d)) {
                const int b = d * 1440 + s.startMinute, e = b + s.durationMinutes;
                if (e <= 7 * 1440) spans.emplace_back(b, e);
                else {
                    spans.emplace_back(b, 7 * 1440);
                    spans.emplace_back(0, e - 7 * 1440);
                }
            }
    }
    std::sort(spans.begin(), spans.end());
    for (std::size_t i = 1; i < spans.size(); ++i) require(spans[i].first >= spans[i - 1].second, "Calendar shifts overlap");
    std::set<std::int64_t> dates;
    for (const auto& e : c.exceptions) {
        require(dates.insert(e.date.days()).second, "Calendar exception for " + e.date.toString() + " is defined twice");
        int last = -1;
        auto periods = e.periods;
        std::sort(periods.begin(), periods.end(), [](auto& a, auto& b) { return a.startMinute < b.startMinute; });
        for (const auto& p : periods) {
            require(p.startMinute >= 0 && p.endMinute <= 1440 && p.startMinute < p.endMinute,
                    "Exception periods on " + e.date.toString() + " must be within the day");
            require(p.startMinute >= last, "Exception periods on " + e.date.toString() + " overlap");
            last = p.endMinute;
        }
    }
}

template <> void ProjectEditor::validate(const ScheduleActivity& a) const {
    const auto& m = p_.model();
    require(!a.name.empty(), "Activity name must not be empty");
    requireNonNegative(a.durationHours, "Duration");
    requireNonNegative(a.quantity, "Quantity");
    requireNonNegative(a.actualQuantity, "Actual quantity");
    require(finite(a.manualPercentComplete) && a.manualPercentComplete >= 0 && a.manualPercentComplete <= 100,
            "Percent complete must be 0–100");
    requireRef(m.cycles, a.productionModelId, "Development cycle");
    if (a.useCycleRate) require(!a.productionModelId.isNil(), "Select a development cycle to derive the rate from");
    if (a.durationFromRate && !a.useCycleRate)
        require(finite(a.rate) && a.rate > 0, "A rate-driven activity needs a production rate greater than zero");
    requireRef(m.calendars, a.calendarId, "Calendar");
    if (a.constraint != ConstraintType::None) require(a.constraintDate.has_value(), "The constraint needs a date");
    if (a.actualStart && a.actualFinish) require(*a.actualFinish >= *a.actualStart, "Actual finish is before actual start");
    if (a.actualFinish) require(a.actualStart.has_value(), "An activity with an actual finish needs an actual start");
    if (a.parentId) {
        require(a.parentId != a.id, "An activity cannot be its own parent");
        const auto* parent = m.activities.find(a.parentId);
        require(parent && parent->kind == ActivityKind::Summary, "WBS parent must be an existing summary activity");
        for (Uuid cur = parent->parentId; cur;) {
            require(cur != a.id, "WBS hierarchy would contain a cycle");
            const auto* x = m.activities.find(cur);
            cur = x ? x->parentId : Uuid{};
        }
    }
    if (const auto* old = m.activities.find(a.id); old && old->kind == ActivityKind::Summary && a.kind != ActivityKind::Summary)
        for (const auto& x : m.activities.all())
            if (x.parentId == a.id) fail("Activity has WBS children and must remain a summary");
    if (a.kind == ActivityKind::Summary) {
        for (const auto& d : m.dependencies.all())
            if (d.predecessorId == a.id || d.successorId == a.id) fail("Summary activities cannot have dependencies");
        for (const auto& r : m.assignments.all())
            if (r.activityId == a.id) fail("Summary activities cannot have resource assignments");
    }
    auto errs = validateAttributes("ScheduleActivity", a.attributes);
    if (!errs.empty()) fail(errs.front());
}

template <> void ProjectEditor::validate(const ActivityDependency& d) const {
    const auto& m = p_.model();
    const auto* pred = m.activities.find(d.predecessorId);
    const auto* succ = m.activities.find(d.successorId);
    require(pred && succ, "Both activities of a dependency must exist");
    require(d.predecessorId != d.successorId, "An activity cannot depend on itself");
    require(pred->kind != ActivityKind::Summary && succ->kind != ActivityKind::Summary,
            "Dependencies on summary (WBS) activities are not supported; link their child activities");
    require(finite(d.lagHours), "Lag must be finite");
    for (const auto& e : m.dependencies.all())
        if (e.id != d.id && e.predecessorId == d.predecessorId && e.successorId == d.successorId)
            fail("A dependency between these activities already exists");
    std::set<Uuid> seen;
    std::function<bool(const Uuid&)> reaches = [&](const Uuid& from) {
        if (from == d.predecessorId) return true;
        if (!seen.insert(from).second) return false;
        for (const auto& e : m.dependencies.all())
            if (e.id != d.id && e.predecessorId == from && reaches(e.successorId)) return true;
        return false;
    };
    require(!reaches(d.successorId), "Dependency would create a circular logic loop");
}

template <> void ProjectEditor::validate(const Resource& r) const {
    require(!r.name.empty(), "Resource name must not be empty");
    requireUnique(p_.model().resources, r.id, r.name, [](const auto& x) { return x.name; }, "Resource name");
    requirePositive(r.capacity, "Resource capacity");
    requirePositive(r.productivityFactor, "Productivity factor");
    requireNonNegative(r.costPerHour, "Cost per hour");
    requireRef(p_.model().calendars, r.calendarId, "Resource calendar");
}

template <> void ProjectEditor::validate(const ActivityResourceAssignment& a) const {
    const auto& m = p_.model();
    const auto* act = m.activities.find(a.activityId);
    require(act != nullptr, "Assigned activity does not exist");
    require(act->kind != ActivityKind::Summary, "Resources cannot be assigned to summary activities");
    const auto* res = m.resources.find(a.resourceId);
    require(res != nullptr, "Assigned resource does not exist");
    requirePositive(a.units, "Assigned units");
    require(a.units <= res->capacity + 1e-9, "Assigned units exceed the capacity of '" + res->name + "'");
    for (const auto& o : m.assignments.all())
        if (o.id != a.id && o.activityId == a.activityId && o.resourceId == a.resourceId)
            fail("'" + res->name + "' is already assigned to '" + act->name + "'");
}

template <> void ProjectEditor::validate(const DevelopmentCycle& c) const {
    require(!c.name.empty(), "Cycle name must not be empty");
    requirePositive(c.roundLength, "Round length");
    require(finite(c.advanceEfficiency) && c.advanceEfficiency > 0 && c.advanceEfficiency <= 1, "Advance efficiency must be in (0, 1]");
    require(c.holesPerRound > 0 && c.drillBooms > 0, "Holes per round and drill booms must be positive");
    requirePositive(c.drillPenetrationRate, "Drill penetration rate");
    require(c.swellFactor >= 1.0, "Swell factor must be at least 1.0");
    requirePositive(c.bucketCapacity, "Bucket capacity");
    require(c.bucketFillFactor > 0 && c.bucketFillFactor <= 1.2, "Bucket fill factor must be in (0, 1.2]");
    requirePositive(c.muckCycleMinutes, "Mucking cycle time");
    require(c.utilisation > 0 && c.utilisation <= 1, "Utilisation must be in (0, 1]");
    require(c.shiftHours > 0 && c.shiftHours <= 24 && c.shiftsPerDay >= 1 && c.shiftHours * c.shiftsPerDay <= 24,
            "Shift hours × shifts per day must not exceed 24 h");
    for (double v : {c.drillSetupMinutes, c.collarMinutesPerHole, c.chargeMinutesPerHole, c.blastAndReentryMinutes,
                     c.minutesPerBolt, c.meshMinutes, c.shotcreteMinutes, c.scalingMinutes, c.surveyMarkupMinutes, c.serviceMinutes})
        requireNonNegative(v, "Cycle times");
    require(c.boltsPerRound >= 0, "Bolts per round must not be negative");
}

template <> void ProjectEditor::validate(const ProductionActual& a) const {
    const auto& m = p_.model();
    const auto* act = m.activities.find(a.activityId);
    require(act != nullptr, "Actual must belong to an existing activity");
    require(act->kind == ActivityKind::Task, "Actuals are recorded on task activities");
    requireRef(m.developments, a.developmentId, "Development");
    requireRef(m.pointClouds, a.surveyDatasetId, "Survey dataset");
    require(finite(a.quantity), "Actual quantity must be finite");
    require(a.quantity >= 0 || !a.note.empty(), "A negative (correcting) actual needs an explanatory note");
    if (a.source == ActualSource::Survey) require(!a.surveyDatasetId.isNil() || a.faceChainage.has_value(),
                                                  "A survey actual needs the survey dataset or face chainage");
}

template <> void ProjectEditor::validate(const ProductionTarget& t) const {
    require(!t.name.empty(), "Target name must not be empty");
    require(t.periodEnd >= t.periodStart, "Target period end is before its start");
    requireNonNegative(t.quantity, "Target quantity");
}

template <> void ProjectEditor::validate(const ScheduleBaseline& b) const {
    require(!b.name.empty(), "Baseline name must not be empty");
    requireUnique(p_.model().baselines, b.id, b.name, [](const auto& x) { return x.name; }, "Baseline name");
    if (const auto* old = p_.model().baselines.find(b.id)) {
        require(old->activities == b.activities, "Baseline dates are immutable once captured");
        require(!old->approved || b.approved, "An approved baseline cannot be un-approved");
    }
}

template <> void ProjectEditor::validate(const Scenario& s) const {
    const auto& m = p_.model();
    require(!s.name.empty(), "Scenario name must not be empty");
    requireUnique(m.scenarios, s.id, s.name, [](const auto& x) { return x.name; }, "Scenario name");
    static const std::set<std::string> activityFields{"rate", "durationHours", "priority", "excluded", "calendarId"};
    static const std::set<std::string> resourceFields{"capacity", "productivityFactor"};
    for (const auto& o : s.overrides) {
        if (o.field == "areaLimit") {
            const auto eq = o.value.find('=');
            require(eq != std::string::npos && eq > 0, "Area limits are written as <work area>=<max concurrent activities>");
            continue;
        }
        if (m.activities.contains(o.entityId)) require(activityFields.count(o.field) == 1, "Unsupported activity override '" + o.field + "'");
        else if (m.resources.contains(o.entityId)) require(resourceFields.count(o.field) == 1, "Unsupported resource override '" + o.field + "'");
        else fail("Scenario override refers to a missing activity or resource");
    }
    require(s.objective == "Makespan" || s.objective == "WeightedCompletion", "Objective must be Makespan or WeightedCompletion");
    requirePositive(s.optimiserTimeLimitSeconds, "Optimiser time limit");
}

// ---- Geology & geotechnical ---------------------------------------------------------------------
template <> void ProjectEditor::validate(const Drillhole& h) const {
    require(!h.holeId.empty(), "Hole ID must not be empty");
    requireUnique(p_.model().drillholes, h.id, h.holeId, [](const auto& x) { return x.holeId; }, "Hole ID");
    require(finite(h.collar), "Collar coordinates must be finite");
    requirePositive(h.length, "Hole length");
    double last = -1;
    for (const auto& s : h.surveys) {
        require(s.depth >= 0 && s.depth <= h.length + 1e-9, "Survey depths must be within the hole");
        require(s.depth > last, "Survey depths must be strictly increasing");
        require(s.dip >= -90 && s.dip <= 90, "Dip must be within ±90°");
        require(s.azimuth >= 0 && s.azimuth < 360, "Azimuth must be in [0°, 360°)");
        last = s.depth;
    }
    auto iv = h.intervals;
    std::sort(iv.begin(), iv.end(), [](auto& a, auto& b) { return a.from < b.from; });
    double end = 0;
    for (std::size_t i = 0; i < iv.size(); ++i) {
        require(iv[i].from >= 0 && iv[i].to <= h.length + 1e-9 && iv[i].from < iv[i].to, "Intervals must lie within the hole with from < to");
        require(i == 0 || iv[i].from >= end - 1e-9, "Drillhole intervals overlap at " + std::to_string(iv[i].from) + " m");
        end = iv[i].to;
    }
}

template <> void ProjectEditor::validate(const GeologicalObject& g) const {
    require(!g.name.empty(), "Geological object name must not be empty");
    requireRef(p_.model().geometry, g.geometryId, "Geometry", false);
    requireRef(p_.model().materials, g.materialId, "Material");
}

template <> void ProjectEditor::validate(const BlockModel& b) const {
    require(!b.name.empty(), "Block model name must not be empty");
    require(finite(b.origin), "Block model origin must be finite");
    requirePositive(b.blockSize.x, "Block size X");
    requirePositive(b.blockSize.y, "Block size Y");
    requirePositive(b.blockSize.z, "Block size Z");
    require(b.nx >= 1 && b.ny >= 1 && b.nz >= 1, "Block counts must be at least 1");
    require(static_cast<double>(b.nx) * b.ny * b.nz <= 2e8, "Block model exceeds 200 million blocks");
    std::set<std::string> names;
    for (const auto& a : b.attributes) require(isValidAttributeKey(a) && names.insert(a).second, "Invalid or duplicate attribute '" + a + "'");
    if (!b.dataAsset.isNull()) requireRef(p_.model().assets, b.dataAsset.id, "Block data asset", false);
}

template <> void ProjectEditor::validate(const StructuralMeasurement& s) const {
    require(finite(s.dip) && s.dip >= 0 && s.dip <= 90, "Dip must be 0–90°");
    require(finite(s.dipDirection) && s.dipDirection >= 0 && s.dipDirection < 360, "Dip direction must be in [0°, 360°)");
    require(finite(s.location), "Location must be finite");
    requireRef(p_.model().developments, s.developmentId, "Development");
}

template <> void ProjectEditor::validate(const GeotechClassification& g) const {
    require(finite(g.value), "Classification value must be finite");
    requireRef(p_.model().developments, g.developmentId, "Development");
}

} // namespace openmine
