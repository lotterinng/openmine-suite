#pragma once
// SPDX-License-Identifier: Apache-2.0
// Survey reports built from the stored results (what was computed is what is reported):
//  * design conformance — summary, overbreak/underbreak along chainage, the station table with
//    flagged stations and, when the full analysis is supplied, cross-sections of the worst stations;
//  * registration — method, transform (matrix, rotation, translation, scale), fit and independent
//    check residuals per point, ICP statistics, with the distinction between them explained;
//  * point-cloud dataset — provenance (source file, format, CRS, scanner, date, derivation chain),
//    attributes, stored and project bounds, transform, registrations and derived as-built models,
//    and optionally sampled spacing statistics.

#include "openmine/reporting/ReportDocument.h"
#include "openmine/survey/Conformance.h"

namespace openmine::reporting {

struct ConformanceReportOptions {
    double underbreakTolerance = 0.05;   ///< m: stations with deeper underbreak are highlighted
    double overbreakTolerance = 0.5;     ///< m: stations with deeper overbreak are highlighted
    int worstSections = 6;               ///< cross-sections drawn when the full analysis is given
};
ReportDocument conformanceReport(const Project& project, const Uuid& conformanceResultId, const survey::ConformanceReport* fullAnalysis = nullptr,
                                 const ConformanceReportOptions& options = {});

ReportDocument registrationReport(const Project& project, const Uuid& registrationId);

ReportDocument pointCloudReport(const Project& project, const Uuid& datasetId, bool spacingStatistics = false);

} // namespace openmine::reporting
