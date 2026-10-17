// SPDX-License-Identifier: Apache-2.0
#include "openmine/formats/DxfProject.h"

#include "openmine/commands/CommandStack.h"
#include "openmine/project/ProjectEditor.h"

#include <map>

namespace openmine::formats {

DxfProjectImportSummary importDxfIntoProject(Project& p, const DxfImportResult& dxf, bool reuse, const std::string& text) {
    DxfProjectImportSummary s;
    ProjectEditor ed(p);
    std::map<Uuid, Uuid> layerMap; // imported layer id → project layer id
    CommandStack::MacroScope macro(p.commands(), text);
    for (const auto& l : dxf.layers) {
        if (reuse)
            if (const auto* existing = ed.findLayerByName(l.name)) {
                layerMap[l.id] = existing->id;
                s.reusedLayers.push_back(existing->id);
                continue;
            }
        CadLayer nl = l;
        nl.id = Uuid{};
        const Uuid id = ed.add(nl);
        layerMap[l.id] = id;
        s.createdLayers.push_back(id);
    }
    for (const auto& g : dxf.objects) {
        GeometryObject o = g;
        o.id = Uuid{};
        const auto it = layerMap.find(g.layerId);
        if (it == layerMap.end()) throw ValidationError("Imported object references an unknown layer");
        o.layerId = it->second;
        s.objects.push_back(ed.add(std::move(o)));
    }
    macro.commit();
    return s;
}

DxfExportReport exportProjectDxf(const Project& p, const std::filesystem::path& file, const std::vector<Uuid>& ids,
                                 const DxfExportOptions& options) {
    const auto& m = p.model();
    std::vector<CadLayer> layers;
    for (const auto& l : m.layers.all()) layers.push_back(l);
    std::vector<const GeometryObject*> objs;
    if (ids.empty()) {
        for (const auto& g : m.geometry.all()) objs.push_back(&g);
    } else {
        for (const auto& id : ids) {
            const auto* g = m.geometry.find(id);
            if (!g) throw ValidationError("Geometry object to export does not exist");
            objs.push_back(g);
        }
    }
    return writeDxf(file, layers, objs, options);
}

} // namespace openmine::formats
