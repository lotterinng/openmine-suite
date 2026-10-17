// SPDX-License-Identifier: Apache-2.0
#include "openmine/occt/SweptSolid.h"

#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>

namespace openmine::occt {

namespace {
TopoDS_Wire makeWire(std::span<const Vec3> pts) {
    BRepBuilderAPI_MakePolygon poly;
    for (const auto& p : pts) poly.Add(gp_Pnt(p.x, p.y, p.z));
    poly.Build();
    if (!poly.IsDone()) throw Standard_Failure("Cannot build wire from polyline");
    return poly.Wire();
}
} // namespace

double wireLength(std::span<const Vec3> pts) {
    if (pts.size() < 2) return 0.0;
    GProp_GProps props;
    BRepGProp::LinearProperties(makeWire(pts), props);
    return props.Mass();
}

SolidResult sweepRectangularProfile(std::span<const Vec3> pts, double width, double height) {
    SolidResult r;
    if (pts.size() < 2) {
        r.error = "Centreline needs at least two vertices";
        return r;
    }
    if (!(width > 0) || !(height > 0)) {
        r.error = "Profile width and height must be positive";
        return r;
    }
    try {
        const TopoDS_Wire spine = makeWire(pts);

        const gp_Pnt p0(pts[0].x, pts[0].y, pts[0].z);
        const gp_Vec t(p0, gp_Pnt(pts[1].x, pts[1].y, pts[1].z));
        if (t.Magnitude() < 1e-9) {
            r.error = "First centreline segment has zero length";
            return r;
        }
        const gp_Dir tangent(t);
        gp_Vec up(0, 0, 1);
        up = up - gp_Vec(tangent) * up.Dot(gp_Vec(tangent)); // project Z onto the profile plane
        if (up.Magnitude() < 1e-9) {
            r.error = "Vertical first segment (shaft) is not supported by the rectangular sweep";
            return r;
        }
        const gp_Dir upDir(up);
        const gp_Dir side = tangent.Crossed(upDir);

        auto corner = [&](double s, double u) {
            return p0.Translated(gp_Vec(side) * s + gp_Vec(upDir) * u);
        };
        const double hw = width / 2.0;
        // Profile: floor at the centreline grade, walls to `height`. (Centreline = floor centre.)
        BRepBuilderAPI_MakePolygon profile(corner(-hw, 0), corner(hw, 0), corner(hw, height), corner(-hw, height), Standard_True);

        BRepOffsetAPI_MakePipeShell pipe(spine);
        pipe.SetMode(gp_Dir(0, 0, 1)); // keep the profile's up-axis fixed relative to +Z (binormal mode)
        pipe.SetTransitionMode(BRepBuilderAPI_RightCorner);
        pipe.Add(profile.Wire());
        pipe.Build();
        if (!pipe.IsDone()) {
            r.error = "Sweep failed";
            return r;
        }
        if (!pipe.MakeSolid()) {
            r.error = "Sweep did not close into a solid";
            return r;
        }
        const TopoDS_Shape solid = pipe.Shape();
        r.valid = BRepCheck_Analyzer(solid).IsValid();
        GProp_GProps vprops, sprops;
        BRepGProp::VolumeProperties(solid, vprops);
        BRepGProp::SurfaceProperties(solid, sprops);
        r.volume = std::abs(vprops.Mass());
        r.surfaceArea = sprops.Mass();
        r.ok = true;
    } catch (const Standard_Failure& e) {
        r.error = std::string("Open CASCADE error: ") + (e.GetMessageString() ? e.GetMessageString() : "unknown");
    }
    return r;
}

} // namespace openmine::occt
