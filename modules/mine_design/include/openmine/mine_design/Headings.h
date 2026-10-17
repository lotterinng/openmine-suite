#pragma once
// SPDX-License-Identifier: Apache-2.0
// Heading design tools: gradient analysis and design-rule checks, grading, vertical alignment,
// crosscuts, ore drives along a contact, access drives (arc + straight connections), ramps
// between levels, level assignment, and creation of the corresponding development headings.
// Layout results are bulged polylines (exact plan arcs) in project coordinates.

#include "openmine/mine_design/Development.h"
#include "openmine/mine_design/Layout.h"

namespace openmine::mine_design {

// ---- Gradients and design rules --------------------------------------------------------------------
struct GradeSegment {
    std::size_t index = 0;           ///< segment index in the polyline
    double chainageFrom = 0, chainageTo = 0; ///< 3D chainage
    double planLength = 0, rise = 0;
    double gradePercent = 0;         ///< rise / plan length × 100 (negative = down)
    double radius = 0;               ///< plan radius of an arc segment (0 = straight)
    std::string oneIn() const;       ///< "1 in 8.0" (empty for level segments)
};
std::vector<GradeSegment> gradientProfile(const std::vector<Vec3>& vertices, const std::vector<double>& bulges = {});

struct DesignRules {
    double maxGradePercent = 15.0;     ///< |grade| limit on straights (e.g. 1:7 ≈ 14.3 %)
    double maxGradeOnCurvesPercent = 12.5;
    double minRadius = 20.0;           ///< m, plan
    double maxGradeChangePercent = 5.0;///< between consecutive segments without a vertical curve
};
struct RuleViolation {
    double chainage = 0;
    std::string rule;
    double value = 0, limit = 0;
};
std::vector<RuleViolation> checkDesignRules(const std::vector<Vec3>& vertices, const std::vector<double>& bulges, const DesignRules& rules);

/// Sets elevations by constant grade from `startElevation` along plan chainage (bulges respected).
std::vector<Vec3> applyGrade(std::vector<Vec3> vertices, const std::vector<double>& bulges, double startElevation, double gradePercent);
/// Constant grade between the existing first and last elevations.
std::vector<Vec3> gradeBetweenEnds(std::vector<Vec3> vertices, const std::vector<double>& bulges);

/// Vertical alignment: points of vertical intersection by plan chainage with symmetric parabolic
/// vertical curves of the given length centred on each interior VPI.
struct Vpi {
    double chainage = 0;   ///< plan chainage along the horizontal alignment
    double elevation = 0;
    double curveLength = 0;///< m (0 = sharp grade change)
};
/// Applies the vertical alignment to a horizontal alignment (densified at `step` m so that the
/// parabolas are represented; arcs keep their bulges where no vertical curve falls on them).
LayoutResult verticalAlignment(const std::vector<Vec3>& horizontal, const std::vector<double>& bulges, const std::vector<Vpi>& vpis, double step = 1.0);
/// Elevation of a vertical alignment at a plan chainage.
double verticalElevation(const std::vector<Vpi>& vpis, double chainage);

// ---- Layout generators ------------------------------------------------------------------------------
enum class Side { Left, Right, Both };

struct CrosscutSpec {
    std::vector<double> chainages;   ///< 3D chainages on the parent; empty → every `interval`
    double interval = 25;
    double firstChainage = 10;
    double length = 30;              ///< plan length from the parent centreline
    Side side = Side::Right;
    double angleDeg = 90;            ///< from the parent's direction of travel
    double gradePercent = 1.0;       ///< drainage grade (positive = rising away from the parent)
    double startOffset = 0;          ///< start this far from the centreline (e.g. half the drive width)
};
std::vector<LayoutResult> crosscutLayouts(const std::vector<Vec3>& parent, const std::vector<double>& parentBulges, const CrosscutSpec& spec);

struct OreDriveSpec {
    double offset = 5.0;             ///< plan distance from the contact string to the drive centreline
    Side side = Side::Left;          ///< side of the contact (relative to its direction)
    double gradePercent = 1.0;       ///< constant grade from the start elevation…
    std::optional<double> startElevation; ///< …default: the contact's first elevation
    bool followContactElevation = false;  ///< use the contact's elevations instead (offset in plan only)
};
/// Ore drive parallel to an orebody contact string (mitred plan offset of the contact).
LayoutResult oreDriveLayout(const std::vector<Vec3>& contact, const OreDriveSpec& spec);

/// Access drive from a start pose (point + bearing, e.g. a stub off the decline) to a target
/// point: a plan arc of `radius` turning towards the target, then a straight, at the constant grade
/// that reaches the target elevation. Throws ValidationError if the target lies inside the turning
/// circle or the grade exceeds `maxGradePercent`.
LayoutResult accessDriveLayout(Vec3 start, double startAzimuthDeg, Vec3 target, double radius, double maxGradePercent = 15.0);

struct RampSpec {
    Vec3 start;
    double startAzimuthDeg = 0;
    double targetElevation = 0;
    double gradePercent = -14.3;
    double radius = 20;
    TurnDirection turn = TurnDirection::Right;
    double straightLength = 40;      ///< 0 → spiral
};
LayoutResult rampLayout(const RampSpec& spec);

// ---- Project integration -----------------------------------------------------------------------------
/// Creates a centreline geometry (bulged polyline) and a development heading for each layout
/// (one undo step). Heading ids are `prefix` + 2-digit sequence when several are created.
std::vector<Uuid> createHeadings(Project& project, const std::vector<LayoutResult>& layouts, const NewDevelopment& templ, const Uuid& layerId,
                                 const std::string& headingPrefix, const std::string& commandText = "Create headings");

/// Crosscuts off an existing development (activity type "Crosscut", zone/level/profile inherited).
std::vector<Uuid> createCrosscuts(Project& project, const Uuid& parentDevelopmentId, const CrosscutSpec& spec, const Uuid& layerId);

/// Assigns developments to the level whose elevation is closest to their mean centreline
/// elevation (within `tolerance` m). Returns the number of developments changed. One undo step.
int assignDevelopmentsToLevels(Project& project, double tolerance = 5.0);

} // namespace openmine::mine_design
