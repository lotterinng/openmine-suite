#pragma once
// SPDX-License-Identifier: Apache-2.0
// Drillhole database import/export from delimited tables (collar, survey, any number of interval
// tables such as assays, lithology, geotechnical logging).
//
// Rules (nothing is changed silently — every rejected or adjusted record is reported):
//  * Columns are found by common names (HOLEID/BHID/DHID, X/EAST/EASTING, FROM/TO, AZIMUTH/DIP…)
//    or by an explicit mapping.
//  * A column is numeric when every non-missing value parses as a number or a detection-limit
//    value ("<0.01", ">10"); otherwise it is a code column (text). Columns can be forced to code.
//  * Missing values (configurable tokens, default "", "-", "NA", "N/A", "NaN", "-99", "-999") are
//    omitted from the interval. Below-detection values use the configured policy (default half the
//    limit) and are counted in the report; above-detection values use the limit.
//  * Interval tables are merged per hole by splitting at the union of their boundaries, so each
//    resulting interval carries every table's values (values are intensive, e.g. grades, so they
//    are unchanged by splitting).
//  * Rows that overlap a previous row of the same table, lie beyond the hole length, have from ≥ to,
//    or refer to a hole without a collar are rejected (error). Holes with invalid collars are rejected.

#include "openmine/geology/Desurvey.h"
#include "openmine/project/Project.h"
#include "openmine/util/Csv.h"

#include <map>
#include <string>
#include <vector>

namespace openmine::geology {

enum class BelowDetection { HalfLimit, Limit, Zero, Missing };

struct DrillholeImportOptions {
    std::vector<std::string> missingTokens{"", "-", "NA", "N/A", "NaN", "-99", "-999", "*"};
    BelowDetection belowDetection = BelowDetection::HalfLimit;
    std::vector<std::string> codeColumns;           ///< forced text columns (e.g. sample numbers)
    std::vector<std::string> ignoreColumns;
    std::map<std::string, std::string> columns;     ///< role → header: holeId, x, y, z, length, type, date, depth, azimuth, dip, from, to
    double defaultDip = -90, defaultAzimuth = 0;    ///< for holes without surveys (reported)
    bool replaceExisting = false;                   ///< update holes whose ID already exists in the project
};

struct DrillholeTables {
    util::CsvTable collars;
    util::CsvTable surveys;                          ///< optional (empty headers = none)
    std::vector<std::pair<std::string, util::CsvTable>> intervals; ///< table name → table
};

struct DrillholeIssue {
    enum class Severity { Info, Warning, Error } severity = Severity::Error;
    std::string table;
    std::size_t row = 0;  ///< 1-based data row (0 = not row-specific)
    std::string holeId;
    std::string message;
};

struct DrillholeReadResult {
    std::vector<Drillhole> holes;
    std::vector<DrillholeIssue> issues;
    std::map<std::string, std::string> fieldTables;   ///< field → source table
    std::vector<std::string> numericFields, codeFields;
    std::size_t belowDetectionCount = 0, aboveDetectionCount = 0, missingCount = 0;
    std::size_t rejectedRows = 0, rejectedHoles = 0;
    std::size_t errors() const;
};

DrillholeReadResult readDrillholes(const DrillholeTables& tables, const DrillholeImportOptions& options = {});

struct DrillholeImportSummary {
    std::vector<Uuid> created, updated;
    std::vector<std::string> skipped; ///< holes that exist and replaceExisting is false
};
/// Adds the holes to the project (one undo step).
DrillholeImportSummary importDrillholes(Project& project, const DrillholeReadResult& read, bool replaceExisting = false);

/// Splits two interval lists at the union of their boundaries and combines values and codes.
std::vector<DrillholeInterval> mergeIntervals(const std::vector<DrillholeInterval>& a, const std::vector<DrillholeInterval>& b);

/// Data-quality checks on a stored hole: gaps between intervals, unsampled lengths, dogleg
/// severity above `maxDoglegDegPer30m`, surveys missing or beyond the hole.
std::vector<DrillholeIssue> checkDrillhole(const Drillhole& hole, double maxDoglegDegPer30m = 6.0);

/// Writes collar, survey and interval tables (interval table has desurveyed start/mid/end).
struct DrillholeCsv {
    std::string collars, surveys, intervals;
};
DrillholeCsv exportDrillholesCsv(const std::vector<Drillhole>& holes, DesurveyMethod method = DesurveyMethod::MinimumCurvature);

/// Desurveyed polyline objects for display (one per hole), with the hole ID as name.
std::vector<Uuid> createDrillholeTraces(Project& project, const Uuid& layerId, double step = 1.0,
                                        DesurveyMethod method = DesurveyMethod::MinimumCurvature);

} // namespace openmine::geology
