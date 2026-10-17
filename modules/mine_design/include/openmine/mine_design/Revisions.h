#pragma once
// SPDX-License-Identifier: Apache-2.0
// Design revisions: immutable snapshots of developments and stopes with approval workflow
// and revision comparison.

#include "openmine/project/Project.h"

#include <optional>
#include <string>

namespace openmine::mine_design {

/// Captures the current state of a development or stope (entity + geometry) as revision `code`,
/// sets it as the object's current revision and updates its revision label. One undo step.
Uuid captureRevision(Project& p, const Uuid& entityId, const std::string& code, const std::string& comment = {});

/// Approves a revision (irreversible; recorded in the audit log).
void approveRevision(Project& p, const Uuid& revisionId);

struct RevisionComparison {
    std::string fromCode, toCode;
    double lengthChange = 0;       ///< m
    double volumeChange = 0;       ///< m³
    double maxVertexDeviation = 0; ///< m, max distance from each vertex of one geometry to the other (symmetric)
    int vertexCountFrom = 0, vertexCountTo = 0;
    std::vector<std::string> attributeChanges; ///< "status: Planned → In progress"
};

/// Compares two revisions of the same entity, or a revision with the current state (`toId` nil).
RevisionComparison compareRevisions(const Project& p, const Uuid& fromId, const std::optional<Uuid>& toId);

/// Restores geometry and attributes of a revision as a new edit (does not delete history).
void restoreRevision(Project& p, const Uuid& revisionId);

} // namespace openmine::mine_design
