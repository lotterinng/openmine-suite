// SPDX-License-Identifier: Apache-2.0
// MainWindow: file import/export by type and the entity actions requested from the project
// explorer, the viewport context menu and the menus. Each action calls the module operation that
// does the work (validated, undoable, or a background job on a snapshot) — nothing is simulated.
#include "MainWindow.h"

#include "app/EntityInfo.h"
#include "dialogs/DataDialogs.h"
#include "dialogs/DesignDialogs.h"
#include "dialogs/DialogUtil.h"
#include "dialogs/GeologyDialogs.h"
#include "dialogs/ModelDialogs.h"
#include "dialogs/SurveyDialogs.h"
#include "panels/LayerManager.h"
#include "panels/ReportsPanel.h"
#include "panels/StereonetPanel.h"
#include "scheduler/LibraryDialogs.h"
#include "scheduler/SchedulerPanel.h"
#include "theme/Theme.h"
#include "viewport/ViewportArea.h"

#include "openmine/cad/Edit.h"
#include "openmine/formats/DxfProject.h"
#include "openmine/formats/MeshFormats.h"
#include "openmine/geology/BlockModel.h"
#include "openmine/geology/Drillholes.h"
#include "openmine/geometry/MeshOps.h"
#include "openmine/geometry/Polyline.h"
#include "openmine/mine_design/Development.h"
#include "openmine/mine_design/GeometryEdits.h"
#include "openmine/mine_design/Revisions.h"
#include "openmine/omf/OmfProject.h"
#include "openmine/project/ProjectEditor.h"
#include "openmine/survey/PointCloudProject.h"
#include "openmine/survey/Registration.h"
#include "openmine/reporting/ReportDocument.h"

#ifdef OPENMINE_APP_HAVE_OCCT
#include "openmine/occt/Modelling.h"
#include "openmine/occt/SolidObjects.h"
#endif
#ifdef OPENMINE_APP_HAVE_GIS
#include "openmine/gis/Gis.h"
#endif

#include <QApplication>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QRegularExpression>

#include <fstream>

namespace openmine::app {

namespace {

std::filesystem::path fsPath(const QString& s) { return std::filesystem::path(s.toStdU16String()); }

void writeText(const std::filesystem::path& f, const std::string& text) {
    std::ofstream o(f, std::ios::binary);
    if (!o) throw std::runtime_error("Cannot write " + f.string());
    o << text;
    if (!o) throw std::runtime_error("Write failed: " + f.string());
}

std::string readText(const std::filesystem::path& f) {
    std::ifstream i(f, std::ios::binary);
    if (!i) throw std::runtime_error("Cannot read " + f.string());
    return std::string(std::istreambuf_iterator<char>(i), {});
}

/// Concatenates meshes (for exporting several selected surfaces to one file).
MeshData mergeMeshes(const std::vector<const MeshData*>& meshes) {
    MeshData out;
    for (const auto* m : meshes) {
        const auto base = static_cast<std::uint32_t>(out.vertices.size());
        out.vertices.insert(out.vertices.end(), m->vertices.begin(), m->vertices.end());
        for (const auto& t : m->triangles) out.triangles.push_back({t[0] + base, t[1] + base, t[2] + base});
    }
    return out;
}

/// What a delimited text file most likely holds, from its header row.
enum class CsvContent { PointCloud, ControlPoints, Structures, Drillholes, BlockModel, Unknown };

CsvContent sniffCsv(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return CsvContent::Unknown;
    const QString header = QString::fromUtf8(f.readLine()).toLower();
    int rows = 0;
    while (!f.atEnd() && rows < 5001) {
        f.readLine();
        ++rows;
    }
    QStringList cols = header.split(QRegularExpression("[,;\\t ]+"), Qt::SkipEmptyParts);
    for (auto& c : cols) c = c.trimmed().remove('"');
    auto has = [&](std::initializer_list<const char*> names) {
        for (const char* n : names)
            if (cols.contains(QString::fromLatin1(n))) return true;
        return false;
    };
    bool numericHeader = !cols.isEmpty();
    for (const auto& c : cols) {
        bool ok = false;
        c.toDouble(&ok);
        numericHeader = numericHeader && ok;
    }
    if (numericHeader) return CsvContent::PointCloud;
    if (has({"dipdirection", "dip_direction", "dipdir", "dip direction", "ddr"}) && has({"dip"})) return CsvContent::Structures;
    if (has({"holeid", "hole_id", "bhid", "hole"})) return CsvContent::Drillholes;
    if (has({"xc", "xcentre", "xcenter", "ijk", "i"}) || (has({"x"}) && has({"dx", "xinc", "xsize"}))) return CsvContent::BlockModel;
    if (has({"name", "id", "point", "station", "pointid", "point_id"}) && rows <= 5000) return CsvContent::ControlPoints;
    if (has({"x", "easting", "e"})) return CsvContent::PointCloud;
    return CsvContent::Unknown;
}

void showSectionAlong(ViewportArea* area, const Uuid& developmentId, double chainage) {
    for (auto* v : area->views())
        if (v != area->active() && v->mode() == ViewMode::Section) {
            v->sectionAtChainage(developmentId, chainage);
            return;
        }
    if (area->layoutKind() == ViewportArea::Layout::Single) area->setLayoutKind(ViewportArea::Layout::TwoColumns);
    for (auto* v : area->views())
        if (v != area->active()) {
            v->sectionAtChainage(developmentId, chainage);
            return;
        }
    area->active()->sectionAtChainage(developmentId, chainage);
}

/// ESRI ASCII grids and ASCII point files share ".asc": a grid starts with its "ncols" header.
bool isAsciiGrid(const QString& file) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    return QString::fromLatin1(f.readLine(256)).trimmed().startsWith(QLatin1String("ncols"), Qt::CaseInsensitive);
}

} // namespace

// ---- import ------------------------------------------------------------------------------------

