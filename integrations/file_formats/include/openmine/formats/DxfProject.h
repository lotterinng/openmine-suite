#pragma once
// SPDX-License-Identifier: Apache-2.0
// DXF import into / export from a project through ProjectEditor (one undo step, validated, audited).

#include "openmine/formats/Dxf.h"
#include "openmine/project/Project.h"

namespace openmine::formats {

struct DxfProjectImportSummary {
    std::vector<Uuid> createdLayers;
    std::vector<Uuid> reusedLayers;
    std::vector<Uuid> objects;
};

/// Adds the imported layers and objects to the project as one undoable command. Layers whose
/// name already exists are reused (their properties are not changed) when `reuseLayersByName`.
DxfProjectImportSummary importDxfIntoProject(Project& project, const DxfImportResult& dxf, bool reuseLayersByName = true,
                                             const std::string& commandText = "Import DXF");

/// Exports geometry objects (all when `ids` is empty) with their layers.
DxfExportReport exportProjectDxf(const Project& project, const std::filesystem::path& file, const std::vector<Uuid>& ids = {},
                                 const DxfExportOptions& options = {});

} // namespace openmine::formats
