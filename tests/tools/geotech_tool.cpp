// SPDX-License-Identifier: Apache-2.0
// Test helper for tests/integration/test_stereonet.py: reads a JSON request file, prints results.
//   {"op":"intersections","pairs":[[dip1,dd1,dip2,dd2],...]}
//   {"op":"fisher","lines":[[trend,plunge],...]}
//   {"op":"density","method":"schmidt"|"kamb","planes":[[dip,dd],...],"at":[[trend,plunge],...]}
//   {"op":"sets","planes":[[dip,dd],...],"k":3}
#include "openmine/geotechnical/Stereonet.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>

using namespace openmine;
using namespace openmine::geotechnical;
using nlohmann::json;

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream in(argv[1]);
    const json req = json::parse(in);
    const std::string op = req.at("op");
    json out;
    if (op == "intersections") {
        for (const auto& p : req.at("pairs")) {
            const auto i = intersection({p[0], p[1]}, {p[2], p[3]});
            out.push_back(i ? json{i->trend, i->plunge} : json(nullptr));
        }
    } else if (op == "fisher") {
        std::vector<Vec3> v;
        for (const auto& l : req.at("lines")) v.push_back(lineVector({l[0], l[1]}));
        const auto f = fisher(v);
        out = {{"trend", f.mean.trend}, {"plunge", f.mean.plunge}, {"k", f.k}, {"alpha95", f.alpha95}, {"r", f.resultant / v.size()}};
    } else if (op == "density") {
        std::vector<Vec3> poles, at;
        for (const auto& p : req.at("planes")) poles.push_back(poleVector({p[0], p[1]}));
        for (const auto& a : req.at("at")) at.push_back(lineVector({a[0], a[1]}));
        double cone = 0;
        out["values"] = densityAt(poles, {}, req.at("method") == "kamb" ? DensityMethod::Kamb : DensityMethod::Schmidt1Percent, at, &cone);
        out["cone"] = cone;
    } else if (op == "sets") {
        std::vector<Vec3> poles;
        for (const auto& p : req.at("planes")) poles.push_back(poleVector({p[0], p[1]}));
        const auto r = identifyJointSets(poles, req.at("k"), 0, 1);
        for (const auto& s : r.sets) out.push_back({s.meanPole.trend, s.meanPole.plunge, s.members.size()});
    }
    std::cout << out.dump() << "\n";
    return 0;
}
