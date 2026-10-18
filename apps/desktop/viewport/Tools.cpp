// SPDX-License-Identifier: Apache-2.0
#include "Tools.h"

#include "Viewport3D.h"

#include "openmine/cad/Edit.h"
#include "openmine/geometry/Curves.h"
#include "openmine/geometry/Polygon2D.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/mine_design/Development.h"
#include "openmine/project/ProjectEditor.h"

#ifdef OPENMINE_APP_HAVE_OCCT
#include "openmine/occt/SolidObjects.h"
#endif

#include <QInputDialog>
#include <QRegularExpression>

#include <cmath>
#include <numbers>

namespace openmine::app {

namespace {
constexpr double kDeg = std::numbers::pi / 180.0;

double planBearingDeg(Vec3 a, Vec3 b) {
    // Bearing clockwise from grid north (+Y).
    double t = std::atan2(b.x - a.x, b.y - a.y) / kDeg;
    if (t < 0) t += 360;
    return t;
}

QString fmt(double v, int d = 3) { return QString::number(v, 'f', d); }
} // namespace

const char* toolName(Tool t) {
    switch (t) {
    case Tool::Select: return "Select";
    case Tool::Point: return "Point";
    case Tool::Line: return "Line";
    case Tool::Polyline: return "Polyline";
    case Tool::Polygon: return "Polygon";
    case Tool::Rectangle: return "Rectangle";
    case Tool::Circle: return "Circle";
    case Tool::Arc3: return "Arc (3 points)";
    case Tool::Spline: return "Spline";
    case Tool::Text: return "Text";
    case Tool::Dimension: return "Dimension";
    case Tool::Measure: return "Measure distance";
    case Tool::MeasureArea: return "Measure area";
    case Tool::Move: return "Move";
    case Tool::Copy: return "Copy";
    case Tool::Rotate: return "Rotate";
    case Tool::Mirror: return "Mirror";
    case Tool::Scale: return "Scale";
    case Tool::Offset: return "Offset";
    case Tool::Trim: return "Trim";
    case Tool::Extend: return "Extend";
    case Tool::Fillet: return "Fillet";
    case Tool::Split: return "Split";
    case Tool::PickPoint: return "Pick point";
    case Tool::PickPoints: return "Pick points";
    case Tool::SectionLine: return "Section line";
    case Tool::CropPolygon: return "Crop polygon";
    }
    return "?";
}

ToolController::ToolController(AppContext& ctx, QObject* parent) : QObject(parent), ctx_(ctx) {
    snap_.modes = cad::SnapMode::Endpoint | cad::SnapMode::Midpoint | cad::SnapMode::Center | cad::SnapMode::Intersection |
                  cad::SnapMode::Node | cad::SnapMode::Perpendicular | cad::SnapMode::Nearest;
    connect(&ctx_, &AppContext::projectReplaced, this, [this] { setTool(Tool::Select); });
}

void ToolController::setPrompt(const QString& p) {
    prompt_ = p;
    emit promptChanged(p);
}

bool ToolController::needsSelection() const {
    switch (tool_) {
    case Tool::Move:
    case Tool::Copy:
    case Tool::Rotate:
    case Tool::Mirror:
    case Tool::Scale: return true;
    default: return false;
    }
}

std::vector<const GeometryObject*> ToolController::selectedObjects() const {
    std::vector<const GeometryObject*> out;
    if (!ctx_.hasProject()) return out;
    for (const auto& id : ctx_.selectedGeometry())
        if (const auto* g = ctx_.project().model().geometry.find(id)) out.push_back(g);
    return out;
}

void ToolController::setTool(Tool t) {
    clearPreview();
    pts_.clear();
    picked_.clear();
    pickedAt_.clear();
    typedValue_.reset();
    if (t != Tool::PickPoint && t != Tool::PickPoints) {
        onPoint_ = nullptr;
        onCloudPoint_ = nullptr;
        onPoints_ = nullptr;
    }
    tool_ = t;
    if (needsSelection() && selectedObjects().empty()) {
        ctx_.report(tr("%1: select the objects first, then start the tool.").arg(tr(toolName(t))), MessageLevel::Warning);
        tool_ = Tool::Select;
    }
    switch (tool_) {
    case Tool::Select: setPrompt({}); break;
    case Tool::Point: setPrompt(tr("POINT: click or type x,y,z")); break;
    case Tool::Line: setPrompt(tr("LINE: first point")); break;
    case Tool::Polyline: setPrompt(tr("POLYLINE: points; Enter/right-click finishes, C closes, Backspace undoes")); break;
    case Tool::Polygon: setPrompt(tr("POLYGON: vertices; Enter/right-click closes")); break;
    case Tool::Rectangle: setPrompt(tr("RECTANGLE: first corner")); break;
    case Tool::Circle: setPrompt(tr("CIRCLE: centre")); break;
    case Tool::Arc3: setPrompt(tr("ARC: start point")); break;
    case Tool::Spline: setPrompt(tr("SPLINE: fit points; Enter/right-click finishes")); break;
    case Tool::Text: setPrompt(tr("TEXT: insertion point (height %1 m)").arg(textHeight_)); break;
    case Tool::Dimension: setPrompt(tr("DIMENSION: first point")); break;
    case Tool::Measure: setPrompt(tr("MEASURE: first point")); break;
    case Tool::MeasureArea: setPrompt(tr("AREA: polygon vertices; Enter/right-click finishes")); break;
    case Tool::Move: setPrompt(tr("MOVE: base point")); break;
    case Tool::Copy: setPrompt(tr("COPY: base point")); break;
    case Tool::Rotate: setPrompt(tr("ROTATE: base point")); break;
    case Tool::Mirror: setPrompt(tr("MIRROR: first point of the mirror line")); break;
    case Tool::Scale: setPrompt(tr("SCALE: base point")); break;
    case Tool::Offset: setPrompt(tr("OFFSET (%1 m, type a value to change): pick the object").arg(offsetDistance_)); break;
    case Tool::Trim: setPrompt(tr("TRIM: pick the part to remove (cutting edges: selection, or all curves)")); break;
    case Tool::Extend: setPrompt(tr("EXTEND: pick near the end to extend (boundaries: selection, or all curves)")); break;
    case Tool::Fillet: setPrompt(tr("FILLET (r = %1 m, type a value to change): first object").arg(filletRadius_)); break;
    case Tool::Split: setPrompt(tr("SPLIT: pick the split point on a curve")); break;
    case Tool::PickPoint:
    case Tool::PickPoints: break; // prompt set by the request
    case Tool::SectionLine: setPrompt(tr("SECTION: first point of the section line")); break;
    case Tool::CropPolygon: setPrompt(tr("CROP: polygon vertices around the points to keep; Enter/right-click closes")); break;
    }
    emit toolChanged(tool_);
}

void ToolController::requestPoint(const QString& prompt, std::function<void(Vec3)> done, bool) {
    returnTo_ = tool_ == Tool::PickPoint || tool_ == Tool::PickPoints ? Tool::Select : tool_;
    setTool(Tool::PickPoint);
    onPoint_ = std::move(done);
    setPrompt(prompt);
}

void ToolController::requestCloudPoint(const QString& prompt, std::function<void(Uuid, Vec3, Vec3)> done) {
    returnTo_ = tool_ == Tool::PickPoint || tool_ == Tool::PickPoints ? Tool::Select : tool_;
    setTool(Tool::PickPoint);
    onCloudPoint_ = std::move(done);
    setPrompt(prompt);
}

void ToolController::requestPoints(const QString& prompt, std::function<void(std::vector<Vec3>)> done) {
    returnTo_ = Tool::Select;
    setTool(Tool::PickPoints);
    onPoints_ = std::move(done);
    setPrompt(prompt);
}

// ---- point resolution --------------------------------------------------------------------------------

std::optional<ToolController::Resolved> ToolController::resolve(Viewport3D* view, QPoint q) const {
    if (!ctx_.hasProject()) return std::nullopt;
    const auto plane = view->workPlanePoint(q);
    std::optional<Resolved> out;
    const std::optional<Vec3> from = pts_.empty() ? std::nullopt : std::optional<Vec3>(pts_.back());
    if (snapOn_) {
        // Candidates: objects whose screen-space bounds come within the aperture of the cursor.
        std::vector<const GeometryObject*> cands;
        const double ap = snap_.aperturePixels;
        const auto& m = ctx_.project().model();
        for (const auto& [id, b] : view->scene().geometryBounds()) {
            double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
            bool any = false;
            for (int i = 0; i < 8; ++i) {
                auto s = view->worldToWidget({(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z});
                if (!s) continue;
                any = true;
                x0 = std::min(x0, s->x());
                y0 = std::min(y0, s->y());
                x1 = std::max(x1, s->x());
                y1 = std::max(y1, s->y());
            }
            if (!any) continue;
            if (q.x() < x0 - ap || q.x() > x1 + ap || q.y() < y0 - ap || q.y() > y1 + ap) continue;
            if (const auto* g = m.geometry.find(id)) cands.push_back(g);
            if (cands.size() > 400) break;
        }
        auto project = [view](Vec3 w) -> Vec2 {
            auto s = view->worldToWidget(w);
            return s ? Vec2{s->x(), s->y()} : Vec2{1e9, 1e9};
        };
        auto settings = snap_;
        // Grid snapping only makes sense on the work plane.
        if (!plane) settings.modes &= ~static_cast<unsigned>(cad::SnapMode::Grid);
        if (auto s = cad::findSnap(cands, Vec2{static_cast<double>(q.x()), static_cast<double>(q.y())}, plane.value_or(Vec3{}), project, settings, from)) {
            if (s->mode != cad::SnapMode::Grid || plane) {
                Resolved r;
                r.point = s->point;
                r.label = qstr(s->label());
                r.objectId = s->objectId;
                out = r;
                if (s->mode == cad::SnapMode::Grid && surfaceSnap_) out.reset(); // let a surface win over the grid
                else return out;
            }
        }
    }
    if (surfaceSnap_) {
        if (auto hit = view->pickAt(q)) {
            Resolved r;
            r.point = hit->world;
            r.label = hit->onPointCloud ? tr("Point cloud") : tr("Surface");
            r.objectId = hit->geometryId;
            r.onCloud = hit->onPointCloud;
            r.cloudId = hit->onPointCloud ? hit->owner.id : Uuid{};
            r.stored = hit->stored;
            return r;
        }
    }
    if (out) return out; // grid snap
    if (plane) {
        Resolved r;
        r.point = *plane;
        r.label = tr("Plane");
        if (ortho_ && from) {
            const Vec3 d = r.point - *from;
            if (std::abs(d.x) >= std::abs(d.y)) r.point.y = from->y;
            else r.point.x = from->x;
            r.label = tr("Ortho");
        }
        return r;
    }
    return std::nullopt;
}

// ---- events --------------------------------------------------------------------------------------

void ToolController::press(Viewport3D* view, QPoint, bool, bool) { lastView_ = view; }

void ToolController::move(Viewport3D* view, QPoint q, bool, bool) {
    lastView_ = view;
    auto r = resolve(view, q);
    lastResolved_ = r;
    if (r) {
        Overlay snap;
        snap.points = {r->point};
        snap.color = r->label == tr("Plane") ? 0x9E9E9E : 0xFFEB3B;
        snap.pointSize = 11;
        if (r->label != tr("Plane")) snap.labels = {{r->point, r->label.toStdString()}};
        view->setOverlay("snap", snap);
        emit snapped(r->label, r->point);
        lastCursor_ = r->point;
    } else {
        view->clearOverlay("snap");
        lastCursor_.reset();
    }
    updatePreview(view, lastCursor_);
}

void ToolController::leave(Viewport3D* view) {
    view->clearOverlay("snap");
    lastCursor_.reset();
    updatePreview(view, std::nullopt);
}

std::optional<Uuid> ToolController::objectAt(Viewport3D* view, QPoint q) const {
    auto hit = view->pickAt(q, false);
    if (!hit) return std::nullopt;
    if (!hit->geometryId.isNil()) return hit->geometryId;
    if (hit->owner.kind == EntityKind::Geometry) return hit->owner.id;
    return std::nullopt;
}

bool ToolController::click(Viewport3D* view, QPoint q, bool, bool) {
    lastView_ = view;
    switch (tool_) {
    case Tool::Select: return false;
    case Tool::Offset:
    case Tool::Trim:
    case Tool::Extend:
    case Tool::Fillet:
    case Tool::Split: {
        // Object-picking tools.
        const auto obj = objectAt(view, q);
        const auto r = resolve(view, q);
        const Vec3 at = r ? r->point : view->workPlanePoint(q).value_or(Vec3{});
        if (tool_ == Tool::Offset && picked_.size() == 1) {
            // Second click: the side.
            pickedAt_.push_back(at);
            complete();
            return true;
        }
        if (!obj) {
            ctx_.report(tr("%1: no curve under the cursor").arg(tr(toolName(tool_))), MessageLevel::Warning);
            return true;
        }
        picked_.push_back(*obj);
        pickedAt_.push_back(at);
        if (tool_ == Tool::Offset) {
            setPrompt(tr("OFFSET: click the side to offset to"));
            return true;
        }
        if (tool_ == Tool::Fillet && picked_.size() == 1) {
            setPrompt(tr("FILLET: second object (the same polyline fillets its nearest corner)"));
            return true;
        }
        complete();
        return true;
    }
    default: break;
    }
    auto r = resolve(view, q);
    if (!r) {
        ctx_.report(tr("The point is not on the work plane, a surface or a snap point (rotate the view or type coordinates)"), MessageLevel::Warning);
        return true;
    }
    addPoint(view, *r);
    return true;
}

bool ToolController::rightClick(Viewport3D*, QPoint) {
    if (!isActive()) return false;
    finish();
    return true;
}

bool ToolController::key(Viewport3D*, const std::string& key, bool, bool) {
    if (!isActive()) return false;
    if (key == "Escape") {
        cancel();
        return true;
    }
    if (key == "Return" || key == "KP_Enter") {
        finish();
        return true;
    }
    if (key == "BackSpace") {
        undoLastPoint();
        return true;
    }
    if ((key == "c" || key == "C") && tool_ == Tool::Polyline && pts_.size() >= 3) {
        auto g = cad::makePolyline(pts_, true);
        createObject(std::move(g), tr("Draw closed polyline"));
        restart();
        return true;
    }
    return false;
}

void ToolController::undoLastPoint() {
    if (pts_.empty()) return;
    pts_.pop_back();
    if (lastView_) updatePreview(lastView_, lastCursor_);
}

void ToolController::cancel() {
    const bool wasPick = tool_ == Tool::PickPoint || tool_ == Tool::PickPoints;
    if (!pts_.empty() || !picked_.empty()) {
        pts_.clear();
        picked_.clear();
        pickedAt_.clear();
        clearPreview();
        ctx_.report(tr("%1 cancelled").arg(tr(toolName(tool_))));
        if (!wasPick) {
            restart();
            return;
        }
    }
    setTool(wasPick ? returnTo_ : Tool::Select);
}

void ToolController::finish() {
    switch (tool_) {
    case Tool::Polyline:
        if (pts_.size() >= 2) createObject(cad::makePolyline(pts_, false), tr("Draw polyline"));
        else ctx_.report(tr("A polyline needs at least two points"), MessageLevel::Warning);
        restart();
        break;
    case Tool::Polygon:
        if (pts_.size() >= 3) createObject(cad::makePolyline(pts_, true), tr("Draw polygon"));
        else ctx_.report(tr("A polygon needs at least three points"), MessageLevel::Warning);
        restart();
        break;
    case Tool::Spline:
        if (pts_.size() >= 2) createObject(cad::makeSpline(pts_, false), tr("Draw spline"));
        restart();
        break;
    case Tool::MeasureArea:
    case Tool::CropPolygon:
    case Tool::PickPoints: complete(); break;
    case Tool::Select: break;
    default: cancel(); break;
    }
}

void ToolController::addPoint(Viewport3D* view, const Resolved& r) {
    if (tool_ == Tool::PickPoint) {
        auto onPoint = std::move(onPoint_);
        auto onCloud = std::move(onCloudPoint_);
        if (onCloud) {
            if (!r.onCloud) {
                onCloudPoint_ = std::move(onCloud);
                ctx_.report(tr("Pick a point on a point cloud (zoom in until points are shown)"), MessageLevel::Warning);
                return;
            }
            setTool(returnTo_);
            onCloud(r.cloudId, r.stored, r.point);
            return;
        }
        setTool(returnTo_);
        if (onPoint) onPoint(r.point);
        return;
    }
    (void)view;
    addPoint(r.point);
}

void ToolController::addPoint(Vec3 p) {
    if (!pts_.empty() && distance(pts_.back(), p) < 1e-9) return; // double click
    pts_.push_back(p);
    const auto n = pts_.size();
    switch (tool_) {
    case Tool::Point:
        {
            GeometryObject g;
            g.type = GeometryType::Point;
            g.vertices = {p};
            createObject(std::move(g), tr("Draw point"));
        }
        restart();
        return;
    case Tool::Line:
        if (n == 2) {
            createObject(cad::makeLine(pts_[0], pts_[1]), tr("Draw line"));
            // Continue from the end point (CAD convention): the next line starts here.
            pts_ = {pts_[1]};
            setPrompt(tr("LINE: next point (Esc to stop)"));
        } else {
            setPrompt(tr("LINE: next point"));
        }
        break;
    case Tool::Rectangle:
        if (n == 2) {
            createObject(cad::makeRectangle(pts_[0], pts_[1]), tr("Draw rectangle"));
            restart();
        } else {
            setPrompt(tr("RECTANGLE: opposite corner"));
        }
        break;
    case Tool::Circle:
        if (n == 2) {
            const double r = horizontalDistance(pts_[0], pts_[1]);
            if (r > 1e-6) createObject(cad::makeCircle(pts_[0], r), tr("Draw circle"));
            restart();
        } else {
            setPrompt(tr("CIRCLE: point on the circle (or type the radius)"));
        }
        break;
    case Tool::Arc3:
        if (n == 3) {
            try {
                createObject(cad::makeArc3(pts_[0], pts_[1], pts_[2]), tr("Draw arc"));
            } catch (const std::exception& e) {
                ctx_.report(tr("Arc: %1").arg(qstr(e.what())), MessageLevel::Error);
            }
            restart();
        } else {
            setPrompt(n == 1 ? tr("ARC: point on the arc") : tr("ARC: end point"));
        }
        break;
    case Tool::Text: {
        bool ok = false;
        const QString text = QInputDialog::getText(nullptr, tr("Text"), tr("Text:"), QLineEdit::Normal, {}, &ok);
        if (ok && !text.isEmpty()) createObject(cad::makeText(pts_[0], sstr(text), textHeight_), tr("Add text"));
        restart();
        break;
    }
    case Tool::Dimension:
        if (n == 3) {
            createObject(cad::makeDimension(DimensionKind::Aligned, pts_, textHeight_ * 0.6, 3), tr("Add dimension"));
            restart();
        } else {
            setPrompt(n == 1 ? tr("DIMENSION: second point") : tr("DIMENSION: dimension line position"));
        }
        break;
    case Tool::Measure:
        if (n == 2) {
            const Vec3 a = pts_[0], b = pts_[1];
            const double d3 = distance(a, b), dh = horizontalDistance(a, b), dz = b.z - a.z;
            QString grade = dh > 1e-9 ? QStringLiteral("%1 %").arg(dz / dh * 100.0, 0, 'f', 2) : QStringLiteral("vertical");
            if (dh > 1e-9 && std::abs(dz) > 1e-9) grade += QStringLiteral(" (1 in %1)").arg(std::abs(dh / dz), 0, 'f', 1);
            const QString text = tr("Distance %1 m, plan %2 m, ΔZ %3 m, grade %4, bearing %5°")
                                     .arg(fmt(d3), fmt(dh), fmt(dz), grade, fmt(planBearingDeg(a, b), 2));
            ctx_.report(text);
            emit measured(text);
            if (lastView_) {
                Overlay o;
                o.lines = {{a, b}};
                o.points = {a, b};
                o.color = 0xFF7043;
                o.labels = {{lerp(a, b, 0.5), fmt(d3) .toStdString() + " m"}};
                lastView_->setOverlay("measure", o);
            }
            pts_.clear();
            setPrompt(tr("MEASURE: first point (Esc to stop)"));
        } else {
            setPrompt(tr("MEASURE: second point"));
        }
        break;
    case Tool::Move:
    case Tool::Copy:
        if (n == 2) complete();
        else setPrompt(tr("%1: destination point (or type @dx,dy,dz)").arg(tool_ == Tool::Move ? "MOVE" : "COPY"));
        break;
    case Tool::Rotate:
        if (n == 2 || (n == 1 && typedValue_)) complete();
        else setPrompt(tr("ROTATE: point giving the angle (from east, counter-clockwise) or type degrees"));
        break;
    case Tool::Mirror:
        if (n == 2) complete();
        else setPrompt(tr("MIRROR: second point of the mirror line"));
        break;
    case Tool::Scale:
        if (n == 1 && typedValue_) complete();
        else if (n == 2) complete();
        else setPrompt(tr("SCALE: type the factor (or click: distance from the base point in metres is the factor)"));
        break;
    case Tool::SectionLine:
        if (n == 2) complete();
        else setPrompt(tr("SECTION: second point of the section line"));
        break;
    case Tool::Polyline:
    case Tool::Polygon:
    case Tool::Spline:
    case Tool::MeasureArea:
    case Tool::CropPolygon:
    case Tool::PickPoints:
        if (tool_ == Tool::PickPoints) ctx_.report(tr("Point %1: %2, %3, %4").arg(n).arg(fmt(p.x), fmt(p.y), fmt(p.z)));
        break;
    default: break;
    }
    if (lastView_) updatePreview(lastView_, lastCursor_);
}

bool ToolController::acceptPoint(Vec3 w) {
    if (!isActive()) return false;
    if (tool_ == Tool::PickPoint) {
        Resolved r;
        r.point = w;
        addPoint(lastView_, r);
        return true;
    }
    addPoint(w);
    return true;
}

bool ToolController::acceptValue(double v) {
    if (!isActive()) return false;
    switch (tool_) {
    case Tool::Offset:
        if (v <= 0) return false;
        offsetDistance_ = v;
        setPrompt(tr("OFFSET (%1 m): pick the object").arg(v));
        return true;
    case Tool::Fillet:
        if (v < 0) return false;
        filletRadius_ = v;
        setPrompt(tr("FILLET (r = %1 m): first object").arg(v));
        return true;
    case Tool::Text:
        if (v <= 0) return false;
        textHeight_ = v;
        setPrompt(tr("TEXT: insertion point (height %1 m)").arg(v));
        return true;
    case Tool::Circle:
        if (pts_.size() == 1 && v > 0) {
            createObject(cad::makeCircle(pts_[0], v), tr("Draw circle"));
            restart();
            return true;
        }
        return false;
    case Tool::Rotate:
    case Tool::Scale:
        typedValue_ = v;
        if (!pts_.empty()) complete();
        else setPrompt(tr("%1 %2: base point").arg(tool_ == Tool::Rotate ? "ROTATE" : "SCALE").arg(v));
        return true;
    default: break;
    }
    // Distance along the current direction (polyline/line drawing): from the last point towards the cursor.
    if (!pts_.empty() && lastCursor_ && v > 0) {
        const Vec3 d = (*lastCursor_ - pts_.back());
        if (d.length() > 1e-9) {
            addPoint(pts_.back() + d.normalized() * v);
            return true;
        }
    }
    return false;
}

bool ToolController::acceptInput(const QString& text) {
    if (!isActive()) return false;
    const QString t = text.trimmed();
    static const QRegularExpression num(QStringLiteral("^[-+]?\\d*\\.?\\d+(?:[eE][-+]?\\d+)?$"));
    static const QRegularExpression abs3(QStringLiteral("^([-+]?[\\d.eE+-]+)\\s*[, ]\\s*([-+]?[\\d.eE+-]+)(?:\\s*[, ]\\s*([-+]?[\\d.eE+-]+))?$"));
    static const QRegularExpression rel(QStringLiteral("^@([-+]?[\\d.eE+-]+)\\s*,\\s*([-+]?[\\d.eE+-]+)(?:\\s*,\\s*([-+]?[\\d.eE+-]+))?$"));
    static const QRegularExpression polar(QStringLiteral("^@([-+]?[\\d.eE+-]+)\\s*<\\s*([-+]?[\\d.eE+-]+)$"));
    if (num.match(t).hasMatch()) return acceptValue(t.toDouble());
    if (auto m = rel.match(t); m.hasMatch()) {
        if (pts_.empty()) {
            ctx_.report(tr("Relative coordinates need a previous point"), MessageLevel::Warning);
            return true;
        }
        const Vec3 d{m.captured(1).toDouble(), m.captured(2).toDouble(), m.captured(3).isEmpty() ? 0.0 : m.captured(3).toDouble()};
        return acceptPoint(pts_.back() + d);
    }
    if (auto m = polar.match(t); m.hasMatch()) {
        if (pts_.empty()) {
            ctx_.report(tr("Polar coordinates need a previous point"), MessageLevel::Warning);
            return true;
        }
        const double dist = m.captured(1).toDouble(), brg = m.captured(2).toDouble() * kDeg;
        return acceptPoint(pts_.back() + Vec3{dist * std::sin(brg), dist * std::cos(brg), 0});
    }
    if (auto m = abs3.match(t); m.hasMatch()) {
        double z = m.captured(3).isEmpty() ? (lastView_ ? lastView_->workPlaneZ() : 0.0) : m.captured(3).toDouble();
        return acceptPoint({m.captured(1).toDouble(), m.captured(2).toDouble(), z});
    }
    return false;
}

// ---- completion ------------------------------------------------------------------------------------

void ToolController::restart() {
    pts_.clear();
    picked_.clear();
    pickedAt_.clear();
    typedValue_.reset();
    clearPreview();
    const Tool t = tool_;
    if (t == Tool::Select) return;
    // Re-issue the start prompt of the same tool.
    const auto keepReturn = returnTo_;
    auto p = std::move(onPoints_);
    setTool(t);
    onPoints_ = std::move(p);
    returnTo_ = keepReturn;
}

void ToolController::createObject(GeometryObject g, const QString& what) {
    const Uuid layer = ctx_.currentLayer();
    if (layer.isNil()) {
        ctx_.report(tr("%1: there is no unlocked layer to draw on").arg(what), MessageLevel::Error);
        return;
    }
    g.layerId = layer;
    ctx_.tryEdit(what, [&] { ProjectEditor(ctx_.project()).addGeometry(std::move(g), sstr(what)); });
}

void ToolController::applyTransform(const Transform3& t, bool copy, const QString& what) {
    const auto objs = selectedObjects();
    if (objs.empty()) return;
    ctx_.tryEdit(what, [&] {
        auto& p = ctx_.project();
        CommandStack::MacroScope macro(p.commands(), sstr(what));
        ProjectEditor ed(p);
        std::vector<SelectionItem> created;
        for (const auto* g : objs) {
            if (g->type == GeometryType::Solid) {
#ifdef OPENMINE_APP_HAVE_OCCT
                const Uuid id = occt::transformSolidObject(p, g->id, t, copy);
                if (copy) created.push_back({EntityKind::Geometry, id});
#else
                throw ValidationError("Solids can only be transformed in builds with Open CASCADE");
#endif
                continue;
            }
            GeometryObject n = cad::transformed(*g, t);
            if (copy) {
                n.id = {};
                n.attributes = g->attributes;
                created.push_back({EntityKind::Geometry, ed.addGeometry(std::move(n))});
            } else {
                // Centrelines: flag linked schedule quantities (never change them silently).
                mine_design::updateGeometryWithImpact(p, n, sstr(what));
            }
        }
        macro.commit();
        if (copy) ctx_.selectMany(created);
    });
}

void ToolController::complete() {
    auto& m = ctx_.project().model();
    switch (tool_) {
    case Tool::Move:
    case Tool::Copy: applyTransform(Transform3::translation(pts_[1] - pts_[0]), tool_ == Tool::Copy, tr(toolName(tool_))); break;
    case Tool::Rotate: {
        double ang = 0;
        if (typedValue_) ang = *typedValue_ * kDeg;
        else ang = std::atan2(pts_[1].y - pts_[0].y, pts_[1].x - pts_[0].x);
        applyTransform(Transform3::rotation({0, 0, 1}, ang, pts_[0]), false, tr("Rotate"));
        break;
    }
    case Tool::Mirror: {
        const Vec3 d = pts_[1] - pts_[0];
        Vec3 n{-d.y, d.x, 0};
        if (n.length() < 1e-9) {
            ctx_.report(tr("Mirror line has no length"), MessageLevel::Warning);
            break;
        }
        applyTransform(Transform3::mirror(pts_[0], n.normalized()), false, tr("Mirror"));
        break;
    }
    case Tool::Scale: {
        const double f = typedValue_ ? *typedValue_ : (pts_.size() >= 2 ? distance(pts_[0], pts_[1]) : 0.0);
        if (!(f > 0)) {
            ctx_.report(tr("Scale factor must be positive"), MessageLevel::Warning);
            break;
        }
        applyTransform(Transform3::scaling(f, pts_[0]), false, tr("Scale"));
        break;
    }
    case Tool::Offset: {
        const auto* g = m.geometry.find(picked_.at(0));
        if (!g) break;
        ctx_.tryEdit(tr("Offset"), [&] {
            auto o = cad::offset(*g, offsetDistance_, pickedAt_.at(1));
            o.id = {};
            o.layerId = g->layerId;
            ProjectEditor(ctx_.project()).addGeometry(std::move(o), "Offset");
        });
        break;
    }
    case Tool::Trim:
    case Tool::Extend: {
        const auto* target = m.geometry.find(picked_.at(0));
        if (!target) break;
        std::vector<const GeometryObject*> edges;
        for (const auto* g : selectedObjects())
            if (g->id != target->id && geometry::isCurve(g->type)) edges.push_back(g);
        if (edges.empty())
            for (const auto& g : m.geometry.all())
                if (g.id != target->id && geometry::isCurve(g.type)) {
                    const auto* layer = m.layers.find(g.layerId);
                    if (!layer || layer->visible) edges.push_back(&g);
                }
        const bool trim = tool_ == Tool::Trim;
        ctx_.tryEdit(trim ? tr("Trim") : tr("Extend"), [&] {
            auto& p = ctx_.project();
            if (trim) {
                auto pieces = cad::trim(*target, edges, pickedAt_.at(0));
                CommandStack::MacroScope macro(p.commands(), "Trim");
                ProjectEditor ed(p);
                if (pieces.empty()) {
                    ed.removeGeometry(target->id);
                } else {
                    // The first piece keeps the object's identity (and any development link).
                    GeometryObject first = pieces.front();
                    first.id = target->id;
                    first.meta = target->meta;
                    mine_design::updateGeometryWithImpact(p, first, "Trim");
                    for (std::size_t i = 1; i < pieces.size(); ++i) {
                        auto n = pieces[i];
                        n.id = {};
                        ed.addGeometry(std::move(n));
                    }
                }
                macro.commit();
            } else {
                auto ext = cad::extend(*target, edges, pickedAt_.at(0));
                ext.id = target->id;
                ext.meta = target->meta;
                mine_design::updateGeometryWithImpact(p, ext, "Extend");
            }
        });
        break;
    }
    case Tool::Fillet: {
        if (picked_.size() < 2) break;
        const auto* a = m.geometry.find(picked_[0]);
        const auto* b = m.geometry.find(picked_[1]);
        if (!a || !b) break;
        ctx_.tryEdit(tr("Fillet"), [&] {
            auto& p = ctx_.project();
            CommandStack::MacroScope macro(p.commands(), "Fillet");
            ProjectEditor ed(p);
            if (a->id == b->id) {
                auto f = cad::filletPolylineVertex(*a, pickedAt_[1], filletRadius_);
                f.id = a->id;
                f.meta = a->meta;
                mine_design::updateGeometryWithImpact(p, f, "Fillet");
            } else {
                auto r = cad::fillet(*a, pickedAt_[0], *b, pickedAt_[1], filletRadius_);
                r.first.id = a->id;
                r.first.meta = a->meta;
                r.second.id = b->id;
                r.second.meta = b->meta;
                mine_design::updateGeometryWithImpact(p, r.first, "Fillet");
                mine_design::updateGeometryWithImpact(p, r.second, "Fillet");
                if (r.join) {
                    auto j = *r.join;
                    j.id = {};
                    j.layerId = a->layerId;
                    ed.addGeometry(std::move(j));
                }
            }
            macro.commit();
        });
        break;
    }
    case Tool::Split: {
        const auto* g = m.geometry.find(picked_.at(0));
        if (!g) break;
        ctx_.tryEdit(tr("Split"), [&] {
            auto& p = ctx_.project();
            auto pieces = cad::split(*g, pickedAt_.at(0));
            CommandStack::MacroScope macro(p.commands(), "Split");
            ProjectEditor ed(p);
            GeometryObject first = pieces.at(0);
            first.id = g->id;
            first.meta = g->meta;
            mine_design::updateGeometryWithImpact(p, first, "Split");
            for (std::size_t i = 1; i < pieces.size(); ++i) {
                auto n = pieces[i];
                n.id = {};
                ed.addGeometry(std::move(n));
            }
            macro.commit();
        });
        break;
    }
    case Tool::MeasureArea: {
        if (pts_.size() < 3) {
            ctx_.report(tr("An area needs at least three points"), MessageLevel::Warning);
            break;
        }
        geometry::Ring ring;
        for (const auto& p : pts_) ring.push_back(xy(p));
        double perim = 0;
        for (std::size_t i = 0; i < pts_.size(); ++i) perim += horizontalDistance(pts_[i], pts_[(i + 1) % pts_.size()]);
        const QString text = tr("Plan area %1 m², plan perimeter %2 m (%3 vertices)")
                                 .arg(fmt(std::abs(geometry::signedArea(ring)), 2), fmt(perim, 3)).arg(pts_.size());
        ctx_.report(text);
        emit measured(text);
        if (lastView_) {
            Overlay o;
            auto loop = pts_;
            loop.push_back(pts_.front());
            o.lines = {loop};
            o.color = 0xFF7043;
            o.labels = {{pts_.front(), fmt(std::abs(geometry::signedArea(ring)), 1).toStdString() + " m²"}};
            lastView_->setOverlay("measure", o);
        }
        break;
    }
    case Tool::SectionLine: {
        const Vec3 a = pts_[0], b = pts_[1];
        Vec3 d{b.x - a.x, b.y - a.y, 0};
        if (d.length() < 1e-6) {
            ctx_.report(tr("Section line has no plan length"), MessageLevel::Warning);
            break;
        }
        d = d.normalized();
        // The plane contains the line and is vertical; its normal is horizontal, to the left of a→b.
        emit sectionDefined(lerp(a, b, 0.5), Vec3{-d.y, d.x, 0}, horizontalDistance(a, b));
        clearPreview();
        setTool(Tool::Select);
        return;
    }
    case Tool::CropPolygon: {
        if (pts_.size() < 3) {
            ctx_.report(tr("A crop polygon needs at least three points"), MessageLevel::Warning);
            break;
        }
        Vec3 n{0, 0, 1};
        if (lastView_) n = lastView_->workPlane().second;
        auto poly = pts_;
        clearPreview();
        setTool(Tool::Select);
        emit polygonDefined(poly, n);
        return;
    }
    case Tool::PickPoints: {
        auto cb = std::move(onPoints_);
        auto poly = pts_;
        clearPreview();
        setTool(returnTo_);
        if (cb) cb(poly);
        return;
    }
    default: break;
    }
    restart();
}

// ---- preview -----------------------------------------------------------------------------------------

void ToolController::clearPreview() {
    if (lastView_) {
        lastView_->clearOverlay("preview");
        lastView_->clearOverlay("snap");
    }
}

void ToolController::updatePreview(Viewport3D* view, std::optional<Vec3> cursor) {
    if (!view) return;
    Overlay o;
    o.color = 0x00B4FF;
    o.lineWidth = 2.0;
    auto chain = pts_;
    if (cursor) chain.push_back(*cursor);
    switch (tool_) {
    case Tool::Line:
    case Tool::Polyline:
    case Tool::Spline:
    case Tool::Measure:
    case Tool::Dimension:
    case Tool::Mirror:
    case Tool::SectionLine:
    case Tool::PickPoints:
        if (chain.size() >= 2) o.lines = {chain};
        o.points = pts_;
        if (tool_ == Tool::Spline && chain.size() >= 2) o.lines = {geometry::splinePoints(chain, false)};
        if (tool_ == Tool::Mirror || tool_ == Tool::SectionLine) o.dashed = true;
        break;
    case Tool::Polygon:
    case Tool::MeasureArea:
    case Tool::CropPolygon:
        if (chain.size() >= 2) {
            auto loop = chain;
            if (chain.size() >= 3) loop.push_back(chain.front());
            o.lines = {loop};
        }
        o.points = pts_;
        break;
    case Tool::Rectangle:
        if (chain.size() == 2) {
            const Vec3 a = chain[0], b = chain[1];
            o.lines = {{a, {b.x, a.y, a.z}, {b.x, b.y, a.z}, {a.x, b.y, a.z}, a}};
        }
        break;
    case Tool::Circle:
        if (chain.size() == 2) {
            const double r = horizontalDistance(chain[0], chain[1]);
            if (r > 1e-9) o.lines = {geometry::arcPoints(geometry::planArc(chain[0], r, 0, 2 * std::numbers::pi), r * 0.002)};
            o.points = {chain[0]};
        }
        break;
    case Tool::Arc3:
        if (chain.size() == 3) {
            if (auto a = geometry::arcThroughPoints(chain[0], chain[1], chain[2])) o.lines = {geometry::arcPoints(*a, a->radius * 0.002)};
        } else if (chain.size() == 2) {
            o.lines = {chain};
        }
        o.points = pts_;
        break;
    case Tool::Move:
    case Tool::Copy:
    case Tool::Rotate:
    case Tool::Scale:
        if (!pts_.empty() && cursor) {
            Transform3 t;
            if (tool_ == Tool::Move || tool_ == Tool::Copy) t = Transform3::translation(*cursor - pts_[0]);
            else if (tool_ == Tool::Rotate) t = Transform3::rotation({0, 0, 1}, std::atan2(cursor->y - pts_[0].y, cursor->x - pts_[0].x), pts_[0]);
            else {
                const double f = distance(pts_[0], *cursor);
                if (f > 1e-6) t = Transform3::scaling(f, pts_[0]);
            }
            for (const auto* g : selectedObjects()) {
                for (auto line : geometry::tessellate(*g, 0.05)) {
                    for (auto& p : line) p = t.apply(p);
                    o.lines.push_back(std::move(line));
                }
                if (o.lines.size() > 500) break;
            }
            o.lines.push_back({pts_[0], *cursor});
            o.dashed = false;
            o.points = {pts_[0]};
        }
        break;
    default: break;
    }
    if (o.lines.empty() && o.points.empty()) view->clearOverlay("preview");
    else view->setOverlay("preview", o);
}

} // namespace openmine::app
