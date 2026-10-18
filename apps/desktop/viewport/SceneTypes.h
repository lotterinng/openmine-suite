#pragma once
// SPDX-License-Identifier: Apache-2.0
// Display representation of project entities, shared by every viewport.
//
// A SceneItem is one drawable part of an entity (a centreline, an excavation envelope, a label,
// the visible blocks of a block model…) in *render-local* coordinates: world coordinates minus
// the scene's render origin (see coordinates::RenderOrigin), so large mine-grid coordinates keep
// sub-millimetre precision in single-precision GPU buffers. Items are rebuilt only when their
// entity (or something they depend on, such as a layer or profile) changes.

#include "../app/AppContext.h"

#include "openmine/coordinates/RenderOrigin.h"

#include <vtkSmartPointer.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

class vtkPolyData;

namespace openmine::app {

enum class Lut { Viridis, Rainbow, Diverging, Categorical, Grey, Classification };

struct ScalarDisplay {
    std::string name;        ///< array name in the poly data (point or cell data)
    bool cellData = false;
    double min = 0, max = 1;
    Lut lut = Lut::Viridis;
    std::string title;       ///< legend title (with unit)
    std::vector<std::pair<double, std::string>> categories; ///< categorical legends
};

struct SceneItem {
    enum class Prim { Lines, Surface, Points, Label };

    SelectionItem owner;          ///< what a pick selects
    Uuid geometryId;              ///< geometry object drawn (for snapping), nil when none
    std::string part;             ///< "curve", "envelope", "mesh", "label", "blocks", "outline", ...
    Prim prim = Prim::Lines;
    vtkSmartPointer<vtkPolyData> data; ///< render-local coordinates (empty for labels)
    std::uint32_t color = 0xD0D0D0;
    double opacity = 1.0;
    double lineWidth = 2.0;       ///< pixels
    double pointSize = 6.0;       ///< pixels
    bool spheres = false;         ///< points as shaded spheres
    bool tubes = false;           ///< lines as shaded tubes
    bool lighting = true;
    bool backfaceCulling = false;
    bool pickable = true;
    bool ghost = false;           ///< context display (4D future state, hidden-by-filter outline…)
    std::optional<ScalarDisplay> scalars;
    std::string labelText;        ///< Label: text and anchor (render-local)
    Vec3 labelAnchor;
    int labelSize = 12;           ///< points
    Uuid layerId;
    BoundingBox bounds = BoundingBox::empty(); ///< world coordinates
    std::uint64_t revision = 0;   ///< changes whenever the item is rebuilt
};

/// Per-block-model display choices.
struct BlockModelDisplay {
    std::string attribute;            ///< empty = first attribute
    std::optional<double> filterMin, filterMax;
    double opacity = 1.0;
    bool outline = true;
    Lut lut = Lut::Viridis;
    friend bool operator==(const BlockModelDisplay&, const BlockModelDisplay&) = default;
};

struct SceneSettings {
    double chordTolerance = 0.01;     ///< m, curve tessellation for display
    double linetypeScale = 1.0;       ///< m per linetype pattern unit
    bool developmentEnvelopes = true;
    double envelopeOpacity = 0.35;
    bool colorDevelopmentsByStatus = true;
    bool developmentStations = false;
    double stationInterval = 20.0;
    bool controlLabels = true;
    bool drillholeLabels = true;
    std::string drillholeField;       ///< numeric interval field; empty = colour by hole type
    double drillholeWidth = 3.0;
    double structureDiscRadius = 2.0; ///< m when the measurement has no persistence
    bool meshEdges = false;           ///< draw mesh wireframe over shaded surfaces
    std::map<Uuid, BlockModelDisplay> blockModels;
    friend bool operator==(const SceneSettings&, const SceneSettings&) = default;
};

/// Colour helpers (0xRRGGBB).
std::uint32_t lutColor(Lut lut, double t);           ///< t in [0,1]
std::uint32_t categoricalColor(std::size_t index);
std::uint32_t categoricalColor(const std::string& key);
std::uint32_t classificationColor(int asprsClass);   ///< ASPRS LAS classes
std::uint32_t statusColor(const std::string& status, std::uint32_t planned);

} // namespace openmine::app