bool MainWindow::importFile(const QString& path) {
    if (!ctx_.hasProject()) ctx_.newProject(QFileInfo(path).completeBaseName(), Date::todayUtc());
    const QString ext = "." + QFileInfo(path).suffix().toLower();
    const auto file = fsPath(path);
    const QString base = QFileInfo(path).fileName();
    const auto pointExts = survey::readableExtensions();
    auto isPointFormat = [&](const QString& e) { return std::find(pointExts.begin(), pointExts.end(), sstr(e)) != pointExts.end(); };

    auto importClouds = [this](const QStringList& files) {
        auto* d = new PointCloudImportDialog(ctx_, files, this);
        d->setAttribute(Qt::WA_DeleteOnClose);
        if (nonInteractive_) {
            d->start();
            d->close();
        } else {
            d->open();
        }
        return true;
    };

    try {
        if (ext == ".openmine") return maybeSave() && openProjectFrom(path);
        if (ext == ".dxf") {
            ctx_.jobs().start(tr("Import %1").arg(base), tr("Import"), [this, file, base](jobs::Progress& pr) -> UiApply {
                pr.update(0.1, "Reading DXF");
                auto dxf = std::make_shared<formats::DxfImportResult>(formats::readDxf(file));
                return [this, dxf, base] {
                    ctx_.tryEdit(tr("Import DXF"), [&] {
                        const auto s = formats::importDxfIntoProject(ctx_.project(), *dxf, true, "Import " + sstr(base));
                        QStringList skipped;
                        for (const auto& [t, n] : dxf->skipped) skipped << QStringLiteral("%1 %2").arg(n).arg(qstr(t));
                        ctx_.report(tr("%1: %2 objects on %3 new and %4 existing layers (DXF %5, units ×%6)%7")
                                        .arg(base)
                                        .arg(s.objects.size())
                                        .arg(s.createdLayers.size())
                                        .arg(s.reusedLayers.size())
                                        .arg(qstr(dxf->acadVersion))
                                        .arg(dxf->unitScale)
                                        .arg(skipped.isEmpty() ? QString() : tr("; not imported: %1").arg(skipped.join(", "))));
                        for (const auto& w : dxf->warnings) ctx_.report(qstr(w), MessageLevel::Warning);
                        std::vector<SelectionItem> sel;
                        for (const auto& id : s.objects) sel.push_back({EntityKind::Geometry, id});
                        ctx_.selectMany(sel);
                        if (auto* v = activeView()) v->fitSelection();
                    });
                };
            });
            return true;
        }
        if (ext == ".omf") {
            ctx_.jobs().start(tr("Import %1").arg(base), tr("Import"), [this, file, base](jobs::Progress& pr) -> UiApply {
                pr.update(0.1, "Reading OMF");
                auto doc = std::make_shared<omf::Document>(omf::readOmf(file));
                return [this, doc, base, file] {
                    ctx_.tryEdit(tr("Import OMF"), [&] {
                        omf::OmfImportOptions o;
                        o.layerName = sstr(base);
                        o.sourceFile = file.string();
                        const auto s = omf::importOmf(ctx_.project(), *doc, o);
                        ctx_.report(tr("%1: %2 geometry objects, %3 block models, %4 point clouds")
                                        .arg(base)
                                        .arg(s.geometry.size())
                                        .arg(s.blockModels.size())
                                        .arg(s.pointClouds.size()));
                        for (const auto& n : s.notes) ctx_.report(qstr(n), MessageLevel::Warning);
                    });
                };
            });
            return true;
        }
        if (ext == ".obj" || ext == ".stl" || (ext == ".ply" && !isPointFormat(ext))) {
            ctx_.jobs().start(tr("Import %1").arg(base), tr("Import"), [this, file, base](jobs::Progress&) -> UiApply {
                auto mf = std::make_shared<formats::MeshFile>(formats::readMesh(file));
                return [this, mf, base] {
                    ctx_.tryEdit(tr("Import mesh"), [&] {
                        CommandStack::MacroScope macro(ctx_.project().commands(), "Import " + sstr(base));
                        const Uuid id = formats::addMeshObject(ctx_.project(), *mf, sstr(QFileInfo(base).completeBaseName()), workflowLayer(ctx_, "Surfaces", 0xC8A165),
                                                               sstr(base));
                        macro.commit();
                        const auto topo = geometry::topology(mf->mesh);
                        ctx_.report(tr("%1: %2 triangles, area %3 m²%4").arg(base).arg(mf->mesh.triangles.size()).arg(formatNumber(geometry::meshArea(mf->mesh), 1))
                                        .arg(topo.closed() ? tr(", closed, volume %1 m³").arg(formatNumber(std::abs(geometry::meshSignedVolume(mf->mesh)), 1)) : QString()));
                        for (const auto& w : mf->warnings) ctx_.report(qstr(w), MessageLevel::Warning);
                        ctx_.select(EntityKind::Geometry, id);
                        if (auto* v = activeView()) v->fitSelection();
                    });
                };
            });
            return true;
        }
        if (ext == ".step" || ext == ".stp" || ext == ".iges" || ext == ".igs") {
#ifdef OPENMINE_APP_HAVE_OCCT
            QApplication::setOverrideCursor(Qt::WaitCursor);
            ctx_.tryEdit(tr("Import solids"), [&] {
                CommandStack::MacroScope macro(ctx_.project().commands(), "Import " + sstr(base));
                const Uuid layer = workflowLayer(ctx_, "Solids", 0x7FB3D5);
                const auto ids = occt::importSolids(ctx_.project(), file, layer);
                macro.commit();
                ctx_.report(tr("%1: %2 solid(s)").arg(base).arg(ids.size()));
                std::vector<SelectionItem> sel;
                for (const auto& id : ids) sel.push_back({EntityKind::Geometry, id});
                ctx_.selectMany(sel);
            });
            QApplication::restoreOverrideCursor();
            return true;
#else
            ctx_.report(tr("STEP/IGES import is not available: built without Open CASCADE"), MessageLevel::Error);
            return false;
#endif
        }
        if (ext == ".shp" || ext == ".gpkg" || ext == ".geojson" || ext == ".json" || ext == ".kml") {
#ifdef OPENMINE_APP_HAVE_GIS
            double z = activeView() ? activeView()->workPlaneZ() : 0.0;
            QString layerName;
            if (!nonInteractive_) {
                const auto layers = gis::vectorInfo(file);
                if (layers.size() > 1) {
                    QStringList names;
                    for (const auto& l : layers) names << QStringLiteral("%1 (%2, %3 features)").arg(qstr(l.name), qstr(l.geometryType)).arg(l.featureCount);
                    bool ok = false;
                    const QString pick = QInputDialog::getItem(this, tr("Import GIS"), tr("Layer:"), names, 0, false, &ok);
                    if (!ok) return false;
                    layerName = qstr(layers[static_cast<std::size_t>(names.indexOf(pick))].name);
                }
                bool ok = false;
                z = QInputDialog::getDouble(this, tr("Import GIS"), tr("Elevation for 2D features (m):"), z, -1e5, 1e5, 3, &ok);
                if (!ok) return false;
            }
            ctx_.jobs().start(tr("Import %1").arg(base), tr("Import"), [this, file, base, layerName, z](jobs::Progress&) -> UiApply {
                auto features = std::make_shared<gis::VectorImport>(gis::importVector(file, sstr(layerName), z));
                return [this, features, base] {
                    ctx_.tryEdit(tr("Import GIS"), [&] {
                        CommandStack::MacroScope macro(ctx_.project().commands(), "Import " + sstr(base));
                        const Uuid layer = workflowLayer(ctx_, sstr(QFileInfo(base).completeBaseName()), 0xE0C060);
                        const auto ids = gis::addVectorFeatures(ctx_.project(), *features, layer);
                        macro.commit();
                        ctx_.report(tr("%1: %2 features (%3 skipped without geometry)%4")
                                        .arg(base)
                                        .arg(ids.size())
                                        .arg(features->skipped)
                                        .arg(features->crsAuthority.empty() ? QString() : tr(", declared CRS %1").arg(qstr(features->crsAuthority))));
                        const auto* pc = ctx_.project().model().projectCrs();
                        if (!features->crsAuthority.empty()) {
                            if (!pc || pc->authorityCode.empty())
                                ctx_.report(tr("The file declares %1 but the project CRS is not declared; coordinates were imported as they are.")
                                                .arg(qstr(features->crsAuthority)),
                                            MessageLevel::Warning);
                            else if (pc->authorityCode != features->crsAuthority)
                                ctx_.report(tr("The file's CRS (%1) differs from the project CRS (%2). Coordinates were not transformed: use Tools ▸ Transform.")
                                                .arg(qstr(features->crsAuthority), qstr(pc->authorityCode)),
                                            MessageLevel::Warning);
                        }
                        for (const auto& w : features->warnings) ctx_.report(qstr(w), MessageLevel::Warning);
                    });
                };
            });
            return true;
#else
            ctx_.report(tr("GIS import is not available: built without GDAL"), MessageLevel::Error);
            return false;
#endif
        }
        if (ext == ".tif" || ext == ".tiff" || (ext == ".asc" && isAsciiGrid(path))) {
#ifdef OPENMINE_APP_HAVE_GIS
            int step = 1;
            if (!nonInteractive_) {
                const auto info = gis::rasterInfo(file);
                bool ok = false;
                step = QInputDialog::getInt(this, tr("Import DEM"),
                                            tr("%1 × %2 cells. Take every n-th cell:").arg(info.width).arg(info.height), 1, 1, 100, 1, &ok);
                if (!ok) return false;
            }
            ctx_.jobs().start(tr("Import %1").arg(base), tr("Import"), [this, file, base, step](jobs::Progress&) -> UiApply {
                gis::DemImportOptions o;
                o.step = step;
                auto info = std::make_shared<gis::RasterInfo>();
                auto mesh = std::make_shared<MeshData>(gis::importDem(file, o, info.get()));
                return [this, mesh, info, base, step] {
                    ctx_.tryEdit(tr("Import DEM"), [&] {
                        CommandStack::MacroScope macro(ctx_.project().commands(), "Import " + sstr(base));
                        GeometryObject g;
                        g.type = GeometryType::Mesh;
                        g.name = sstr(QFileInfo(base).completeBaseName());
                        g.layerId = workflowLayer(ctx_, "Surfaces", 0xC8A165);
                        g.mesh = mesh;
                        ensureAttribute(ctx_.project(), "GeometryObject", "source_file", "Source file");
                        ensureAttribute(ctx_.project(), "GeometryObject", "raster_step", "Raster step", AttributeValueType::Integer);
                        g.attributes["source_file"] = sstr(base);
                        g.attributes["raster_step"] = std::to_string(step);
                        const Uuid id = ProjectEditor(ctx_.project()).addGeometry(std::move(g));
                        macro.commit();
                        ctx_.report(tr("%1: %2 × %3 raster → %4 triangles").arg(base).arg(info->width).arg(info->height).arg(mesh->triangles.size()));
                        ctx_.select(EntityKind::Geometry, id);
                    });
                };
            });
            return true;
#else
            ctx_.report(tr("DEM import is not available: built without GDAL"), MessageLevel::Error);
            return false;
#endif
        }
        if (ext == ".csv" || ext == ".txt") {
            CsvContent c = sniffCsv(path);
            if (!nonInteractive_) {
                const QStringList kinds{tr("Point cloud (x, y, z …)"), tr("Survey control points"), tr("Structural measurements"),
                                        tr("Drillhole table"), tr("Block model (centroids)")};
                const int guess = c == CsvContent::Unknown ? 0 : static_cast<int>(c);
                bool ok = false;
                const QString pick = QInputDialog::getItem(this, tr("Import %1").arg(base), tr("The file contains:"), kinds, guess, false, &ok);
                if (!ok) return false;
                c = static_cast<CsvContent>(kinds.indexOf(pick));
            }
            switch (c) {
            case CsvContent::ControlPoints: {
                auto* d = new ControlImportDialog(ctx_, path, this);
                d->setAttribute(Qt::WA_DeleteOnClose);
                if (nonInteractive_) {
                    const bool ok = d->apply();
                    d->close();
                    return ok;
                }
                d->open();
                return true;
            }
            case CsvContent::Structures: {
                auto* d = new StructureImportDialog(ctx_, path, this);
                d->setAttribute(Qt::WA_DeleteOnClose);
                if (nonInteractive_) {
                    const bool ok = d->apply();
                    d->close();
                    return ok;
                }
                d->open();
                return true;
            }
            case CsvContent::Drillholes: {
                auto* d = new DrillholeImportDialog(ctx_, this);
                d->setAttribute(Qt::WA_DeleteOnClose);
                // The collar table was given: the survey and interval tables usually sit next to it.
                const QFileInfo fi(path);
                QString surveys, collars = path;
                QStringList intervals;
                for (const auto& sib : fi.dir().entryInfoList({"*.csv", "*.txt"}, QDir::Files)) {
                    const QString n = sib.fileName().toLower();
                    if (sib.absoluteFilePath() == fi.absoluteFilePath()) continue;
                    if (n.contains("survey")) surveys = sib.absoluteFilePath();
                    else if (n.contains("assay") || n.contains("lith") || n.contains("interval") || n.contains("geol")) intervals << sib.absoluteFilePath();
                    else if (n.contains("collar") && !fi.fileName().toLower().contains("collar")) collars = sib.absoluteFilePath();
                }
                d->setFiles(collars, surveys, intervals);
                if (nonInteractive_) {
                    const bool ok = d->validate() == 0 && d->importNow();
                    d->close();
                    return ok;
                }
                d->open();
                return true;
            }
            case CsvContent::BlockModel: {
                double size = 0;
                if (!nonInteractive_) {
                    bool ok = false;
                    size = QInputDialog::getDouble(this, tr("Import block model"), tr("Block size (m, 0 = infer from the centroid spacing):"), 0, 0, 10000, 3, &ok);
                    if (!ok) return false;
                }
                ctx_.jobs().start(tr("Import %1").arg(base), tr("Import"), [this, file, base, size](jobs::Progress&) -> UiApply {
                    const std::string text = readText(file);
                    auto grid = geology::inferGridFromCsv(text, Vec3{size, size, size});
                    grid.name = sstr(QFileInfo(base).completeBaseName());
                    auto imp = std::make_shared<geology::BlockCsvImport>(geology::importBlockModelCsv(text, grid));
                    return [this, imp, base] {
                        ctx_.tryEdit(tr("Import block model"), [&] {
                            CommandStack::MacroScope macro(ctx_.project().commands(), "Import " + sstr(base));
                            const Uuid id = geology::createBlockModel(ctx_.project(), imp->data.header);
                            auto values = imp->data;
                            values.header = *ctx_.project().model().blockModels.find(id);
                            geology::saveBlockModel(ctx_.project(), values, "Import block values");
                            macro.commit();
                            ctx_.report(tr("%1: %2 of %3 rows placed in a %4 × %5 × %6 grid")
                                            .arg(base)
                                            .arg(imp->placed)
                                            .arg(imp->rows)
                                            .arg(values.header.nx)
                                            .arg(values.header.ny)
                                            .arg(values.header.nz));
                            for (std::size_t i = 0; i < std::min<std::size_t>(imp->issues.size(), 20); ++i) ctx_.report(qstr(imp->issues[i]), MessageLevel::Warning);
                            if (imp->issues.size() > 20) ctx_.report(tr("… %1 more issues").arg(imp->issues.size() - 20), MessageLevel::Warning);
                            ctx_.select(EntityKind::BlockModel, id);
                        });
                    };
                });
                return true;
            }
            case CsvContent::PointCloud: return importClouds({path});
            case CsvContent::Unknown: break;
            }
            ctx_.report(tr("%1: cannot tell what the columns hold — import it from the Survey or Geology menu").arg(base), MessageLevel::Error);
            return false;
        }
        if (isPointFormat(ext) || ext == ".xyz" || ext == ".pts" || ext == ".ptx") return importClouds({path});

        // Formats added by plugins.
        if (const auto reg = ctx_.plugins().importerFor(file)) {
            const auto r = ctx_.plugins().runImport(ctx_.project(), file);
            for (const auto& l : r.log) ctx_.report(qstr(l));
            if (!r.ok) {
                ctx_.report(tr("%1 (plugin %2) failed: %3").arg(base, qstr(reg->pluginId), qstr(r.error)), MessageLevel::Error);
                return false;
            }
            ctx_.report(tr("%1 imported by plugin %2: %3").arg(base, qstr(reg->pluginId), qstr(r.text)));
            return true;
        }
    } catch (const std::exception& e) {
        ctx_.report(tr("Import of %1 failed: %2").arg(base, qstr(e.what())), MessageLevel::Error);
        return false;
    }
    ctx_.report(tr("%1: no importer for '%2' files").arg(base, ext), MessageLevel::Error);
    return false;
}

