#pragma once
// SPDX-License-Identifier: Apache-2.0
// Fitting a local mine grid to a base CRS from control pairs (local ↔ base coordinates).
//
//  Helmert2D (5 parameters): E = s(cosθ·x − sinθ·y) + tE, N = s(sinθ·x + cosθ·y) + tN, Z = z + tZ
//      parameters = [tE, tN, tZ, s, θ (rad)]
//  Affine2D (7 parameters):  E = a·x + b·y + c, N = d·x + e·y + f, Z = z + tZ
//      parameters = [a, b, c, d, e, f, tZ]     (handles a mirrored grid and different axis scales)
//  Helmert3D (7 parameters): similarity in 3D (Umeyama), parameters = [tx, ty, tz, s, ωx, ωy, ωz]
//      (rotation vector, rad)
//
// Pairs marked `use = false` are check pairs: they do not enter the fit and their residuals are
// reported separately as an independent accuracy estimate.

#include "openmine/geometry/Transform.h"
#include "openmine/objects/ProjectEntities.h"

#include <string>
#include <vector>

namespace openmine::geometry {

struct GridControlPair {
    std::string name;
    Vec3 local;
    Vec3 base;
    bool use = true;
    Vec3 residual; ///< base − T(local), filled by the fit
};

struct LocalGridFit {
    LocalGridMethod method = LocalGridMethod::Helmert2D;
    Transform3 localToBase;
    std::vector<double> parameters;
    std::vector<GridControlPair> pairs; ///< with residuals
    double rms = 0, max = 0;            ///< fit pairs (horizontal for 2D methods, 3D for Helmert3D)
    double verticalRms = 0;
    int fitCount = 0, checkCount = 0;
    double checkRms = -1, checkMax = -1;
};

/// Least-squares fit. Throws std::invalid_argument for too few or degenerate pairs
/// (2D methods: ≥ 2 pairs for Helmert, ≥ 3 non-collinear for affine; Helmert3D: ≥ 3 non-collinear).
LocalGridFit fitLocalGrid(std::vector<GridControlPair> pairs, LocalGridMethod method);

/// JSON array of the pairs with residuals, as stored in LocalGridDefinition::controlPairsJson.
std::string pairsJson(const std::vector<GridControlPair>& pairs);
std::vector<GridControlPair> pairsFromJson(const std::string& json);

} // namespace openmine::geometry
