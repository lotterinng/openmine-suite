#pragma once
// SPDX-License-Identifier: Apache-2.0
// DXF (ASCII, AutoCAD R2004 / AC1018 dialect) reader and writer. Own implementation of the
// published DXF reference; no third-party DXF library is used at run time (ezdxf is used only
// in tests to validate the files we write).
//
// Reader: LINE, POINT, CIRCLE, ARC (with OCS), ELLIPSE (tessellated), LWPOLYLINE, POLYLINE
// (2D/3D, curve-fit vertices, polygon mesh, polyface mesh), SPLINE (exact via fit points when the
// curve is a centripetal Catmull–Rom through them, otherwise evaluated to a polyline), TEXT, MTEXT
// (formatting codes stripped), INSERT/MINSERT (expanded recursively, with BYBLOCK colours and
// layer-0 inheritance), 3DFACE, SOLID/TRACE, HATCH (polyline and edge boundaries, pattern spacing
// from the pattern definition lines), DIMENSION (linear/aligned/angular/radial/diametric mapped to
// annotation objects; other dimensions expanded from their anonymous block). Layers with ACI or
// true colour, on/off/frozen/locked, linetype and transparency. $INSUNITS scaling to metres.
// Unsupported entities are counted and reported, never silently dropped.
//
// Writer: every OpenMine geometry type, layers (true colour + nearest ACI, linetypes,
// transparency), XDATA (application "OPENMINE") carrying object names and annotation settings so
// that a write → read round trip restores them.

#include "openmine/objects/CadEntities.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

namespace openmine::formats {

class DxfError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct DxfImportOptions {
    /// Scale drawing units to metres using $INSUNITS (unitless files are taken as metres).
    bool scaleToMetres = true;
    /// Overrides $INSUNITS: drawing units per metre is 1/unitScale. 0 = use the header.
    double unitScaleOverride = 0.0;
    /// Chord tolerance (metres) for curves that must be tessellated (ellipses, NURBS, OCS bulges).
    double chordTolerance = 0.001;
    /// Expand INSERTs (block references). If false, inserts are reported as skipped.
    bool expandInserts = true;
    /// Import entities on frozen / off layers.
    bool includeHiddenLayers = true;
    /// Prefix added to imported layer names (e.g. "DXF/").
    std::string layerPrefix;
};

struct DxfImportResult {
    std::string acadVersion;           ///< $ACADVER
    int insUnits = 0;                  ///< $INSUNITS
    double unitScale = 1.0;            ///< factor applied to coordinates (to metres)
    std::vector<CadLayer> layers;      ///< layers used by the imported objects (ids assigned)
    std::vector<GeometryObject> objects; ///< objects reference `layers` by id
    std::map<std::string, int> imported; ///< DXF entity type → count imported
    std::map<std::string, int> skipped;  ///< DXF entity type → count not imported
    std::vector<std::string> warnings;
};

DxfImportResult readDxf(const std::filesystem::path& file, const DxfImportOptions& options = {});
DxfImportResult readDxfString(std::string_view content, const DxfImportOptions& options = {});

struct DxfExportOptions {
    /// Chord tolerance used when an object cannot be represented exactly (3D polylines with bulges).
    double chordTolerance = 0.001;
    bool exportMeshes = true; ///< meshes/solids as polyface meshes
    std::string producer = "OpenMine Suite";
};

struct DxfExportReport {
    std::map<std::string, int> written;  ///< DXF entity type → count
    std::vector<std::string> warnings;   ///< approximations made
    int skipped = 0;
};

/// Writes `objects` (with the layers they reference) as a DXF file. Layer ids not found in
/// `layers` are written on layer "0".
DxfExportReport writeDxf(const std::filesystem::path& file, const std::vector<CadLayer>& layers,
                         const std::vector<const GeometryObject*>& objects, const DxfExportOptions& options = {});
std::string writeDxfString(const std::vector<CadLayer>& layers, const std::vector<const GeometryObject*>& objects,
                           const DxfExportOptions& options = {}, DxfExportReport* report = nullptr);

/// The AutoCAD Color Index palette (index 0–255 → 0xRRGGBB; 0 and 256 are BYBLOCK/BYLAYER).
std::uint32_t aciToRgb(int aci);
/// Nearest ACI (1–255) to an RGB colour.
int rgbToAci(std::uint32_t rgb);

/// Metres per drawing unit for a $INSUNITS code (0 when the code is unitless/unknown).
double metresPerInsUnit(int insUnits);

} // namespace openmine::formats