// ---- export ------------------------------------------------------------------------------------

bool MainWindow::exportFile(const QString& path) {
    if (!ctx_.hasProject()) return false;
    const QString ext = "." + QFileInfo(path).suffix().toLower();
    const auto file = fsPath(path);
    const QString base = QFileInfo(path).fileName();
    const auto& m = ctx_.project().model();
    const auto selGeom = ctx_.selectedGeometry();
    const auto clouds = ctx_.selected(EntityKind::PointCloud);
    const auto blocks = ctx_.selected(EntityKind::BlockModel);
    const auto holes = ctx_.selected(EntityKind::Drillhole);
    const auto writable = survey::writableExtensions();
    const bool pointExt = std::find(writable.begin(), writable.end(), sstr(ext)) != writable.end();
    auto crsText = [&]() -> std::string {
        const auto* c = m.projectCrs();
        if (!c) return {};
        return c->authorityCode.empty() ? c->definition : c->authorityCode;
    };
    auto meshesOf = [&](const std::vector<Uuid>& ids) {
        std::vector<const MeshData*> out;
        for (const auto& id : ids)
            if (const auto* g = m.geometry.find(id); g && g->mesh && (g->type == GeometryType::Mesh || g->type == GeometryType::Solid))
                out.push_back(g->mesh.get());
        return out;
    };
    auto done = [this, base](const QString& what) { ctx_.report(tr("Exported %1 to %2").arg(what, base)); };

    try {
        // Point clouds (selected) go first: .ply/.csv/.txt are also mesh / table formats.
        if (!clouds.empty() && pointExt) {
            if (clouds.size() > 1) ctx_.report(tr("Several point clouds are selected: exporting the last one selected"), MessageLevel::Warning);
            const Uuid id = clouds.back();
            const auto snap = ctx_.snapshot();
            ctx_.jobs().start(tr("Export %1").arg(base), tr("Export"), [snap, id, file](jobs::Progress& pr) -> UiApply {
                survey::exportDataset(*snap, id, file, pr);
                return {};
            });
            done(tr("the point cloud (project coordinates)"));
            return true;
        }
        if (ext == ".dxf") {
            const auto r = formats::exportProjectDxf(ctx_.project(), file, selGeom);
            int n = 0;
            for (const auto& [t, c] : r.written) n += c;
            done(tr("%1 DXF entities%2").arg(n).arg(selGeom.empty() ? tr(" (all geometry)") : QString()));
            for (const auto& w : r.warnings) ctx_.report(qstr(w), MessageLevel::Warning);
            return true;
        }
        if (ext == ".omf") {
            omf::OmfExportOptions o;
            o.allGeometry = selGeom.empty() && blocks.empty() && clouds.empty();
            o.geometry = selGeom;
            o.allBlockModels = o.allGeometry;
            o.blockModels = blocks;
            o.pointClouds = clouds;
            o.drillholes = o.allGeometry || !holes.empty();
            o.author = ctx_.project().userName();
            const auto snap = ctx_.snapshot();
            ctx_.jobs().start(tr("Export %1").arg(base), tr("Export"), [this, snap, o, file, base](jobs::Progress& pr) -> UiApply {
                auto notes = std::make_shared<std::vector<std::string>>();
                const auto doc = omf::exportProjectOmf(*snap, o, notes.get(), pr);
                omf::writeOmf(doc, file);
                return [this, notes, base] {
                    ctx_.report(tr("Exported OMF %1").arg(base));
                    for (const auto& n : *notes) ctx_.report(qstr(n), MessageLevel::Warning);
                };
            });
            return true;
        }
        if (ext == ".obj" || ext == ".stl" || ext == ".ply") {
            auto meshes = meshesOf(selGeom);
            if (meshes.empty()) {
                ctx_.report(tr("Select the surfaces or solids to export"), MessageLevel::Warning);
                return false;
            }
            const MeshData merged = meshes.size() == 1 ? *meshes.front() : mergeMeshes(meshes);
            const auto* g0 = m.geometry.find(selGeom.front());
            for (const auto& w : formats::writeMesh(file, merged, g0 ? g0->name : std::string())) ctx_.report(qstr(w), MessageLevel::Warning);
            done(tr("%1 surface(s), %2 triangles").arg(meshes.size()).arg(merged.triangles.size()));
            return true;
        }
        if (ext == ".step" || ext == ".stp" || ext == ".iges" || ext == ".igs") {
#ifdef OPENMINE_APP_HAVE_OCCT
            std::vector<Uuid> solids;
            for (const auto& id : selGeom)
                if (const auto* g = m.geometry.find(id); g && (g->type == GeometryType::Solid || g->type == GeometryType::Mesh)) solids.push_back(id);
            if (solids.empty()) {
                ctx_.report(tr("Select the solids to export"), MessageLevel::Warning);
                return false;
            }
            occt::exportSolids(ctx_.project(), solids, file);
            done(tr("%1 solid(s)").arg(solids.size()));
            return true;
#else
            ctx_.report(tr("STEP/IGES export is not available: built without Open CASCADE"), MessageLevel::Error);
            return false;
#endif
        }
        if (ext == ".shp" || ext == ".gpkg" || ext == ".geojson" || ext == ".kml") {
#ifdef OPENMINE_APP_HAVE_GIS
            std::vector<const GeometryObject*> objs;
            auto consider = [&](const GeometryObject& g) {
                if (g.type != GeometryType::Mesh && g.type != GeometryType::Solid && g.type != GeometryType::Text && g.type != GeometryType::Dimension &&
                    g.type != GeometryType::Hatch)
                    objs.push_back(&g);
            };
            if (selGeom.empty())
                for (const auto& g : m.geometry.all()) consider(g);
            else
                for (const auto& id : selGeom)
                    if (const auto* g = m.geometry.find(id)) consider(*g);
            if (objs.empty()) {
                ctx_.report(tr("No points or curves to export"), MessageLevel::Warning);
                return false;
            }
            gis::VectorExportOptions o;
            o.layerName = sstr(QFileInfo(base).completeBaseName());
            o.crsWkt = crsText();
            gis::exportVector(objs, file, o);
            done(tr("%1 feature(s)").arg(objs.size()));
            return true;
#else
            ctx_.report(tr("GIS export is not available: built without GDAL"), MessageLevel::Error);
            return false;
#endif
        }
        if (ext == ".tif" || ext == ".tiff" || ext == ".asc") {
#ifdef OPENMINE_APP_HAVE_GIS
            auto meshes = meshesOf(selGeom);
            if (meshes.empty()) {
                ctx_.report(tr("Select the surface to export as a DEM"), MessageLevel::Warning);
                return false;
            }
            double cell = 1.0;
            if (!nonInteractive_) {
                bool ok = false;
                cell = QInputDialog::getDouble(this, tr("Export DEM"), tr("Cell size (m):"), 1.0, 0.01, 1000, 3, &ok);
                if (!ok) return false;
            }
            gis::DemExportOptions o;
            o.cellSize = cell;
            o.crsWkt = crsText();
            o.driver = ext == ".asc" ? "AAIGrid" : "GTiff";
            const MeshData merged = meshes.size() == 1 ? *meshes.front() : mergeMeshes(meshes);
            const auto info = gis::exportDem(merged, file, o);
            done(tr("a %1 × %2 elevation raster").arg(info.width).arg(info.height));
            return true;
#else
            ctx_.report(tr("DEM export is not available: built without GDAL"), MessageLevel::Error);
            return false;
#endif
        }
        if (ext == ".csv" && !blocks.empty()) {
            const auto bmd = geology::loadBlockModel(ctx_.project(), blocks.back());
            writeText(file, geology::exportBlockModelCsv(bmd));
            done(tr("block model '%1'").arg(qstr(bmd.header.name)));
            return true;
        }
        if (ext == ".csv" && (!holes.empty() || ctx_.primary().kind == EntityKind::Drillhole)) {
            std::vector<Drillhole> list;
            for (const auto& id : holes)
                if (const auto* h = m.drillholes.find(id)) list.push_back(*h);
            const auto csv = geology::exportDrillholesCsv(list);
            const QFileInfo fi(path);
            const QString stem = fi.dir().filePath(fi.completeBaseName());
            writeText(fsPath(stem + "_collars.csv"), csv.collars);
            writeText(fsPath(stem + "_surveys.csv"), csv.surveys);
            writeText(fsPath(stem + "_intervals.csv"), csv.intervals);
            ctx_.report(tr("Exported %1 drillhole(s) to %2_collars.csv, _surveys.csv and _intervals.csv").arg(list.size()).arg(fi.completeBaseName()));
            return true;
        }
        if (ext == ".csv" && ctx_.primary().kind == EntityKind::Conformance) {
            handleEntityAction("export-conformance-to:" + path, ctx_.selection());
            return true;
        }
        if (ext == ".csv" || ext == ".xlsx" || ext == ".pdf") return scheduler_->exportFile(path);
        if (pointExt) {
            ctx_.report(tr("Select the point cloud to export"), MessageLevel::Warning);
            return false;
        }
        if (const auto reg = ctx_.plugins().exporterFor(file)) {
            const auto r = ctx_.plugins().runExport(ctx_.project(), file);
            for (const auto& l : r.log) ctx_.report(qstr(l));
            if (!r.ok) {
                ctx_.report(tr("%1 (plugin %2) failed: %3").arg(base, qstr(reg->pluginId), qstr(r.error)), MessageLevel::Error);
                return false;
            }
            done(tr("by plugin %1").arg(qstr(reg->pluginId)));
            return true;
        }
    } catch (const std::exception& e) {
        ctx_.report(tr("Export to %1 failed: %2").arg(base, qstr(e.what())), MessageLevel::Error);
        return false;
    }
    ctx_.report(tr("%1: no exporter for '%2' files").arg(base, ext), MessageLevel::Error);
    return false;
}

