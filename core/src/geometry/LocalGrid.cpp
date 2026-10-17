// SPDX-License-Identifier: Apache-2.0
#include "openmine/geometry/LocalGrid.h"

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>

namespace openmine::geometry {

namespace {
double rmsOf(const std::vector<double>& v) {
    if (v.empty()) return 0;
    double s = 0;
    for (double x : v) s += x * x;
    return std::sqrt(s / static_cast<double>(v.size()));
}
} // namespace

LocalGridFit fitLocalGrid(std::vector<GridControlPair> pairs, LocalGridMethod method) {
    std::vector<const GridControlPair*> fit;
    for (const auto& p : pairs)
        if (p.use) fit.push_back(&p);
    const std::size_t n = fit.size();
    if (n == 0) throw std::invalid_argument("No control pairs are marked for use in the fit");
    // Local origins (first fit pair) keep the normal equations well conditioned at mine coordinates.
    const Vec3 ol = fit[0]->local, ob = fit[0]->base;
    LocalGridFit r;
    r.method = method;
    std::array<double, 16> m{};
    m[15] = 1;
    double tz = 0;
    for (auto* p : fit) tz += (p->base.z - ob.z) - (p->local.z - ol.z);
    tz /= static_cast<double>(n);
    const double tZ = ob.z + tz - ol.z; // Z = z + tZ
    switch (method) {
    case LocalGridMethod::Helmert2D: {
        if (n < 2) throw std::invalid_argument("A 2D Helmert fit needs at least two control pairs");
        double mx = 0, my = 0, mE = 0, mN = 0;
        for (auto* p : fit) {
            mx += p->local.x - ol.x;
            my += p->local.y - ol.y;
            mE += p->base.x - ob.x;
            mN += p->base.y - ob.y;
        }
        mx /= n, my /= n, mE /= n, mN /= n;
        double sxx = 0, sab = 0, sba = 0;
        for (auto* p : fit) {
            const double x = p->local.x - ol.x - mx, y = p->local.y - ol.y - my, E = p->base.x - ob.x - mE, N = p->base.y - ob.y - mN;
            sxx += x * x + y * y;
            sab += x * E + y * N;
            sba += x * N - y * E;
        }
        if (sxx < 1e-18) throw std::invalid_argument("The control pairs coincide: the grid rotation is undetermined");
        const double a = sab / sxx, b = sba / sxx; // a = s cosθ, b = s sinθ
        const double cE = ob.x + mE - (a * (ol.x + mx) - b * (ol.y + my));
        const double cN = ob.y + mN - (b * (ol.x + mx) + a * (ol.y + my));
        m = {a, -b, 0, cE, b, a, 0, cN, 0, 0, 1, tZ, 0, 0, 0, 1};
        r.parameters = {cE, cN, tZ, std::hypot(a, b), std::atan2(b, a)};
        break;
    }
    case LocalGridMethod::Affine2D: {
        if (n < 3) throw std::invalid_argument("A 2D affine fit needs at least three control pairs");
        Eigen::MatrixXd A(static_cast<Eigen::Index>(n), 3);
        Eigen::VectorXd e(static_cast<Eigen::Index>(n)), nn(static_cast<Eigen::Index>(n));
        for (std::size_t i = 0; i < n; ++i) {
            const auto I = static_cast<Eigen::Index>(i);
            A(I, 0) = fit[i]->local.x - ol.x;
            A(I, 1) = fit[i]->local.y - ol.y;
            A(I, 2) = 1;
            e(I) = fit[i]->base.x - ob.x;
            nn(I) = fit[i]->base.y - ob.y;
        }
        Eigen::JacobiSVD<Eigen::MatrixXd> svd(A, Eigen::ComputeThinU | Eigen::ComputeThinV);
        if (svd.singularValues()(2) < 1e-9 * svd.singularValues()(0)) throw std::invalid_argument("The control pairs are collinear: an affine grid is undetermined");
        const Eigen::Vector3d pe = svd.solve(e), pn = svd.solve(nn);
        const double a = pe(0), b = pe(1), d = pn(0), ee = pn(1);
        const double c = ob.x + pe(2) - a * ol.x - b * ol.y, f = ob.y + pn(2) - d * ol.x - ee * ol.y;
        m = {a, b, 0, c, d, ee, 0, f, 0, 0, 1, tZ, 0, 0, 0, 1};
        r.parameters = {a, b, c, d, ee, f, tZ};
        break;
    }
    case LocalGridMethod::Helmert3D: {
        if (n < 3) throw std::invalid_argument("A 3D Helmert fit needs at least three control pairs");
        Eigen::Vector3d ms = Eigen::Vector3d::Zero(), md = Eigen::Vector3d::Zero();
        for (auto* p : fit) {
            ms += Eigen::Vector3d(p->local.x - ol.x, p->local.y - ol.y, p->local.z - ol.z);
            md += Eigen::Vector3d(p->base.x - ob.x, p->base.y - ob.y, p->base.z - ob.z);
        }
        ms /= static_cast<double>(n);
        md /= static_cast<double>(n);
        Eigen::Matrix3d S = Eigen::Matrix3d::Zero();
        double var = 0;
        for (auto* p : fit) {
            const Eigen::Vector3d a = Eigen::Vector3d(p->local.x - ol.x, p->local.y - ol.y, p->local.z - ol.z) - ms;
            const Eigen::Vector3d b = Eigen::Vector3d(p->base.x - ob.x, p->base.y - ob.y, p->base.z - ob.z) - md;
            S += b * a.transpose();
            var += a.squaredNorm();
        }
        Eigen::JacobiSVD<Eigen::Matrix3d> svd(S, Eigen::ComputeFullU | Eigen::ComputeFullV);
        if (svd.singularValues()(1) < 1e-12 * std::max(1.0, svd.singularValues()(0))) throw std::invalid_argument("The control pairs are collinear: a 3D Helmert fit is undetermined");
        Eigen::Matrix3d D = Eigen::Matrix3d::Identity();
        if ((svd.matrixU() * svd.matrixV().transpose()).determinant() < 0) D(2, 2) = -1;
        const Eigen::Matrix3d R = svd.matrixU() * D * svd.matrixV().transpose();
        const double s = (svd.singularValues().asDiagonal() * D).trace() / var;
        const Eigen::Vector3d t = Eigen::Vector3d(ob.x, ob.y, ob.z) + md - s * R * (Eigen::Vector3d(ol.x, ol.y, ol.z) + ms);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) m[static_cast<std::size_t>(i * 4 + j)] = s * R(i, j);
            m[static_cast<std::size_t>(i * 4 + 3)] = t(i);
        }
        m[12] = m[13] = m[14] = 0;
        m[15] = 1;
        const Eigen::AngleAxisd aa(R);
        const Eigen::Vector3d w = aa.axis() * aa.angle();
        r.parameters = {t(0), t(1), t(2), s, w(0), w(1), w(2)};
        break;
    }
    }
    r.localToBase = Transform3(m);
    std::vector<double> fitH, fitV, chk;
    double fitMax = 0;
    for (auto& p : pairs) {
        p.residual = p.base - r.localToBase.apply(p.local);
        const double d = method == LocalGridMethod::Helmert3D ? p.residual.length() : p.residual.horizontalLength();
        if (p.use) {
            fitH.push_back(d);
            fitV.push_back(p.residual.z);
            fitMax = std::max(fitMax, d);
        } else chk.push_back(d);
    }
    r.fitCount = static_cast<int>(fitH.size());
    r.rms = rmsOf(fitH);
    r.max = fitMax;
    r.verticalRms = rmsOf(fitV);
    r.checkCount = static_cast<int>(chk.size());
    if (!chk.empty()) {
        r.checkRms = rmsOf(chk);
        r.checkMax = *std::max_element(chk.begin(), chk.end());
    }
    r.pairs = std::move(pairs);
    return r;
}

std::string pairsJson(const std::vector<GridControlPair>& pairs) {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& p : pairs)
        a.push_back({{"name", p.name}, {"local", {p.local.x, p.local.y, p.local.z}}, {"base", {p.base.x, p.base.y, p.base.z}}, {"use", p.use},
                     {"residual", {p.residual.x, p.residual.y, p.residual.z}}});
    return a.dump();
}

std::vector<GridControlPair> pairsFromJson(const std::string& s) {
    std::vector<GridControlPair> out;
    if (s.empty()) return out;
    for (const auto& j : nlohmann::json::parse(s)) {
        GridControlPair p;
        p.name = j.value("name", "");
        p.local = {j.at("local")[0], j.at("local")[1], j.at("local")[2]};
        p.base = {j.at("base")[0], j.at("base")[1], j.at("base")[2]};
        p.use = j.value("use", true);
        if (j.contains("residual")) p.residual = {j["residual"][0], j["residual"][1], j["residual"][2]};
        out.push_back(p);
    }
    return out;
}

} // namespace openmine::geometry
