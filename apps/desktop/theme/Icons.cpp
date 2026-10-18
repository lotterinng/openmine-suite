// SPDX-License-Identifier: Apache-2.0
// OpenMine's own tool and entity icons, drawn programmatically in the theme colours (no
// third-party artwork). Each icon is a 64×64 vector drawing; icons are cached per theme.

#include "Theme.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <array>
#include <cmath>
#include <functional>
#include <numbers>

namespace openmine::app {

namespace {

struct Ink {
    QPainter& p;
    const ThemeColors& c;
    QPen line() const { return QPen(c.text, 4.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin); }
    QPen thin() const { return QPen(c.text, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin); }
    QPen accent() const { return QPen(c.accent, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin); }
    QPen dashed() const { return QPen(c.textMuted, 3, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin); }
    void dot(QPointF pt, const QColor& col, double r = 5.5) {
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawEllipse(pt, r, r);
        p.setBrush(Qt::NoBrush);
    }
    void poly(std::initializer_list<QPointF> pts, const QPen& pen, bool closed = false) {
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPolygonF pg(pts);
        if (closed) p.drawPolygon(pg);
        else p.drawPolyline(pg);
    }
    void fill(std::initializer_list<QPointF> pts, const QColor& col, const QPen& pen = Qt::NoPen) {
        p.setPen(pen);
        p.setBrush(col);
        p.drawPolygon(QPolygonF(pts));
        p.setBrush(Qt::NoBrush);
    }
    void text(const QRectF& r, const QString& t, int px, const QColor& col, bool bold = true) {
        QFont f = p.font();
        f.setPixelSize(px);
        f.setBold(bold);
        p.setFont(f);
        p.setPen(col);
        p.drawText(r, Qt::AlignCenter, t);
    }
    void arrowHead(QPointF tip, double angleDeg, const QPen& pen) {
        const double a = angleDeg * std::numbers::pi / 180;
        const QPointF l = tip - QPointF(std::cos(a - 0.5) * 10, std::sin(a - 0.5) * 10);
        const QPointF r = tip - QPointF(std::cos(a + 0.5) * 10, std::sin(a + 0.5) * 10);
        poly({l, tip, r}, pen);
    }
};

using Painter = std::function<void(Ink&)>;

const QHash<QString, Painter>& painters() {
    static const QHash<QString, Painter> h = {
        // ---- file / edit ----------------------------------------------------------------------
        {"new", [](Ink& k) {
             k.poly({{16, 8}, {40, 8}, {50, 18}, {50, 56}, {16, 56}}, k.line(), true);
             k.poly({{40, 8}, {40, 18}, {50, 18}}, k.thin());
             k.poly({{33, 28}, {33, 46}}, k.accent());
             k.poly({{24, 37}, {42, 37}}, k.accent());
         }},
        {"open", [](Ink& k) {
             k.poly({{8, 16}, {24, 16}, {30, 22}, {54, 22}, {54, 52}, {8, 52}}, k.line(), true);
             k.fill({{8, 52}, {16, 30}, {60, 30}, {54, 52}}, k.c.accent);
         }},
        {"save", [](Ink& k) {
             k.poly({{10, 10}, {46, 10}, {54, 18}, {54, 54}, {10, 54}}, k.line(), true);
             k.fill({{20, 10}, {42, 10}, {42, 24}, {20, 24}}, k.c.accent);
             k.poly({{20, 38}, {44, 38}, {44, 54}, {20, 54}}, k.thin(), true);
         }},
        {"undo", [](Ink& k) {
             QPainterPath p;
             p.moveTo(18, 26);
             p.cubicTo(30, 12, 54, 18, 52, 36);
             p.cubicTo(50, 50, 34, 54, 24, 48);
             k.p.setPen(k.line());
             k.p.drawPath(p);
             k.fill({{8, 26}, {24, 16}, {24, 34}}, k.c.accent);
         }},
        {"redo", [](Ink& k) {
             QPainterPath p;
             p.moveTo(46, 26);
             p.cubicTo(34, 12, 10, 18, 12, 36);
             p.cubicTo(14, 50, 30, 54, 40, 48);
             k.p.setPen(k.line());
             k.p.drawPath(p);
             k.fill({{56, 26}, {40, 16}, {40, 34}}, k.c.accent);
         }},
        {"delete", [](Ink& k) {
             k.poly({{14, 14}, {50, 50}}, k.accent());
             k.poly({{50, 14}, {14, 50}}, k.accent());
         }},
        {"import", [](Ink& k) {
             k.poly({{10, 40}, {10, 54}, {54, 54}, {54, 40}}, k.line());
             k.poly({{32, 8}, {32, 40}}, k.accent());
             k.poly({{20, 28}, {32, 40}, {44, 28}}, k.accent());
         }},
        {"export", [](Ink& k) {
             k.poly({{10, 40}, {10, 54}, {54, 54}, {54, 40}}, k.line());
             k.poly({{32, 40}, {32, 8}}, k.accent());
             k.poly({{20, 20}, {32, 8}, {44, 20}}, k.accent());
         }},
        {"properties", [](Ink& k) {
             for (int y : {16, 32, 48}) {
                 k.poly({{10, double(y)}, {54, double(y)}}, k.thin());
                 k.dot({y == 32 ? 40.0 : 22.0, double(y)}, k.c.accent, 6);
             }
         }},
        // ---- selection / view -----------------------------------------------------------------
        {"select", [](Ink& k) {
             k.fill({{16, 8}, {16, 50}, {27, 40}, {35, 56}, {42, 53}, {34, 37}, {48, 37}}, k.c.text);
         }},
        {"fit", [](Ink& k) {
             for (auto [x, y, dx, dy] : {std::array<int, 4>{8, 8, 1, 1}, {56, 8, -1, 1}, {8, 56, 1, -1}, {56, 56, -1, -1}})
                 k.poly({{double(x), double(y + 12 * dy)}, {double(x), double(y)}, {double(x + 12 * dx), double(y)}}, k.line());
             k.p.setPen(k.accent());
             k.p.drawRect(QRectF(22, 22, 20, 20));
         }},
        {"zoomin", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawEllipse(QPointF(26, 26), 16, 16);
             k.poly({{38, 38}, {54, 54}}, k.line());
             k.poly({{18, 26}, {34, 26}}, k.accent());
             k.poly({{26, 18}, {26, 34}}, k.accent());
         }},
        {"zoomout", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawEllipse(QPointF(26, 26), 16, 16);
             k.poly({{38, 38}, {54, 54}}, k.line());
             k.poly({{18, 26}, {34, 26}}, k.accent());
         }},
        {"plan", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRect(QRectF(10, 10, 44, 44));
             k.poly({{32, 18}, {32, 46}}, k.dashed());
             k.poly({{18, 32}, {46, 32}}, k.dashed());
             k.text(QRectF(10, 10, 44, 44), "N", 18, k.c.accent);
         }},
        {"elevation", [](Ink& k) {
             k.poly({{8, 50}, {56, 50}}, k.line());
             k.fill({{14, 50}, {24, 26}, {34, 38}, {44, 18}, {52, 50}}, k.c.accent);
         }},
        {"section", [](Ink& k) {
             k.poly({{8, 32}, {56, 32}}, QPen(k.c.accent, 4, Qt::DashDotLine));
             k.fill({{20, 18}, {44, 18}, {50, 46}, {14, 46}}, k.c.panelAlt, k.line());
             k.arrowHead({8, 20}, 270, k.accent());
             k.poly({{8, 32}, {8, 20}}, k.accent());
             k.arrowHead({56, 20}, 270, k.accent());
             k.poly({{56, 32}, {56, 20}}, k.accent());
         }},
        {"perspective", [](Ink& k) {
             k.poly({{12, 50}, {24, 14}, {40, 14}, {52, 50}}, k.line(), true);
             k.poly({{18, 32}, {46, 32}}, k.dashed());
         }},
        {"view1", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRect(QRectF(10, 12, 44, 40));
         }},
        {"view2", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRect(QRectF(10, 12, 44, 40));
             k.poly({{32, 12}, {32, 52}}, k.accent());
         }},
        {"view2h", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRect(QRectF(10, 12, 44, 40));
             k.poly({{10, 32}, {54, 32}}, k.accent());
         }},
        {"view4", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRect(QRectF(10, 12, 44, 40));
             k.poly({{32, 12}, {32, 52}}, k.accent());
             k.poly({{10, 32}, {54, 32}}, k.accent());
         }},
        {"theme", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawEllipse(QPointF(32, 32), 22, 22);
             k.p.setBrush(k.c.accent);
             k.p.drawPie(QRectF(10, 10, 44, 44), 90 * 16, 180 * 16);
         }},
        {"top", [](Ink& k) {
             k.fill({{32, 10}, {54, 22}, {32, 34}, {10, 22}}, k.c.accent, k.thin());
             k.fill({{10, 22}, {32, 34}, {32, 56}, {10, 44}}, k.c.panelAlt, k.thin());
             k.fill({{54, 22}, {32, 34}, {32, 56}, {54, 44}}, k.c.panelAlt, k.thin());
         }},
        {"front", [](Ink& k) {
             k.fill({{32, 10}, {54, 22}, {32, 34}, {10, 22}}, k.c.panelAlt, k.thin());
             k.fill({{10, 22}, {32, 34}, {32, 56}, {10, 44}}, k.c.accent, k.thin());
             k.fill({{54, 22}, {32, 34}, {32, 56}, {54, 44}}, k.c.panelAlt, k.thin());
         }},
        {"side", [](Ink& k) {
             k.fill({{32, 10}, {54, 22}, {32, 34}, {10, 22}}, k.c.panelAlt, k.thin());
             k.fill({{10, 22}, {32, 34}, {32, 56}, {10, 44}}, k.c.panelAlt, k.thin());
             k.fill({{54, 22}, {32, 34}, {32, 56}, {54, 44}}, k.c.accent, k.thin());
         }},
        {"iso", [](Ink& k) {
             k.fill({{32, 10}, {54, 22}, {32, 34}, {10, 22}}, k.c.accent, k.thin());
             k.fill({{10, 22}, {32, 34}, {32, 56}, {10, 44}}, k.c.accent.darker(130), k.thin());
             k.fill({{54, 22}, {32, 34}, {32, 56}, {54, 44}}, k.c.accent.darker(170), k.thin());
         }},
        // ---- drawing ---------------------------------------------------------------------------
        {"point", [](Ink& k) {
             k.poly({{20, 32}, {44, 32}}, k.thin());
             k.poly({{32, 20}, {32, 44}}, k.thin());
             k.dot({32, 32}, k.c.accent, 7);
         }},
        {"line", [](Ink& k) {
             k.poly({{12, 52}, {52, 12}}, k.line());
             k.dot({12, 52}, k.c.accent);
             k.dot({52, 12}, k.c.accent);
         }},
        {"polyline", [](Ink& k) {
             k.poly({{8, 50}, {22, 18}, {40, 40}, {56, 12}}, k.line());
             for (QPointF pt : {QPointF(8, 50), QPointF(22, 18), QPointF(40, 40), QPointF(56, 12)}) k.dot(pt, k.c.accent);
         }},
        {"polygon", [](Ink& k) {
             k.poly({{12, 48}, {16, 16}, {44, 10}, {54, 38}, {32, 54}}, k.line(), true);
             for (QPointF pt : {QPointF(12, 48), QPointF(16, 16), QPointF(44, 10), QPointF(54, 38), QPointF(32, 54)}) k.dot(pt, k.c.accent, 4.5);
         }},
        {"rectangle", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRect(QRectF(10, 16, 44, 32));
             k.dot({10, 16}, k.c.accent);
             k.dot({54, 48}, k.c.accent);
         }},
        {"circle", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawEllipse(QPointF(32, 32), 22, 22);
             k.dot({32, 32}, k.c.accent);
             k.poly({{32, 32}, {54, 32}}, k.dashed());
         }},
        {"arc", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawArc(QRectF(10, 14, 44, 44), 20 * 16, 140 * 16);
             for (QPointF pt : {QPointF(52.7, 28.5), QPointF(32, 14), QPointF(11.3, 28.5)}) k.dot(pt, k.c.accent);
         }},
        {"spline", [](Ink& k) {
             QPainterPath p;
             p.moveTo(8, 46);
             p.cubicTo(18, 6, 30, 58, 40, 30);
             p.cubicTo(46, 14, 52, 16, 56, 20);
             k.p.setPen(k.line());
             k.p.drawPath(p);
             for (QPointF pt : {QPointF(8, 46), QPointF(24, 30), QPointF(40, 30), QPointF(56, 20)}) k.dot(pt, k.c.accent, 4.5);
         }},
        {"text", [](Ink& k) { k.text(QRectF(4, 4, 56, 56), "A", 40, k.c.text); }},
        {"dimension", [](Ink& k) {
             k.poly({{12, 18}, {12, 50}}, k.thin());
             k.poly({{52, 18}, {52, 50}}, k.thin());
             k.poly({{12, 26}, {52, 26}}, k.accent());
             k.arrowHead({12, 26}, 180, k.accent());
             k.arrowHead({52, 26}, 0, k.accent());
             k.text(QRectF(12, 30, 40, 22), "12.5", 13, k.c.text);
         }},
        {"hatch", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRect(QRectF(10, 10, 44, 44));
             for (int i = 0; i < 5; ++i) k.poly({{10.0 + i * 11, 54}, {10.0 + i * 11 + 22, 32}}, k.thin());
         }},
        {"distance", [](Ink& k) {
             k.poly({{8, 44}, {56, 44}}, k.line());
             for (int x = 8; x <= 56; x += 8) k.poly({{double(x), 44}, {double(x), x % 16 == 8 ? 32.0 : 38.0}}, k.thin());
             k.poly({{12, 20}, {52, 20}}, k.accent());
             k.arrowHead({12, 20}, 180, k.accent());
             k.arrowHead({52, 20}, 0, k.accent());
         }},
        {"area", [](Ink& k) {
             k.fill({{10, 50}, {16, 14}, {46, 10}, {54, 42}}, QColor(k.c.accent.red(), k.c.accent.green(), k.c.accent.blue(), 110), k.line());
             k.text(QRectF(10, 10, 44, 44), "m²", 16, k.c.text);
         }},
        {"snap", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRect(QRectF(20, 20, 24, 24));
             k.poly({{8, 56}, {32, 32}}, k.accent());
         }},
        {"ortho", [](Ink& k) {
             k.poly({{14, 10}, {14, 50}, {54, 50}}, k.line());
             k.p.setPen(k.accent());
             k.p.drawRect(QRectF(14, 38, 12, 12));
         }},
        {"grid", [](Ink& k) {
             for (int i = 0; i < 4; ++i) {
                 k.poly({{10.0 + i * 14.6, 10}, {10.0 + i * 14.6, 54}}, k.thin());
                 k.poly({{10, 10.0 + i * 14.6}, {54, 10.0 + i * 14.6}}, k.thin());
             }
             k.dot({24.6, 39.2}, k.c.accent);
         }},
        // ---- editing ---------------------------------------------------------------------------
        {"move", [](Ink& k) {
             k.poly({{32, 8}, {32, 56}}, k.line());
             k.poly({{8, 32}, {56, 32}}, k.line());
             for (auto [x, y, a] : {std::array<double, 3>{32, 8, 270}, {32, 56, 90}, {8, 32, 180}, {56, 32, 0}}) k.arrowHead({x, y}, a, k.accent());
         }},
        {"copy", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRect(QRectF(8, 8, 30, 30));
             k.p.setPen(k.accent());
             k.p.drawRect(QRectF(26, 26, 30, 30));
         }},
        {"rotate", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawArc(QRectF(10, 10, 44, 44), 30 * 16, 280 * 16);
             k.arrowHead({51, 21}, 300, k.accent());
             k.dot({32, 32}, k.c.accent);
         }},
        {"mirror", [](Ink& k) {
             k.poly({{32, 6}, {32, 58}}, QPen(k.c.accent, 3, Qt::DashDotLine));
             k.fill({{26, 14}, {26, 50}, {8, 50}}, k.c.text);
             k.poly({{38, 14}, {38, 50}, {56, 50}}, k.line(), true);
         }},
        {"scale", [](Ink& k) {
             k.p.setPen(k.thin());
             k.p.drawRect(QRectF(8, 30, 26, 26));
             k.p.setPen(k.line());
             k.p.drawRect(QRectF(8, 8, 48, 48));
             k.poly({{24, 40}, {48, 16}}, k.accent());
             k.arrowHead({48, 16}, 315, k.accent());
         }},
        {"offset", [](Ink& k) {
             k.poly({{8, 46}, {24, 22}, {56, 22}}, k.line());
             k.poly({{8, 58}, {28, 32}, {56, 32}}, k.accent());
         }},
        {"trim", [](Ink& k) {
             k.poly({{32, 6}, {32, 58}}, k.line());
             k.poly({{8, 32}, {32, 32}}, k.accent());
             k.poly({{32, 32}, {56, 32}}, k.dashed());
             k.poly({{44, 24}, {52, 40}}, QPen(k.c.warning, 4));
             k.poly({{52, 24}, {44, 40}}, QPen(k.c.warning, 4));
         }},
        {"extend", [](Ink& k) {
             k.poly({{52, 6}, {52, 58}}, k.line());
             k.poly({{8, 32}, {30, 32}}, k.accent());
             k.poly({{30, 32}, {52, 32}}, QPen(k.c.accent, 3, Qt::DotLine));
             k.arrowHead({50, 32}, 0, k.accent());
         }},
        {"fillet", [](Ink& k) {
             k.poly({{10, 54}, {10, 30}}, k.line());
             k.poly({{34, 10}, {56, 10}}, k.line());
             k.p.setPen(k.accent());
             k.p.drawArc(QRectF(10, 10, 48, 40), 90 * 16, 90 * 16);
         }},
        {"split", [](Ink& k) {
             k.poly({{8, 32}, {28, 32}}, k.line());
             k.poly({{36, 32}, {56, 32}}, k.line());
             k.poly({{32, 12}, {32, 52}}, k.accent());
         }},
        {"join", [](Ink& k) {
             k.poly({{8, 40}, {30, 24}}, k.line());
             k.poly({{34, 24}, {56, 40}}, k.line());
             k.dot({32, 24}, k.c.accent, 7);
         }},
        {"explode", [](Ink& k) {
             k.poly({{20, 44}, {12, 52}}, k.line());
             k.poly({{44, 44}, {52, 52}}, k.line());
             k.poly({{20, 20}, {12, 12}}, k.line());
             k.poly({{44, 20}, {52, 12}}, k.line());
             k.dot({32, 32}, k.c.accent, 8);
         }},
        // ---- survey ----------------------------------------------------------------------------
        {"cloud", [](Ink& k) {
             static const std::array<QPointF, 30> pts = [] {
                 std::array<QPointF, 30> a{};
                 for (int i = 0; i < 30; ++i) a[static_cast<std::size_t>(i)] = QPointF(12 + (i * 37) % 41, 14 + (i * 23 + i * i) % 37);
                 return a;
             }();
             for (std::size_t i = 0; i < pts.size(); ++i) k.dot(pts[i], i % 3 ? k.c.text : k.c.accent, 2.6);
         }},
        {"control", [](Ink& k) {
             k.fill({{32, 8}, {54, 50}, {10, 50}}, Qt::transparent, k.line());
             k.dot({32, 36}, k.c.accent, 6);
         }},
        {"register", [](Ink& k) {
             for (auto [x, y] : {std::pair{18.0, 22.0}, {28.0, 14.0}, {14.0, 36.0}}) k.dot({x, y}, k.c.text, 4);
             for (auto [x, y] : {std::pair{40.0, 44.0}, {50.0, 36.0}, {36.0, 58.0}}) k.dot({x, y}, k.c.accent, 4);
             k.poly({{22, 30}, {40, 46}}, k.dashed());
             k.arrowHead({40, 46}, 42, k.accent());
         }},
        {"icp", [](Ink& k) {
             QPainterPath a, b;
             a.moveTo(6, 40);
             a.cubicTo(20, 18, 40, 18, 58, 34);
             b.moveTo(6, 48);
             b.cubicTo(20, 26, 40, 26, 58, 42);
             k.p.setPen(k.line());
             k.p.drawPath(a);
             k.p.setPen(k.accent());
             k.p.drawPath(b);
             k.arrowHead({32, 26}, 270, k.thin());
         }},
        {"mesh", [](Ink& k) {
             const QPointF v[] = {{8, 46}, {22, 20}, {36, 44}, {48, 16}, {58, 40}, {26, 56}};
             k.p.setPen(k.thin());
             for (auto [a, b] : {std::pair{0, 1}, {1, 2}, {0, 2}, {1, 3}, {2, 3}, {3, 4}, {2, 4}, {0, 5}, {2, 5}}) k.p.drawLine(v[a], v[b]);
             k.fill({v[1], v[2], v[3]}, QColor(k.c.accent.red(), k.c.accent.green(), k.c.accent.blue(), 150));
         }},
        {"surface", [](Ink& k) {
             QPainterPath p;
             p.moveTo(6, 44);
             p.cubicTo(18, 24, 30, 50, 42, 30);
             p.cubicTo(48, 20, 54, 26, 58, 30);
             p.lineTo(58, 56);
             p.lineTo(6, 56);
             p.closeSubpath();
             k.p.setPen(k.line());
             k.p.setBrush(QColor(k.c.accent.red(), k.c.accent.green(), k.c.accent.blue(), 130));
             k.p.drawPath(p);
         }},
        {"volume", [](Ink& k) {
             k.fill({{8, 42}, {24, 26}, {40, 34}, {56, 18}, {56, 50}, {8, 50}}, k.c.accent, k.line());
             k.poly({{8, 34}, {56, 34}}, k.dashed());
         }},
        {"filter", [](Ink& k) { k.fill({{8, 10}, {56, 10}, {36, 32}, {36, 54}, {28, 48}, {28, 32}}, Qt::transparent, k.line()); }},
        {"normals", [](Ink& k) {
             QPainterPath p;
             p.moveTo(6, 48);
             p.cubicTo(20, 30, 44, 30, 58, 48);
             k.p.setPen(k.line());
             k.p.drawPath(p);
             for (auto [x, y, a] : {std::array<double, 3>{14, 40, 240}, {32, 34, 270}, {50, 40, 300}}) {
                 const double r = a * std::numbers::pi / 180;
                 const QPointF tip(x + std::cos(r) * 20, y + std::sin(r) * 20);
                 k.poly({{x, y}, tip}, k.accent());
             }
         }},
        {"ground", [](Ink& k) {
             k.fill({{6, 46}, {20, 38}, {36, 44}, {58, 34}, {58, 56}, {6, 56}}, QColor(0xA0, 0x70, 0x2A), k.thin());
             for (auto [x, y] : {std::pair{16.0, 22.0}, {30.0, 16.0}, {44.0, 24.0}}) k.dot({x, y}, QColor(0x3C, 0xB0, 0x43), 5);
         }},
        {"conformance", [](Ink& k) {
             QPainterPath d;
             d.moveTo(12, 54);
             d.lineTo(12, 30);
             d.arcTo(QRectF(12, 10, 40, 40), 180, -180);
             d.lineTo(52, 54);
             k.p.setPen(k.dashed());
             k.p.drawPath(d);
             QPainterPath a;
             a.moveTo(8, 54);
             a.lineTo(9, 28);
             a.cubicTo(12, 4, 54, 6, 55, 28);
             a.lineTo(56, 54);
             k.p.setPen(k.accent());
             k.p.drawPath(a);
         }},
        {"face", [](Ink& k) {
             k.poly({{8, 32}, {40, 32}}, k.line());
             k.p.setPen(k.accent());
             k.p.drawRect(QRectF(40, 16, 10, 32));
             k.arrowHead({38, 32}, 0, k.line());
         }},
        // ---- design ----------------------------------------------------------------------------
        {"heading", [](Ink& k) {
             k.poly({{8, 54}, {8, 26}}, k.line());
             k.p.setPen(k.line());
             k.p.drawArc(QRectF(8, 8, 48, 36), 0, 180 * 16);
             k.poly({{56, 26}, {56, 54}, {8, 54}}, k.line());
             k.poly({{32, 54}, {32, 32}}, QPen(k.c.accent, 4, Qt::DashLine));
         }},
        {"decline", [](Ink& k) {
             k.poly({{8, 12}, {48, 20}}, k.line());
             k.p.setPen(k.line());
             k.p.drawArc(QRectF(36, 20, 20, 16), 270 * 16, 180 * 16);
             k.poly({{48, 36}, {16, 42}}, k.line());
             k.p.drawArc(QRectF(8, 42, 20, 14), 90 * 16, 180 * 16);
             k.poly({{18, 56}, {56, 58}}, k.accent());
         }},
        {"crosscut", [](Ink& k) {
             k.poly({{8, 32}, {56, 32}}, k.line());
             for (int x : {18, 32, 46}) k.poly({{double(x), 32}, {double(x), 8}}, k.accent());
         }},
        {"oredrive", [](Ink& k) {
             k.poly({{8, 20}, {30, 14}, {56, 22}}, QPen(k.c.warning, 4, Qt::DashLine));
             k.poly({{8, 32}, {30, 26}, {56, 34}}, k.line());
         }},
        {"access", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawArc(QRectF(-8, 16, 48, 48), 0, 90 * 16);
             k.poly({{16, 16}, {56, 12}}, k.line());
             k.dot({56, 12}, k.c.accent, 6);
         }},
        {"ramp", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawEllipse(QRectF(12, 8, 40, 16));
             k.p.drawEllipse(QRectF(12, 24, 40, 16));
             k.p.setPen(k.accent());
             k.p.drawEllipse(QRectF(12, 40, 40, 16));
         }},
        {"level", [](Ink& k) {
             k.poly({{8, 20}, {56, 20}}, k.dashed());
             k.poly({{8, 44}, {56, 44}}, k.line());
             k.text(QRectF(8, 22, 48, 20), "RL", 14, k.c.accent);
         }},
        {"stope", [](Ink& k) {
             k.fill({{14, 14}, {44, 8}, {52, 46}, {20, 56}}, QColor(k.c.accent.red(), k.c.accent.green(), k.c.accent.blue(), 160), k.line());
             k.poly({{17, 35}, {48, 27}}, k.dashed());
         }},
        {"solid", [](Ink& k) {
             k.fill({{32, 8}, {56, 20}, {32, 32}, {8, 20}}, k.c.accent, k.thin());
             k.fill({{8, 20}, {32, 32}, {32, 58}, {8, 46}}, k.c.accent.darker(140), k.thin());
             k.fill({{56, 20}, {32, 32}, {32, 58}, {56, 46}}, k.c.accent.darker(180), k.thin());
         }},
        {"boolean", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawEllipse(QPointF(24, 32), 16, 16);
             k.p.setBrush(QColor(k.c.accent.red(), k.c.accent.green(), k.c.accent.blue(), 140));
             k.p.drawEllipse(QPointF(40, 32), 16, 16);
         }},
        {"revision", [](Ink& k) {
             k.poly({{14, 8}, {44, 8}, {50, 14}, {50, 56}, {14, 56}}, k.line(), true);
             k.text(QRectF(14, 18, 36, 30), "B", 22, k.c.accent);
         }},
        {"link", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRoundedRect(QRectF(6, 22, 28, 20), 10, 10);
             k.p.setPen(k.accent());
             k.p.drawRoundedRect(QRectF(30, 22, 28, 20), 10, 10);
         }},
        {"impacts", [](Ink& k) {
             k.fill({{32, 8}, {58, 54}, {6, 54}}, k.c.warning, k.thin());
             k.text(QRectF(6, 18, 52, 36), "!", 26, QColor(20, 20, 20));
         }},
        // ---- schedule --------------------------------------------------------------------------
        {"gantt", [](Ink& k) {
             k.p.setPen(Qt::NoPen);
             k.p.setBrush(k.c.text);
             k.p.drawRect(QRectF(8, 12, 26, 8));
             k.p.setBrush(k.c.accent);
             k.p.drawRect(QRectF(22, 28, 28, 8));
             k.p.setBrush(k.c.text);
             k.p.drawRect(QRectF(36, 44, 20, 8));
         }},
        {"task", [](Ink& k) {
             k.p.setPen(Qt::NoPen);
             k.p.setBrush(k.c.accent);
             k.p.drawRoundedRect(QRectF(8, 24, 48, 16), 3, 3);
         }},
        {"activity", [](Ink& k) {
             k.p.setPen(Qt::NoPen);
             k.p.setBrush(k.c.text);
             k.p.drawRect(QRectF(8, 24, 48, 16));
             k.p.setBrush(k.c.accent);
             k.p.drawRect(QRectF(8, 24, 28, 16));
         }},
        {"milestone", [](Ink& k) { k.fill({{32, 10}, {54, 32}, {32, 54}, {10, 32}}, k.c.accent); }},
        {"summary", [](Ink& k) {
             k.fill({{8, 22}, {56, 22}, {56, 34}, {50, 40}, {44, 34}, {20, 34}, {14, 40}, {8, 34}}, k.c.text);
         }},
        {"dependency", [](Ink& k) {
             k.p.setPen(Qt::NoPen);
             k.p.setBrush(k.c.text);
             k.p.drawRect(QRectF(6, 12, 24, 10));
             k.p.drawRect(QRectF(34, 42, 24, 10));
             k.poly({{30, 17}, {40, 17}, {40, 38}}, k.accent());
             k.arrowHead({40, 40}, 90, k.accent());
         }},
        {"calc", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawEllipse(QPointF(32, 32), 22, 22);
             k.poly({{32, 32}, {32, 16}}, k.accent());
             k.poly({{32, 32}, {44, 38}}, k.accent());
         }},
        {"baseline", [](Ink& k) {
             k.p.setPen(Qt::NoPen);
             k.p.setBrush(k.c.textMuted);
             k.p.drawRect(QRectF(8, 36, 40, 6));
             k.p.setBrush(k.c.accent);
             k.p.drawRect(QRectF(16, 20, 40, 12));
         }},
        {"resource", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawEllipse(QPointF(32, 20), 10, 10);
             QPainterPath p;
             p.moveTo(12, 56);
             p.cubicTo(14, 34, 50, 34, 52, 56);
             k.p.setPen(k.accent());
             k.p.drawPath(p);
         }},
        {"calendar", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRect(QRectF(8, 12, 48, 44));
             k.p.fillRect(QRectF(8, 12, 48, 10), k.c.accent);
             for (int r = 0; r < 3; ++r)
                 for (int c = 0; c < 4; ++c) k.dot({16.0 + c * 11, 30.0 + r * 9}, k.c.text, 2.5);
         }},
        {"scenario", [](Ink& k) {
             k.poly({{8, 32}, {28, 32}}, k.line());
             k.poly({{28, 32}, {54, 14}}, k.line());
             k.poly({{28, 32}, {54, 50}}, k.accent());
             k.dot({28, 32}, k.c.accent);
         }},
        {"levelling", [](Ink& k) {
             k.p.setPen(Qt::NoPen);
             k.p.setBrush(k.c.textMuted);
             k.p.drawRect(QRectF(8, 18, 10, 36));
             k.p.drawRect(QRectF(22, 30, 10, 24));
             k.p.setBrush(k.c.accent);
             k.p.drawRect(QRectF(36, 34, 10, 20));
             k.p.drawRect(QRectF(50, 34, 8, 20));
             k.poly({{6, 32}, {58, 32}}, QPen(k.c.warning, 2.5, Qt::DashLine));
         }},
        {"optimise", [](Ink& k) {
             k.poly({{8, 48}, {20, 30}, {32, 38}, {44, 16}, {56, 22}}, k.line());
             k.p.setPen(k.accent());
             k.p.drawEllipse(QPointF(44, 16), 7, 7);
         }},
        {"histogram", [](Ink& k) {
             k.p.setPen(Qt::NoPen);
             const double hs[] = {16, 30, 42, 24, 12};
             for (int i = 0; i < 5; ++i) {
                 k.p.setBrush(i == 2 ? k.c.accent : k.c.text);
                 k.p.drawRect(QRectF(8 + i * 10, 56 - hs[i], 8, hs[i]));
             }
         }},
        {"chart", [](Ink& k) {
             k.poly({{8, 8}, {8, 56}, {56, 56}}, k.thin());
             k.poly({{12, 48}, {24, 36}, {36, 40}, {52, 16}}, k.accent());
         }},
        {"actual", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRect(QRectF(10, 10, 44, 44));
             k.poly({{18, 32}, {28, 44}, {48, 20}}, k.accent());
         }},
        {"play", [](Ink& k) { k.fill({{18, 10}, {52, 32}, {18, 54}}, k.c.accent); }},
        {"pause", [](Ink& k) {
             k.p.fillRect(QRectF(16, 12, 10, 40), k.c.accent);
             k.p.fillRect(QRectF(38, 12, 10, 40), k.c.accent);
         }},
        {"stop", [](Ink& k) { k.p.fillRect(QRectF(14, 14, 36, 36), k.c.accent); }},
        {"4d", [](Ink& k) {
             k.fill({{32, 8}, {54, 20}, {32, 32}, {10, 20}}, k.c.panelAlt, k.thin());
             k.fill({{10, 20}, {32, 32}, {32, 56}, {10, 44}}, k.c.accent, k.thin());
             k.fill({{54, 20}, {32, 32}, {32, 56}, {54, 44}}, k.c.panelAlt, k.thin());
             k.text(QRectF(34, 30, 24, 22), "t", 18, k.c.text);
         }},
        // ---- geology / geotechnical ---------------------------------------------------------
        {"drillhole", [](Ink& k) {
             k.poly({{18, 8}, {22, 24}, {30, 40}, {44, 56}}, k.line());
             k.poly({{20, 16}, {23, 27}}, QPen(k.c.accent, 7, Qt::SolidLine, Qt::FlatCap));
             k.poly({{28, 37}, {34, 45}}, QPen(k.c.warning, 7, Qt::SolidLine, Qt::FlatCap));
         }},
        {"blocks", [](Ink& k) {
             for (int i = 0; i < 3; ++i)
                 for (int j = 0; j < 3; ++j) {
                     const QColor col = (i + j) % 3 == 0 ? k.c.accent : (i + j) % 3 == 1 ? k.c.accent.darker(150) : k.c.panelAlt;
                     k.p.fillRect(QRectF(10 + i * 15, 10 + j * 15, 13, 13), col);
                 }
         }},
        {"estimate", [](Ink& k) {
             for (auto [x, y] : {std::pair{12.0, 14.0}, {50.0, 18.0}, {16.0, 50.0}, {48.0, 48.0}}) k.dot({x, y}, k.c.text, 4.5);
             k.p.fillRect(QRectF(26, 26, 12, 12), k.c.accent);
             for (auto [x, y] : {std::pair{12.0, 14.0}, {50.0, 18.0}, {16.0, 50.0}, {48.0, 48.0}}) k.poly({{x, y}, {32, 32}}, k.dashed());
         }},
        {"variogram", [](Ink& k) {
             k.poly({{8, 8}, {8, 56}, {56, 56}}, k.thin());
             QPainterPath p;
             p.moveTo(8, 50);
             p.cubicTo(20, 20, 30, 16, 56, 16);
             k.p.setPen(k.accent());
             k.p.drawPath(p);
             for (auto [x, y] : {std::pair{14.0, 40.0}, {22.0, 28.0}, {32.0, 20.0}, {46.0, 17.0}}) k.dot({x, y}, k.c.text, 3.5);
         }},
        {"structure", [](Ink& k) {
             k.fill({{8, 40}, {40, 16}, {56, 26}, {24, 50}}, QColor(k.c.accent.red(), k.c.accent.green(), k.c.accent.blue(), 160), k.line());
             k.poly({{32, 32}, {44, 52}}, k.thin());
         }},
        {"stereonet", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawEllipse(QPointF(32, 32), 24, 24);
             k.poly({{32, 8}, {32, 56}}, k.dashed());
             k.poly({{8, 32}, {56, 32}}, k.dashed());
             k.p.setPen(k.accent());
             k.p.drawArc(QRectF(14, 8, 36, 48), 90 * 16, 180 * 16);
             k.dot({40, 22}, k.c.accent, 4);
         }},
        {"geotech", [](Ink& k) {
             k.fill({{8, 56}, {8, 34}, {22, 26}, {34, 34}, {48, 22}, {56, 30}, {56, 56}}, k.c.panelAlt, k.line());
             k.text(QRectF(8, 30, 48, 26), "Q", 18, k.c.accent);
         }},
        // ---- reports / tools -------------------------------------------------------------------
        {"report", [](Ink& k) {
             k.poly({{14, 8}, {40, 8}, {50, 18}, {50, 56}, {14, 56}}, k.line(), true);
             for (int y : {26, 34, 42}) k.poly({{20, double(y)}, {44, double(y)}}, k.thin());
             k.p.fillRect(QRectF(20, 46, 10, 6), k.c.accent);
         }},
        {"pdf", [](Ink& k) {
             k.poly({{14, 8}, {40, 8}, {50, 18}, {50, 56}, {14, 56}}, k.line(), true);
             k.text(QRectF(12, 22, 40, 26), "PDF", 13, k.c.accent);
         }},
        {"xlsx", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRect(QRectF(8, 12, 48, 40));
             for (int x : {24, 40}) k.poly({{double(x), 12}, {double(x), 52}}, k.thin());
             for (int y : {25, 38}) k.poly({{8, double(y)}, {56, double(y)}}, k.thin());
             k.p.fillRect(QRectF(25, 26, 14, 11), k.c.accent);
         }},
        {"crs", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawEllipse(QPointF(32, 32), 22, 22);
             k.p.drawEllipse(QRectF(22, 10, 20, 44));
             k.poly({{10, 32}, {54, 32}}, k.thin());
             k.poly({{40, 50}, {56, 56}}, k.accent());
         }},
        {"python", [](Ink& k) {
             k.p.setPen(Qt::NoPen);
             k.p.setBrush(QColor(0x37, 0x76, 0xAB));
             k.p.drawRoundedRect(QRectF(10, 8, 26, 28), 7, 7);
             k.p.setBrush(QColor(0xFF, 0xD3, 0x43));
             k.p.drawRoundedRect(QRectF(28, 28, 26, 28), 7, 7);
             k.dot({18, 15}, Qt::white, 3);
             k.dot({46, 49}, Qt::white, 3);
         }},
        {"console", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawRoundedRect(QRectF(6, 10, 52, 44), 5, 5);
             k.poly({{14, 24}, {24, 32}, {14, 40}}, k.accent());
             k.poly({{28, 42}, {44, 42}}, k.thin());
         }},
        {"plugin", [](Ink& k) {
             k.fill({{12, 22}, {24, 22}, {24, 12}, {36, 12}, {36, 22}, {52, 22}, {52, 54}, {12, 54}}, k.c.panelAlt, k.line());
             k.dot({32, 38}, k.c.accent, 7);
         }},
        {"jobs", [](Ink& k) {
             for (int i = 0; i < 3; ++i) {
                 k.p.fillRect(QRectF(8, 12 + i * 16, 48, 10), k.c.panelAlt);
                 k.p.fillRect(QRectF(8, 12 + i * 16, 48.0 * (3 - i) / 3.6, 10), k.c.accent);
             }
         }},
        {"layer", [](Ink& k) {
             k.fill({{32, 36}, {58, 46}, {32, 56}, {6, 46}}, k.c.panelAlt, k.thin());
             k.fill({{32, 22}, {58, 32}, {32, 42}, {6, 32}}, k.c.accent.darker(140), k.thin());
             k.fill({{32, 8}, {58, 18}, {32, 28}, {6, 18}}, k.c.accent, k.thin());
         }},
        {"eye", [](Ink& k) {
             QPainterPath p;
             p.moveTo(6, 32);
             p.quadTo(32, 6, 58, 32);
             p.quadTo(32, 58, 6, 32);
             k.p.setPen(k.line());
             k.p.drawPath(p);
             k.dot({32, 32}, k.c.accent, 8);
         }},
        {"lock", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawArc(QRectF(20, 8, 24, 28), 0, 180 * 16);
             k.poly({{20, 22}, {20, 28}}, k.line());
             k.poly({{44, 22}, {44, 28}}, k.line());
             k.p.fillRect(QRectF(14, 28, 36, 26), k.c.accent);
         }},
        {"entity", [](Ink& k) {
             k.p.setPen(k.thin());
             k.p.drawRoundedRect(QRectF(12, 12, 40, 40), 6, 6);
             k.dot({32, 32}, k.c.accent, 6);
         }},
        {"folder", [](Ink& k) {
             k.fill({{8, 16}, {24, 16}, {30, 22}, {56, 22}, {56, 52}, {8, 52}}, k.c.panelAlt, k.thin());
         }},
        {"info", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawEllipse(QPointF(32, 32), 22, 22);
             k.text(QRectF(10, 10, 44, 44), "i", 26, k.c.accent);
         }},
        {"help", [](Ink& k) {
             k.p.setPen(k.line());
             k.p.drawEllipse(QPointF(32, 32), 22, 22);
             k.text(QRectF(10, 10, 44, 44), "?", 26, k.c.accent);
         }},
        {"settings", [](Ink& k) {
             k.p.setPen(QPen(k.c.text, 8, Qt::SolidLine, Qt::FlatCap));
             for (int i = 0; i < 8; ++i) {
                 const double a = i * std::numbers::pi / 4;
                 k.p.drawLine(QPointF(32 + std::cos(a) * 16, 32 + std::sin(a) * 16), QPointF(32 + std::cos(a) * 24, 32 + std::sin(a) * 24));
             }
             k.p.setPen(k.line());
             k.p.drawEllipse(QPointF(32, 32), 14, 14);
             k.dot({32, 32}, k.c.accent, 6);
         }},
    };
    return h;
}

} // namespace

QIcon toolIcon(const QString& name) {
    static QHash<QString, QIcon> cache;
    static ThemeKind cachedKind = Theme::current();
    if (cachedKind != Theme::current()) {
        cache.clear();
        cachedKind = Theme::current();
    }
    if (auto it = cache.find(name); it != cache.end()) return *it;
    const auto& c = Theme::colors();
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    Ink ink{p, c};
    p.setBrush(Qt::NoBrush);
    if (auto it = painters().find(name); it != painters().end()) {
        it.value()(ink);
    } else {
        // Initials on a tile for names without a drawing.
        p.setPen(QPen(c.textMuted, 3));
        p.drawRoundedRect(QRectF(8, 8, 48, 48), 8, 8);
        ink.text(QRectF(8, 8, 48, 48), name.left(2).toUpper(), 20, c.text);
    }
    p.end();
    QIcon icon(pm);
    cache.insert(name, icon);
    return icon;
}

bool hasToolIcon(const QString& name) { return painters().contains(name); }

} // namespace openmine::app