// ---- entity actions ----------------------------------------------------------------------------

void MainWindow::handleEntityAction(const QString& actionIn, const std::vector<SelectionItem>& itemsIn) {
    if (!ctx_.hasProject()) return;
    QString action = actionIn;
    QString argument;
    if (const int colon = action.indexOf(':'); colon > 0) {
        argument = action.mid(colon + 1);
        action = action.left(colon);
    }
    // Explorer actions act on the items given; make them the selection so dialogs and panels agree.
    if (!itemsIn.empty() && itemsIn != ctx_.selection()) ctx_.selectMany(itemsIn);
    const auto items = itemsIn.empty() ? ctx_.selection() : itemsIn;
    auto& p = ctx_.project();
    const auto& m = p.model();
    auto first = [&](EntityKind k) -> Uuid {
        for (auto it = items.rbegin(); it != items.rend(); ++it)
            if (it->kind == k) return it->id;
        return primaryOf(k);
    };
    auto all = [&](EntityKind k) {
        std::vector<Uuid> out;
        for (const auto& s : items)
            if (s.kind == k) out.push_back(s.id);
        return out;
    };
    auto runDialog = [this](QDialog* d, bool modeless = false) {
        d->setAttribute(Qt::WA_DeleteOnClose);
        if (modeless || nonInteractive_) d->show();
        else d->open();
    };
    auto need = [&](EntityKind k, const QString& what) -> Uuid {
        const Uuid id = first(k);
        if (id.isNil()) ctx_.report(tr("%1: select a %2").arg(what, kindLabel(k).toLower()), MessageLevel::Warning);
        return id;
    };
    // A development selected through its centreline geometry counts as the development.
    auto development = [&]() -> Uuid {
        Uuid id = first(EntityKind::Development);
        if (id.isNil())
            for (const auto& g : ctx_.selectedGeometry())
                if (const auto* d = m.developmentForGeometry(g)) return d->id;
        return id;
    };
    auto cloudOp = [&](CloudProcessDialog::Operation op) {
        const Uuid id = need(EntityKind::PointCloud, CloudProcessDialog::title(op));
        if (id.isNil()) return;
        auto* d = new CloudProcessDialog(ctx_, op, id, this);
        if (nonInteractive_) {
            d->setAttribute(Qt::WA_DeleteOnClose);
            d->start();
            d->close();
        } else {
            runDialog(d);
        }
    };
    auto chooseFiles = [this](const QString& title, const QString& filter, const QString& key) {
        if (nonInteractive_) return QStringList();
        const QStringList f = QFileDialog::getOpenFileNames(this, title, ctx_.settings().value("dirs/" + key).toString(), filter);
        if (!f.isEmpty()) ctx_.settings().setValue("dirs/" + key, QFileInfo(f.front()).absolutePath());
        return f;
    };

    // ---- general
    if (action == "zoom") {
        if (auto* v = activeView()) v->fitSelection();
        return;
    }
    if (action == "properties") {
        dock("properties")->show();
        dock("properties")->raise();
        return;
    }
    if (action == "select-layer-objects") {
        std::vector<SelectionItem> sel;
        for (const auto& l : all(EntityKind::Layer))
            for (const auto& g : m.geometry.all())
                if (g.layerId == l) sel.push_back({EntityKind::Geometry, g.id});
        ctx_.selectMany(sel);
        ctx_.report(tr("%1 object(s) selected").arg(sel.size()));
        return;
    }
    if (action == "new-layer") {
        QString name = tr("Layer %1").arg(m.layers.size() + 1);
        if (!nonInteractive_) {
            bool ok = false;
            name = QInputDialog::getText(this, tr("New layer"), tr("Layer name:"), QLineEdit::Normal, name, &ok);
            if (!ok || name.trimmed().isEmpty()) return;
        }
        if (layers_->addLayer(name.trimmed(), categoricalColor(m.layers.size()))) {
            dock("layers")->show();
            dock("layers")->raise();
        }
        return;
    }

    // ---- survey
    if (action == "import-cloud") {
        const QStringList files = chooseFiles(tr("Import point clouds"), tr("Point clouds (*.las *.laz *.e57 *.ply *.xyz *.txt *.csv *.pts *.ptx);;All files (*)"), "clouds");
        if (files.isEmpty()) return;
        auto* d = new PointCloudImportDialog(ctx_, files, this);
        runDialog(d);
        return;
    }
    if (action == "import-control") {
        const QStringList files = chooseFiles(tr("Import control points"), tr("Delimited text (*.csv *.txt);;All files (*)"), "control");
        if (files.isEmpty()) return;
        runDialog(new ControlImportDialog(ctx_, files.front(), this));
        return;
    }
    if (action == "report-cloud") {
        if (!need(EntityKind::PointCloud, tr("Point-cloud report")).isNil()) reports_->generate("pointcloud", "pdf");
        return;
    }
    if (action == "export-cloud") {
        const Uuid id = need(EntityKind::PointCloud, tr("Export point cloud"));
        if (id.isNil()) return;
        QString f = argument;
        if (f.isEmpty()) {
            QStringList filters;
            for (const auto& e : survey::writableExtensions()) filters << QStringLiteral("*%1").arg(qstr(e));
            f = chooseSaveFile(tr("Export point cloud"), tr("Point clouds (%1)").arg(filters.join(' ')), "clouds", qstr(m.pointClouds.find(id)->name) + ".laz");
        }
        if (f.isEmpty()) return;
        ctx_.select(EntityKind::PointCloud, id);
        exportFile(f);
        return;
    }
    if (action == "cloud-stats") return cloudOp(CloudProcessDialog::Operation::Statistics);
    if (action == "cloud-subsample") return cloudOp(CloudProcessDialog::Operation::Subsample);
    if (action == "cloud-outliers") return cloudOp(CloudProcessDialog::Operation::Outliers);
    if (action == "cloud-normals") return cloudOp(CloudProcessDialog::Operation::Normals);
    if (action == "cloud-ground") return cloudOp(CloudProcessDialog::Operation::Ground);
    if (action == "cloud-delaunay") return cloudOp(CloudProcessDialog::Operation::Delaunay);
    if (action == "cloud-poisson") return cloudOp(CloudProcessDialog::Operation::Poisson);
    if (action == "cloud-distance") return cloudOp(CloudProcessDialog::Operation::Distance);
    if (action == "cloud-planes") return cloudOp(CloudProcessDialog::Operation::Planes);
    if (action == "cloud-segment") return cloudOp(CloudProcessDialog::Operation::Segment);
    if (action == "cloud-crop-box") {
        const Uuid id = need(EntityKind::PointCloud, tr("Crop by box"));
        if (id.isNil()) return;
        auto* d = new CloudProcessDialog(ctx_, CloudProcessDialog::Operation::CropBox, id, this);
        // A section slab in the active view gives a sensible default box.
        if (auto* v = activeView(); v && v->section().enabled) {
            const auto& sec = v->section();
            const auto* pc = m.pointClouds.find(id);
            BoundingBox bb = BoundingBox::empty();
            if (pc)
                for (int k = 0; k < 8; ++k)
                    bb.expand(pc->transform.apply({(k & 1) ? pc->storedBounds.max.x : pc->storedBounds.min.x, (k & 2) ? pc->storedBounds.max.y : pc->storedBounds.min.y,
                                                   (k & 4) ? pc->storedBounds.max.z : pc->storedBounds.min.z}));
            if (!bb.isEmpty() && std::abs(sec.normal.z) < 1e-9 && (std::abs(sec.normal.x) > 0.999 || std::abs(sec.normal.y) > 0.999)) {
                if (std::abs(sec.normal.x) > 0.999) {
                    bb.min.x = sec.point.x - sec.halfWidth;
                    bb.max.x = sec.point.x + sec.halfWidth;
                } else {
                    bb.min.y = sec.point.y - sec.halfWidth;
                    bb.max.y = sec.point.y + sec.halfWidth;
                }
                d->setBox(bb, true);
            }
        }
        if (nonInteractive_) {
            d->setAttribute(Qt::WA_DeleteOnClose);
            d->start();
            d->close();
        } else {
            runDialog(d);
        }
        return;
    }
    if (action == "solid-check") {
        QStringList lines;
        for (const auto& id : ctx_.selectedGeometry())
            if (const auto* g = m.geometry.find(id); g && (g->type == GeometryType::Solid || g->mesh)) lines << solidReport(p, id);
        if (lines.isEmpty()) {
            ctx_.report(tr("Select solids or meshes to check"), MessageLevel::Warning);
            return;
        }
        for (const auto& l : lines) ctx_.report(l);
        if (!nonInteractive_) {
            QMessageBox box(this);
            box.setWindowTitle(tr("Solid check"));
            box.setText(lines.join("\n\n"));
            box.exec();
        }
        return;
    }
    if (action == "solid-heal") {
        int n = 0;
        for (const auto& id : ctx_.selectedGeometry())
            if (const auto* g = m.geometry.find(id); g && g->type == GeometryType::Solid) n += healSolid(ctx_, id) ? 1 : 0;
        if (!n) ctx_.report(tr("Select the solids to heal"), MessageLevel::Warning);
        return;
    }
    if (action == "solid-fillet" || action == "solid-offset") {
        const bool fillet = action == "solid-fillet";
        std::vector<Uuid> solids;
        for (const auto& id : ctx_.selectedGeometry())
            if (const auto* g = m.geometry.find(id); g && g->type == GeometryType::Solid) solids.push_back(id);
        if (solids.empty()) {
            ctx_.report(fillet ? tr("Select the solids whose edges to fillet") : tr("Select the solids to offset"), MessageLevel::Warning);
            return;
        }
#ifdef OPENMINE_APP_HAVE_OCCT
        double value = argument.isEmpty() ? (fillet ? 0.5 : 0.25) : argument.toDouble();
        if (!nonInteractive_ && argument.isEmpty()) {
            bool ok = false;
            value = fillet ? QInputDialog::getDouble(this, tr("Fillet solid edges"), tr("Radius (m):"), value, 0.001, 1000, 3, &ok)
                           : QInputDialog::getDouble(this, tr("Offset solid"), tr("Distance (m, negative inwards):"), value, -1000, 1000, 3, &ok);
            if (!ok) return;
        }
        ctx_.tryEdit(fillet ? tr("Fillet solid edges") : tr("Offset solid"), [&] {
            CommandStack::MacroScope macro(p.commands(), fillet ? "Fillet solid edges" : "Offset solids");
            for (const auto& id : solids) {
                const auto info = fillet ? occt::filletSolidEdges(p, id, value) : occt::offsetSolidObject(p, id, value);
                ctx_.report(tr("%1: %2 m³, %3").arg(qstr(m.geometry.find(id)->name), formatNumber(info.volume, 3), info.valid ? tr("valid") : tr("NOT VALID")),
                            info.valid ? MessageLevel::Info : MessageLevel::Warning);
            }
            macro.commit();
        });
#else
        ctx_.report(tr("Solid modelling needs Open CASCADE, which this build does not include"), MessageLevel::Error);
#endif
        return;
    }
    if (action == "section-objects") {
        std::vector<Uuid> ids;
        for (const auto& id : ctx_.selectedGeometry())
            if (const auto* g = m.geometry.find(id); g && g->mesh) ids.push_back(id);
        if (ids.empty()) {
            ctx_.report(tr("Select the solids or meshes to section"), MessageLevel::Warning);
            return;
        }
        Vec3 point{0, 0, activeView() ? activeView()->workPlaneZ() : 0.0}, normal{0, 0, 1};
        if (auto* v = activeView(); v && v->section().enabled) {
            point = v->section().point;
            normal = v->section().normal;
        } else {
            for (auto* view : viewports_->views())
                if (view->section().enabled) {
                    point = view->section().point;
                    normal = view->section().normal;
                }
        }
        sectionObjects(ctx_, ids, point, normal);
        return;
    }
    if (action == "resources") {
        auto* d = new ResourceLibraryDialog(ctx_, this);
        d->setProperty("nonInteractive", nonInteractive_);
        runDialog(d, true);
        return;
    }
    if (action == "calendars" || action == "edit-calendar") {
        runDialog(new CalendarDialog(ctx_, first(EntityKind::Calendar), this), true);
        return;
    }
    if (action == "cycles") {
        runDialog(new CycleDialog(ctx_, this), true);
        return;
    }
    if (action == "targets") {
        runDialog(new TargetsDialog(ctx_, this), true);
        return;
    }
    if (action == "cloud-crop") {
        if (need(EntityKind::PointCloud, tr("Crop")).isNil()) return;
        startTool(static_cast<int>(Tool::CropPolygon));
        return;
    }
    if (action == "register-pairs") {
        const Uuid id = need(EntityKind::PointCloud, tr("Registration"));
        if (!id.isNil()) runDialog(new RegistrationDialog(ctx_, viewports_->tools(), id, this), true);
        return;
    }
    if (action == "register-icp") {
        const Uuid id = need(EntityKind::PointCloud, tr("ICP registration"));
        if (!id.isNil()) runDialog(new IcpDialog(ctx_, id, this));
        return;
    }
    if (action == "report-registration") {
        if (!need(EntityKind::Registration, tr("Registration report")).isNil()) reports_->generate("registration", "pdf");
        return;
    }
    if (action == "revert-registration") {
        const Uuid id = need(EntityKind::Registration, tr("Revert registration"));
        if (id.isNil()) return;
        if (!nonInteractive_ &&
            QMessageBox::question(this, tr("Revert registration"),
                                  tr("Restore the dataset's transform from before this registration? The registration record is kept for the audit trail.")) !=
                QMessageBox::Yes)
            return;
        ctx_.tryEdit(tr("Revert registration"), [&] {
            survey::revertRegistration(p, id);
            ctx_.report(tr("Registration reverted"));
        });
        return;
    }
    if (action == "asbuilt") {
        const Uuid id = need(EntityKind::PointCloud, tr("As-built"));
        if (!id.isNil()) runDialog(new AsBuiltDialog(ctx_, id, this));
        return;
    }
    if (action == "conformance") {
        runDialog(new ConformanceDialog(ctx_, first(EntityKind::AsBuilt), this));
        return;
    }
    if (action == "report-conformance") {
        if (!need(EntityKind::Conformance, tr("Conformance report")).isNil()) reports_->generate("conformance", "pdf");
        return;
    }
    if (action == "export-conformance") {
        const Uuid id = need(EntityKind::Conformance, tr("Export conformance"));
        if (id.isNil()) return;
        const auto* c = m.conformanceResults.find(id);
        const auto* a = c ? m.analysisResults.find(c->analysisResultId) : nullptr;
        if (!a || a->payloadJson.empty()) {
            ctx_.report(tr("The conformance result has no station table"), MessageLevel::Error);
            return;
        }
        QString f = argument;
        if (f.isEmpty()) {
            const auto* dev = m.developments.find(c->developmentId);
            f = chooseSaveFile(tr("Export station table"), tr("CSV (*.csv)"), "conformance", (dev ? qstr(dev->headingId) : QString("conformance")) + "_stations.csv");
        }
        if (f.isEmpty()) return;
        try {
            const auto rows = nlohmann::json::parse(a->payloadJson);
            std::string out = "chainage,valid,design_area_m2,asbuilt_area_m2,overbreak_area_m2,underbreak_area_m2,max_overbreak_m,max_underbreak_m,clearance_m,issue\n";
            for (const auto& r : rows) {
                auto num = [&](const char* k) { return r.contains(k) && r[k].is_number() ? reporting::fmtNumber(r[k].get<double>(), 4) : std::string(); };
                std::string issue = r.value("issue", std::string());
                std::replace(issue.begin(), issue.end(), '"', '\'');
                out += num("chainage") + "," + (r.value("valid", false) ? "1" : "0") + "," + num("designArea") + "," + num("asBuiltArea") + "," +
                       num("overbreakArea") + "," + num("underbreakArea") + "," + num("maxOverbreak") + "," + num("maxUnderbreak") + "," + num("clearance") +
                       ",\"" + issue + "\"\n";
            }
            writeText(fsPath(f), out);
            ctx_.report(tr("Exported %1 stations to %2").arg(rows.size()).arg(QFileInfo(f).fileName()));
        } catch (const std::exception& e) {
            ctx_.report(tr("Export failed: %1").arg(qstr(e.what())), MessageLevel::Error);
        }
        return;
    }
    if (action == "face-advance") {
        runDialog(new FaceAdvanceDialog(ctx_, development(), this));
        return;
    }
    if (action == "surface-volume") {
        Uuid surface;
        for (const auto& g : ctx_.selectedGeometry())
            if (const auto* o = m.geometry.find(g); o && o->type == GeometryType::Mesh) surface = g;
        runDialog(new VolumeDialog(ctx_, surface, this));
        return;
    }

    // ---- design
    if (action == "heading-from-geometry") {
        Uuid g;
        for (const auto& id : ctx_.selectedGeometry())
            if (const auto* o = m.geometry.find(id);
                o && (o->type == GeometryType::Polyline || o->type == GeometryType::Line || o->type == GeometryType::Arc || o->type == GeometryType::Spline))
                g = id;
        if (g.isNil()) {
            ctx_.report(tr("Select the centreline (line, polyline, arc or spline)"), MessageLevel::Warning);
            return;
        }
        if (const auto* d = m.developmentForGeometry(g)) {
            ctx_.report(tr("That centreline already belongs to heading %1").arg(qstr(d->headingId)), MessageLevel::Warning);
            return;
        }
        runDialog(new HeadingDialog(ctx_, g, this));
        return;
    }
    if (action == "linked-activity") {
        const Uuid dev = development();
        if (dev.isNil()) {
            ctx_.report(tr("Select a heading"), MessageLevel::Warning);
            return;
        }
        const auto* d = m.developments.find(dev);
        double rate = 4.0;
        if (!nonInteractive_) {
            bool ok = false;
            rate = QInputDialog::getDouble(this, tr("Linked activity"), tr("Advance rate for %1 (m/day):").arg(qstr(d->headingId)), rate, 0.01, 1000, 2, &ok);
            if (!ok) return;
        }
        ctx_.tryEdit(tr("Create linked activity"), [&] {
            const Uuid a = mine_design::createLinkedActivity(p, dev, rate);
            const auto* act = m.activities.find(a);
            ctx_.report(tr("Activity '%1' created: %2 m at %3 m/day, linked to %4")
                            .arg(qstr(act->name), formatNumber(act->quantity, 1), formatNumber(rate, 2), qstr(d->headingId)));
            ctx_.select(EntityKind::Activity, a);
        });
        return;
    }
    if (action == "section-chainage") {
        const Uuid dev = development();
        if (dev.isNil()) {
            ctx_.report(tr("Select a heading"), MessageLevel::Warning);
            return;
        }
        const auto* d = m.developments.find(dev);
        const auto metrics = mine_design::computeMetrics(p, *d);
        double ch = argument.isEmpty() ? d->chainageFrom + metrics.length3d / 2 : argument.toDouble();
        if (!nonInteractive_ && argument.isEmpty()) {
            bool ok = false;
            ch = QInputDialog::getDouble(this, tr("Section"), tr("Chainage on %1 (%2 – %3):").arg(qstr(d->headingId), formatNumber(d->chainageFrom), formatNumber(d->chainageFrom + metrics.length3d)),
                                         ch, d->chainageFrom, d->chainageFrom + metrics.length3d, 3, &ok);
            if (!ok) return;
        }
        showSectionAlong(viewports_, dev, ch);
        return;
    }
    if (action == "crosscuts") {
        const Uuid dev = development();
        if (dev.isNil()) {
            ctx_.report(tr("Select the parent heading"), MessageLevel::Warning);
            return;
        }
        runDialog(new CrosscutDialog(ctx_, dev, this));
        return;
    }
    if (action == "capture-revision") {
        Uuid id = development();
        if (id.isNil()) id = first(EntityKind::Stope);
        if (id.isNil()) id = first(EntityKind::MiningBlock);
        if (id.isNil()) {
            ctx_.report(tr("Select a heading, stope or mining block"), MessageLevel::Warning);
            return;
        }
        runDialog(new RevisionDialog(ctx_, id, this));
        return;
    }
    if (action == "compare-revision") {
        const Uuid id = need(EntityKind::DesignRevision, tr("Compare revision"));
        if (id.isNil()) return;
        const auto revs = all(EntityKind::DesignRevision);
        const std::optional<Uuid> to = revs.size() >= 2 ? std::optional<Uuid>(revs.back()) : std::nullopt;
        const Uuid from = revs.size() >= 2 ? revs[revs.size() - 2] : id;
        const QString text = revisionComparisonText(m, from, to, p);
        ctx_.report(text.section('\n', 0, 0));
        if (!nonInteractive_) {
            QMessageBox box(this);
            box.setWindowTitle(tr("Revision comparison"));
            box.setText(text);
            box.exec();
        }
        return;
    }
    if (action == "approve-revision") {
        const Uuid id = need(EntityKind::DesignRevision, tr("Approve revision"));
        if (id.isNil()) return;
        ctx_.tryEdit(tr("Approve revision"), [&] {
            mine_design::approveRevision(p, id);
            ctx_.report(tr("Revision %1 approved").arg(qstr(m.designRevisions.find(id)->revisionCode)));
        });
        return;
    }
    if (action == "restore-revision") {
        const Uuid id = need(EntityKind::DesignRevision, tr("Restore revision"));
        if (id.isNil()) return;
        if (!nonInteractive_ &&
            QMessageBox::question(this, tr("Restore revision"),
                                  tr("Restore the design to revision %1? Linked schedule activities are flagged for review, not changed. This can be undone.")
                                      .arg(qstr(m.designRevisions.find(id)->revisionCode))) != QMessageBox::Yes)
            return;
        ctx_.tryEdit(tr("Restore revision"), [&] {
            mine_design::restoreRevision(p, id);
            ctx_.report(tr("Revision %1 restored").arg(qstr(m.designRevisions.find(id)->revisionCode)));
        });
        return;
    }
    if (action == "apply-quantities") {
        std::vector<Uuid> acts = all(EntityKind::Activity);
        for (const auto& dev : all(EntityKind::Development))
            for (const auto& a : m.activitiesForDevelopment(dev)) acts.push_back(a);
        if (acts.empty()) {
            scheduler_->applyDesignQuantities();
            return;
        }
        ctx_.tryEdit(tr("Apply design quantities"), [&] {
            CommandStack::MacroScope macro(p.commands(), "Apply design quantities");
            for (const auto& a : acts) mine_design::applyDesignQuantities(p, a);
            macro.commit();
            ctx_.report(tr("Quantities of %1 activit%2 set from the current design").arg(acts.size()).arg(acts.size() == 1 ? "y" : "ies"));
        });
        return;
    }
    if (action == "impacts") {
        const auto list = mine_design::detectDesignImpacts(p);
        if (list.empty()) ctx_.report(tr("No design changes are pending schedule review"));
        std::vector<SelectionItem> sel;
        for (const auto& i : list) {
            ctx_.report(tr("%1: approved %2 m → current %3 m; %4 linked activit%5 flagged")
                            .arg(qstr(i.headingId), formatNumber(i.approvedLength, 2), formatNumber(i.currentLength, 2))
                            .arg(i.activityIds.size())
                            .arg(i.activityIds.size() == 1 ? "y" : "ies"),
                        MessageLevel::Warning);
            sel.push_back({EntityKind::Development, i.developmentId});
        }
        if (!sel.empty()) ctx_.selectMany(sel);
        dock("console")->show();
        dock("console")->raise();
        return;
    }
    if (action == "gradient") {
        Uuid g;
        for (const auto& id : ctx_.selectedGeometry())
            if (const auto* o = m.geometry.find(id); o && o->vertices.size() >= 2 && o->type != GeometryType::Mesh && o->type != GeometryType::Solid) g = id;
        if (g.isNil()) {
            ctx_.report(tr("Select a centreline or heading"), MessageLevel::Warning);
            return;
        }
        runDialog(new GradientDialog(ctx_, g, this));
        return;
    }
    if (action == "development-solid") {
#ifdef OPENMINE_APP_HAVE_OCCT
        std::vector<Uuid> devs = all(EntityKind::Development);
        if (devs.empty())
            if (const Uuid d = development(); !d.isNil()) devs.push_back(d);
        if (devs.empty()) {
            ctx_.report(tr("Select the headings"), MessageLevel::Warning);
            return;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        ctx_.tryEdit(tr("Excavation solid"), [&] {
            CommandStack::MacroScope macro(p.commands(), "Excavation solids");
            const Uuid layer = workflowLayer(ctx_, "Excavation solids", 0x8FBF6F);
            std::vector<SelectionItem> sel;
            double volume = 0;
            for (const auto& d : devs) {
                const Uuid s = occt::developmentSolid(p, d, layer);
                sel.push_back({EntityKind::Geometry, s});
                if (const auto* g = m.geometry.find(s); g && g->mesh) volume += std::abs(geometry::meshSignedVolume(*g->mesh));
            }
            macro.commit();
            ctx_.report(tr("%1 excavation solid(s), %2 m³ in total").arg(devs.size()).arg(formatNumber(volume, 1)));
            ctx_.selectMany(sel);
        });
        QApplication::restoreOverrideCursor();
#else
        ctx_.report(tr("Excavation solids need Open CASCADE (not in this build)"), MessageLevel::Error);
#endif
        return;
    }
    if (action == "stope-from-geometry") {
        Uuid g;
        for (const auto& id : ctx_.selectedGeometry())
            if (const auto* o = m.geometry.find(id);
                o && (o->type == GeometryType::Mesh || o->type == GeometryType::Solid || ((o->type == GeometryType::Polyline || o->type == GeometryType::Circle) && (o->closed || o->type == GeometryType::Circle))))
                g = id;
        if (g.isNil()) {
            ctx_.report(tr("Select a closed shape (mesh or solid) or a closed outline"), MessageLevel::Warning);
            return;
        }
        runDialog(new StopeDialog(ctx_, g, this));
        return;
    }
    if (action == "subdivide-stope") {
        const Uuid id = need(EntityKind::Stope, tr("Subdivide"));
        if (!id.isNil()) runDialog(new SubdivideDialog(ctx_, id, this));
        return;
    }
    if (action == "new-level") {
        runDialog(new LevelDialog(ctx_, this));
        return;
    }

    // ---- CAD / solids
    if (action == "join") {
        const auto ids = ctx_.selectedGeometry();
        ctx_.tryEdit(tr("Join"), [&] {
            const auto r = mine_design::joinObjects(p, ids);
            for (const auto& n : r.notes) ctx_.report(qstr(n), MessageLevel::Warning);
            std::vector<SelectionItem> sel;
            for (const auto& id : r.created) sel.push_back({EntityKind::Geometry, id});
            ctx_.report(tr("%1 objects joined into %2").arg(r.removed.size()).arg(r.created.size()));
            ctx_.selectMany(sel);
        });
        return;
    }
    if (action == "explode") {
        const auto ids = ctx_.selectedGeometry();
        if (ids.empty()) {
            ctx_.report(tr("Explode: select objects"), MessageLevel::Warning);
            return;
        }
        ctx_.tryEdit(tr("Explode"), [&] {
            const auto r = mine_design::explodeObjects(p, ids);
            for (const auto& n : r.notes) ctx_.report(qstr(n), MessageLevel::Warning);
            std::vector<SelectionItem> sel;
            for (const auto& id : r.created) sel.push_back({EntityKind::Geometry, id});
            ctx_.report(tr("%1 object(s) exploded into %2").arg(r.removed.size()).arg(r.created.size()));
            ctx_.selectMany(sel);
        });
        return;
    }
#ifdef OPENMINE_APP_HAVE_OCCT
    if (action == "union" || action == "subtract" || action == "intersect") {
        const auto geoms = ctx_.selectedGeometry();
        if (geoms.size() < 2) {
            ctx_.report(tr("Select two solids, meshes or closed outlines (the first is the one kept for subtract)"), MessageLevel::Warning);
            return;
        }
        bool removeInputs = false;
        if (!nonInteractive_)
            removeInputs = QMessageBox::question(this, tr("Boolean"), tr("Remove the input objects? (Undo restores them.)"),
                                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
        const auto op = action == "union" ? occt::BooleanOp::Union : action == "subtract" ? occt::BooleanOp::Difference : occt::BooleanOp::Intersection;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        ctx_.tryEdit(tr("Boolean"), [&] {
            CommandStack::MacroScope macro(p.commands(), sstr(action) + " solids");
            Uuid acc = geoms.front();
            for (std::size_t i = 1; i < geoms.size(); ++i) {
                const auto* a = m.geometry.find(acc);
                const std::string name = (a ? a->name : std::string("solid")) + " " + sstr(action);
                const Uuid next = occt::booleanObjects(p, acc, geoms[i], op, removeInputs, name);
                // Intermediate results of a chain (three or more inputs) are not kept.
                if (i > 1 && !removeInputs) ProjectEditor(p).removeGeometry(acc);
                acc = next;
            }
            macro.commit();
            const auto* r = m.geometry.find(acc);
            ctx_.report(tr("Result '%1'%2").arg(qstr(r->name), r->mesh ? tr(": %1 m³").arg(formatNumber(std::abs(geometry::meshSignedVolume(*r->mesh)), 2)) : QString()));
            ctx_.select(EntityKind::Geometry, acc);
        });
        QApplication::restoreOverrideCursor();
        return;
    }
    if (action == "mesh-to-solid") {
        std::vector<Uuid> meshes;
        for (const auto& id : ctx_.selectedGeometry())
            if (const auto* g = m.geometry.find(id); g && g->type == GeometryType::Mesh) meshes.push_back(id);
        if (meshes.empty()) {
            ctx_.report(tr("Select closed meshes"), MessageLevel::Warning);
            return;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        ctx_.tryEdit(tr("Mesh to solid"), [&] {
            CommandStack::MacroScope macro(p.commands(), "Mesh to solid");
            std::vector<SelectionItem> sel;
            for (const auto& id : meshes) sel.push_back({EntityKind::Geometry, occt::solidFromMeshObject(p, id, true)});
            macro.commit();
            ctx_.report(tr("%1 solid(s) created").arg(sel.size()));
            ctx_.selectMany(sel);
        });
        QApplication::restoreOverrideCursor();
        return;
    }
#else
    if (action == "union" || action == "subtract" || action == "intersect" || action == "mesh-to-solid") {
        ctx_.report(tr("Solid modelling needs Open CASCADE (not in this build)"), MessageLevel::Error);
        return;
    }
#endif

    // ---- geology / geotechnical
    if (action == "import-drillholes") {
        runDialog(new DrillholeImportDialog(ctx_, this));
        return;
    }
    if (action == "composite") {
        runDialog(new CompositeDialog(ctx_, this));
        return;
    }
    if (action == "block-display") {
        const Uuid id = need(EntityKind::BlockModel, tr("Block display"));
        if (!id.isNil()) runDialog(new BlockDisplayDialog(ctx_, viewports_->scene(), id, this));
        return;
    }
    if (action == "block-code") {
        const Uuid id = need(EntityKind::BlockModel, tr("Code blocks"));
        if (!id.isNil()) runDialog(new BlockCodeDialog(ctx_, id, this));
        return;
    }
    if (action == "estimate") {
        const Uuid id = need(EntityKind::BlockModel, tr("Estimation"));
        if (!id.isNil()) runDialog(new EstimationDialog(ctx_, id, this));
        return;
    }
    if (action == "grade-tonnage") {
        if (!need(EntityKind::BlockModel, tr("Grade–tonnage")).isNil()) reports_->generate("gradetonnage", "pdf");
        return;
    }
    if (action == "export-blocks") {
        const Uuid id = need(EntityKind::BlockModel, tr("Export block model"));
        if (id.isNil()) return;
        QString f = argument;
        if (f.isEmpty()) f = chooseSaveFile(tr("Export block model"), tr("CSV (*.csv)"), "blocks", qstr(m.blockModels.find(id)->name) + ".csv");
        if (f.isEmpty()) return;
        ctx_.select(EntityKind::BlockModel, id);
        exportFile(f);
        return;
    }
    if (action == "import-structures") {
        const QStringList files = chooseFiles(tr("Import structural measurements"), tr("Delimited text (*.csv *.txt);;All files (*)"), "structures");
        if (files.isEmpty()) return;
        runDialog(new StructureImportDialog(ctx_, files.front(), this));
        return;
    }
    if (action == "stereonet") {
        stereonet_->recompute();
        dock("stereonet")->show();
        dock("stereonet")->raise();
        return;
    }
    if (action == "classify") {
        runDialog(new ClassificationDialog(ctx_, development(), this));
        return;
    }

    // ---- reports
    if (action == "open-report") {
        const Uuid id = need(EntityKind::Report, tr("Open report"));
        if (id.isNil()) return;
        reports_->preview(id);
        dock("reports")->show();
        dock("reports")->raise();
        return;
    }
    if (action == "save-report") {
        const Uuid id = need(EntityKind::Report, tr("Save report"));
        if (id.isNil()) return;
        const auto* r = m.reports.find(id);
        QString f = argument;
        if (f.isEmpty()) f = chooseSaveFile(tr("Save report"), tr("Report (*.%1)").arg(qstr(r->format)), "reports", qstr(r->name) + "." + qstr(r->format));
        if (!f.isEmpty() && reports_->saveCopy(id, f)) ctx_.report(tr("Saved %1").arg(QFileInfo(f).fileName()));
        return;
    }

    // ---- schedule
    if (action == "new-task") {
        scheduler_->addActivity(ActivityKind::Task);
        dock("scheduler")->show();
        dock("scheduler")->raise();
        return;
    }
    if (action == "new-scenario") {
        scheduler_->editScenario(true);
        return;
    }
    if (action == "optimise") {
        if (const Uuid s = first(EntityKind::Scenario); !s.isNil()) ctx_.setActiveScenario(s);
        scheduler_->optimise();
        return;
    }
    if (action == "compare-scenario") {
        if (const Uuid s = first(EntityKind::Scenario); !s.isNil()) ctx_.setActiveScenario(s);
        reports_->generate("scenarios", "pdf");
        return;
    }
    if (action == "add-dependency") {
        scheduler_->addDependency();
        return;
    }
    if (action == "record-actual") {
        scheduler_->recordActual();
        return;
    }

    ctx_.report(tr("Unknown action '%1'").arg(action), MessageLevel::Error);
}

} // namespace openmine::app
