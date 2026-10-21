#pragma once
// SPDX-License-Identifier: Apache-2.0
// Files attached to project entities (photos of a face, a survey field sheet, a geotechnical
// report, a permit). The file is copied into the project's asset store (immutable, SHA-256
// verified); the Attachment entity records the entity it belongs to, the original file name and a
// MIME type. Attachments go with their entity when it is deleted (and come back on undo).

#include "openmine/project/Project.h"

#include <filesystem>
#include <string>
#include <vector>

namespace openmine {

/// Attaches a copy of `file` to the entity `entityId` of document type `entityType` (e.g.
/// "MineDevelopment"); a nil id attaches it to the project itself. One undo step; returns the
/// attachment id. Throws ValidationError for an unknown entity, AssetError when the file cannot
/// be read.
Uuid attachFile(Project& project, const std::string& entityType, const Uuid& entityId, const std::filesystem::path& file,
                const std::string& description = {});

/// Writes the attachment's bytes to `destination` after verifying them against the stored
/// SHA-256 (AssetError when the stored file is missing or was modified).
void saveAttachment(const Project& project, const Uuid& attachmentId, const std::filesystem::path& destination);

/// Attachments of one entity (nil: those of the project itself), in creation order.
std::vector<const Attachment*> attachmentsOf(const Project& project, const Uuid& entityId);

/// MIME type from a file extension (application/octet-stream when unknown).
std::string mimeTypeFor(const std::filesystem::path& file);

/// True when `type` names an entity type and `id` exists in it.
bool entityExists(const ProjectModel& model, const std::string& type, const Uuid& id);

} // namespace openmine
