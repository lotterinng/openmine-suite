// SPDX-License-Identifier: Apache-2.0
#include "PointCloudLod.h"

#include "openmine/diagnostics/Log.h"

#include <QMetaObject>

#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkFloatArray.h>
#include <vtkIdTypeArray.h>
#include <vtkMatrix4x4.h>
#include <vtkPlane.h>
#include <vtkPlaneCollection.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkUnsignedCharArray.h>

#include <algorithm>
#include <cmath>
#include <queue>
#include <numbers>
#include <iterator>

namespace openmine::app {

std::size_t NodePoints::bytes() const {
    std::size_t b = xyz.size() * sizeof(float) + intensity.size() * 2 + rgb.size() + classification.size() + sizeof(NodePoints);
    for (const auto& [k, v] : scalars) b += v.size() * sizeof(float) + k.size();
    return b;
}

// ---- store -------------------------------------------------------------------------------------------

PointCloudStore::PointCloudStore(QObject* parent) : QObject(parent) {
    const unsigned n = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 3u);
    for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this] { loaderLoop(); });
}

PointCloudStore::~PointCloudStore() {
    {
        std::lock_guard lk(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

std::shared_ptr<const survey::OctreeReader> PointCloudStore::reader(const Uuid& assetId, const std::filesystem::path& folder) {
    {
        std::lock_guard lk(mutex_);
        if (auto it = readers_.find(assetId); it != readers_.end()) return it->second;
        if (readerErrors_.count(assetId)) return nullptr;
    }
    try {
        auto r = std::make_shared<const survey::OctreeReader>(folder);
        std::lock_guard lk(mutex_);
        readers_[assetId] = r;
        return r;
    } catch (const std::exception& e) {
        std::lock_guard lk(mutex_);
        readerErrors_[assetId] = e.what();
        emit loadFailed(QString::fromStdString(e.what()));
        return nullptr;
    }
}

std::shared_ptr<const NodePoints> PointCloudStore::node(const Uuid& assetId, const std::string& name) {
    std::lock_guard lk(mutex_);
    auto it = cache_.find(Key{assetId, name});
    if (it == cache_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.second);
    return it->second.first;
}

void PointCloudStore::request(int requester, std::vector<std::pair<Uuid, std::string>> nodes) {
    std::vector<Key> keys;
    keys.reserve(nodes.size());
    // Stored in reverse so the loader pops the highest priority from the back.
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) keys.push_back(Key{it->first, std::move(it->second)});
    {
        std::lock_guard lk(mutex_);
        requests_[requester] = std::move(keys);
    }
    cv_.notify_all();
}

void PointCloudStore::cancelRequests(int requester) {
    std::lock_guard lk(mutex_);
    requests_.erase(requester);
}

void PointCloudStore::clear() {
    std::lock_guard lk(mutex_);
    readers_.clear();
    readerErrors_.clear();
    cache_.clear();
    lru_.clear();
    requests_.clear();
    used_ = 0;
}

std::size_t PointCloudStore::memoryUsed() const {
    std::lock_guard lk(mutex_);
    return used_;
}

std::size_t PointCloudStore::queued() const {
    std::lock_guard lk(mutex_);
    std::size_t n = 0;
    for (const auto& [r, v] : requests_) n += v.size();
    return n;
}

void PointCloudStore::evict() {
    // Caller holds the mutex. Never evicts below a working set of 64 nodes.
    while (used_ > budget_ && lru_.size() > 64) {
        const Key k = lru_.back();
        lru_.pop_back();
        auto it = cache_.find(k);
        if (it != cache_.end()) {
            used_ -= it->second.first->bytes();
            cache_.erase(it);
        }
    }
}

void PointCloudStore::loaderLoop() {
    std::size_t turn = 0;
    for (;;) {
        Key key;
        std::shared_ptr<const survey::OctreeReader> reader;
        {
            std::unique_lock lk(mutex_);
            for (;;) {
                if (stop_) return;
                bool found = false;
                // Round-robin over requesters, taking each one's highest-priority missing node.
                std::vector<int> ids;
                for (const auto& [r, v] : requests_) ids.push_back(r);
                for (std::size_t k = 0; k < ids.size() && !found; ++k) {
                    auto& list = requests_[ids[(turn + k) % ids.size()]];
                    while (!list.empty()) {
                        Key cand = std::move(list.back());
                        list.pop_back();
                        if (cache_.count(cand) || inFlight_.count(cand)) continue;
                        auto rit = readers_.find(cand.asset);
                        if (rit == readers_.end()) continue;
                        key = std::move(cand);
                        reader = rit->second;
                        found = true;
                        break;
                    }
                }
                ++turn;
                if (found) break;
                cv_.wait(lk);
            }
            inFlight_.insert(key);
        }
        std::shared_ptr<NodePoints> np;
        std::string error;
        try {
            const auto* info = reader->node(key.node);
            const survey::PointBuffer b = reader->readNode(key.node);
            np = std::make_shared<NodePoints>();
            np->centre = info ? info->cube.center() : Vec3{};
            const std::size_t n = b.size();
            np->xyz.resize(n * 3);
            for (std::size_t i = 0; i < n; ++i) {
                const Vec3 d = b.xyz[i] - np->centre;
                np->xyz[i * 3] = static_cast<float>(d.x);
                np->xyz[i * 3 + 1] = static_cast<float>(d.y);
                np->xyz[i * 3 + 2] = static_cast<float>(d.z);
            }
            np->intensity = b.intensity;
            np->classification = b.classification;
            if (!b.rgb.empty()) {
                np->rgb.resize(n * 3);
                for (std::size_t i = 0; i < n; ++i) {
                    np->rgb[i * 3] = static_cast<std::uint8_t>(b.rgb[i].r >> 8);
                    np->rgb[i * 3 + 1] = static_cast<std::uint8_t>(b.rgb[i].g >> 8);
                    np->rgb[i * 3 + 2] = static_cast<std::uint8_t>(b.rgb[i].b >> 8);
                }
            }
            const auto& names = b.schema().scalarFields;
            for (std::size_t s = 0; s < names.size() && s < b.scalars.size(); ++s) np->scalars[names[s]] = b.scalars[s];
        } catch (const std::exception& e) {
            error = e.what();
        }
        bool notify = false;
        {
            std::lock_guard lk(mutex_);
            inFlight_.erase(key);
            if (np) {
                lru_.push_front(key);
                used_ += np->bytes();
                cache_[key] = {np, lru_.begin()};
                evict();
            }
            if (!notifyPending_) {
                notifyPending_ = true;
                notify = true;
            }
        }
        if (!error.empty()) {
            diag::warn("app.pointcloud", "Node " + key.node + " could not be read: " + error);
            QMetaObject::invokeMethod(this, [this, error] { emit loadFailed(QString::fromStdString(error)); }, Qt::QueuedConnection);
        }
        if (notify)
            QMetaObject::invokeMethod(
                this,
                [this] {
                    {
                        std::lock_guard lk(mutex_);
                        notifyPending_ = false;
                    }
                    emit nodesLoaded();
                },
                Qt::QueuedConnection);
    }
}

// ---- view --------------------------------------------------------------------------------------------

PointCloudView::PointCloudView(PointCloudStore& store, vtkRenderer* renderer, int requesterId)
    : store_(store), renderer_(renderer), requester_(requesterId) {}

PointCloudView::~PointCloudView() {
    store_.cancelRequests(requester_);
    clear();
}

void PointCloudView::clear() {
    for (auto& [k, na] : actors_) renderer_->RemoveActor(na.actor);
    actors_.clear();
    datasets_.clear();
    displayed_ = pending_ = 0;
}

vtkSmartPointer<vtkActor> PointCloudView::makeActor(const NodePoints& np) const {
    const auto n = static_cast<vtkIdType>(np.count());
    vtkNew<vtkFloatArray> coords;
    coords->SetNumberOfComponents(3);
    coords->SetNumberOfTuples(n);
    std::copy(np.xyz.begin(), np.xyz.end(), coords->GetPointer(0));
    vtkNew<vtkPoints> pts;
    pts->SetData(coords);
    vtkNew<vtkIdTypeArray> offsets, conn;
    offsets->SetNumberOfValues(n + 1);
    conn->SetNumberOfValues(n);
    for (vtkIdType i = 0; i < n; ++i) {
        offsets->SetValue(i, i);
        conn->SetValue(i, i);
    }
    offsets->SetValue(n, n);
    vtkNew<vtkCellArray> verts;
    verts->SetData(offsets, conn);
    vtkNew<vtkPolyData> pd;
    pd->SetPoints(pts);
    pd->SetVerts(verts);
    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputData(pd);
    mapper->SetColorModeToDirectScalars();
    mapper->ScalarVisibilityOn();
    mapper->SetScalarModeToUsePointData();
    if (!clipPlanes_.empty()) {
        for (const auto& [p, nrm] : clipPlanes_) {
            vtkNew<vtkPlane> pl;
            pl->SetOrigin(p.x, p.y, p.z);
            pl->SetNormal(nrm.x, nrm.y, nrm.z);
            mapper->AddClippingPlane(pl);
        }
    }
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->LightingOff();
    actor->PickableOff();
    return actor;
}

void PointCloudView::setClipPlanes(const std::vector<std::pair<Vec3, Vec3>>& planes) {
    clipPlanes_ = planes;
    for (auto& [k, na] : actors_) {
        auto* m = na.actor->GetMapper();
        m->RemoveAllClippingPlanes();
        for (const auto& [p, nrm] : clipPlanes_) {
            vtkNew<vtkPlane> pl;
            pl->SetOrigin(p.x, p.y, p.z);
            pl->SetNormal(nrm.x, nrm.y, nrm.z);
            m->AddClippingPlane(pl);
        }
    }
}

void PointCloudView::applyMatrix(NodeActor& na, const DatasetState& ds, const coordinates::RenderOrigin& origin) const {
    const Transform3 m = Transform3::translation(-origin.origin()) * ds.transform * Transform3::translation(na.points->centre);
    vtkNew<vtkMatrix4x4> vm;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) vm->SetElement(r, c, m(r, c));
    na.actor->SetUserMatrix(vm);
}

void PointCloudView::colorize(NodeActor& na, const DatasetState& ds) const {
    const auto& np = *na.points;
    const auto n = static_cast<vtkIdType>(np.count());
    vtkNew<vtkUnsignedCharArray> col;
    col->SetName("color");
    col->SetNumberOfComponents(3);
    col->SetNumberOfTuples(n);
    auto* out = col->GetPointer(0);
    auto put = [&](vtkIdType i, std::uint32_t c) {
        out[i * 3] = static_cast<unsigned char>((c >> 16) & 0xFF);
        out[i * 3 + 1] = static_cast<unsigned char>((c >> 8) & 0xFF);
        out[i * 3 + 2] = static_cast<unsigned char>(c & 0xFF);
    };
    const std::string& mode = ds.colorMode;
    const auto& T = ds.transform;
    if (mode == "RGB" && !np.rgb.empty()) {
        std::copy(np.rgb.begin(), np.rgb.end(), out);
    } else if (mode == "Intensity" && !np.intensity.empty()) {
        const double span = std::max(1.0, ds.iMax - ds.iMin);
        for (vtkIdType i = 0; i < n; ++i) {
            const double t = std::clamp((np.intensity[static_cast<std::size_t>(i)] - ds.iMin) / span, 0.0, 1.0);
            const auto g = static_cast<unsigned char>(std::lround(std::pow(t, 0.8) * 255));
            out[i * 3] = out[i * 3 + 1] = out[i * 3 + 2] = g;
        }
    } else if (mode == "Classification" && !np.classification.empty()) {
        for (vtkIdType i = 0; i < n; ++i) put(i, classificationColor(np.classification[static_cast<std::size_t>(i)]));
    } else if (mode.rfind("Scalar:", 0) == 0 && np.scalars.count(mode.substr(7))) {
        const auto& v = np.scalars.at(mode.substr(7));
        auto r = ds.scalarRange.count(mode.substr(7)) ? ds.scalarRange.at(mode.substr(7)) : std::make_pair(0.f, 1.f);
        const bool diverging = r.first < 0 && r.second > 0;
        if (diverging) {
            const float m = std::max(-r.first, r.second);
            r = {-m, m};
        }
        const double span = std::max(1e-12, static_cast<double>(r.second - r.first));
        for (vtkIdType i = 0; i < n; ++i) {
            const float s = v[static_cast<std::size_t>(i)];
            put(i, std::isfinite(s) ? lutColor(diverging ? Lut::Diverging : Lut::Rainbow, (s - r.first) / span) : 0x606060);
        }
    } else if (mode == "Single") {
        for (vtkIdType i = 0; i < n; ++i) put(i, ds.singleColor);
    } else { // Elevation (default, also the fallback when an attribute is missing)
        const double span = std::max(1e-9, ds.zMax - ds.zMin);
        const Vec3 c = np.centre;
        for (vtkIdType i = 0; i < n; ++i) {
            const Vec3 p{c.x + np.xyz[static_cast<std::size_t>(i * 3)], c.y + np.xyz[static_cast<std::size_t>(i * 3 + 1)],
                         c.z + np.xyz[static_cast<std::size_t>(i * 3 + 2)]};
            const double z = T(2, 0) * p.x + T(2, 1) * p.y + T(2, 2) * p.z + T(2, 3);
            put(i, lutColor(Lut::Viridis, (z - ds.zMin) / span));
        }
    }
    auto* pd = vtkPolyData::SafeDownCast(na.actor->GetMapper()->GetInput());
    pd->GetPointData()->SetScalars(col);
    pd->Modified();
    na.colorKey = mode;
}

void PointCloudView::recolor() {
    for (auto& [k, na] : actors_) {
        auto it = datasets_.find(na.datasetId);
        if (it != datasets_.end()) colorize(na, it->second);
    }
}

bool PointCloudView::update(const Project& project, const coordinates::RenderOrigin& origin, int vw, int vh) {
    bool changed = false;
    const bool originMoved = !(origin.origin() == origin_.origin());
    origin_ = origin;
    const auto& m = project.model();
    std::set<std::pair<Uuid, std::string>> wanted;
    std::vector<std::pair<double, std::pair<Uuid, std::string>>> missing;
    displayed_ = 0;

    auto* cam = renderer_->GetActiveCamera();
    double planes[24];
    const double aspect = vh > 0 ? static_cast<double>(vw) / vh : 1.0;
    cam->GetFrustumPlanes(aspect, planes);
    double camPos[3];
    cam->GetPosition(camPos);
    const Vec3 eye{camPos[0], camPos[1], camPos[2]};
    const bool parallel = cam->GetParallelProjection() != 0;
    // Pixels per metre at unit distance (perspective) or everywhere (parallel).
    const double pxPerUnit = parallel ? vh / (2.0 * std::max(1e-9, cam->GetParallelScale()))
                                      : vh / (2.0 * std::tan(cam->GetViewAngle() * std::numbers::pi / 360.0));

    std::set<Uuid> present;
    if (options_.enabled) {
        struct Cand {
            double priority;
            Uuid dataset;
            const survey::OctreeNodeInfo* node;
            std::shared_ptr<const survey::OctreeReader> reader;
            bool operator<(const Cand& o) const { return priority < o.priority; }
        };
        std::priority_queue<Cand> queue;
        for (const auto& d : m.pointClouds.all()) {
            if (!d.visible) continue;
            const auto* asset = m.assets.find(d.asset.id);
            if (!asset) continue;
            auto r = store_.reader(asset->id, project.assets().pathOf(*asset));
            if (!r) continue;
            present.insert(d.id);
            auto& ds = datasets_[d.id];
            const bool recolorNeeded = ds.colorMode != d.colorMode || ds.pointSize != d.pointSize || !(ds.transform == d.transform);
            const bool transformChanged = !(ds.transform == d.transform);
            ds.assetId = asset->id;
            ds.transform = d.transform;
            ds.colorMode = d.colorMode;
            ds.pointSize = d.pointSize;
            // Elevation range of the dataset in project coordinates.
            const auto& sb = d.storedBounds.isEmpty() ? r->info().bounds : d.storedBounds;
            double z0 = 1e300, z1 = -1e300;
            for (int i = 0; i < 8; ++i) {
                const Vec3 p = d.transform.apply({(i & 1) ? sb.max.x : sb.min.x, (i & 2) ? sb.max.y : sb.min.y, (i & 4) ? sb.max.z : sb.min.z});
                z0 = std::min(z0, p.z);
                z1 = std::max(z1, p.z);
            }
            ds.zMin = z0;
            ds.zMax = z1;
            // Intensity / scalar ranges from the root node (a uniform sample of the whole cloud).
            if (!ds.rangesFromRoot) {
                if (auto root = store_.node(asset->id, "r")) {
                    if (!root->intensity.empty()) {
                        auto v = root->intensity;
                        std::sort(v.begin(), v.end());
                        ds.iMin = v[v.size() * 2 / 100];
                        ds.iMax = v[std::min(v.size() - 1, v.size() * 98 / 100)];
                    }
                    for (const auto& [name, vals] : root->scalars) {
                        std::vector<float> f;
                        for (float x : vals)
                            if (std::isfinite(x)) f.push_back(x);
                        if (f.empty()) continue;
                        std::sort(f.begin(), f.end());
                        ds.scalarRange[name] = {f[f.size() / 100], f[std::min(f.size() - 1, f.size() * 99 / 100)]};
                    }
                    ds.rangesFromRoot = true;
                }
            }
            for (auto& [k, na] : actors_)
                if (na.datasetId == d.id) {
                    if (transformChanged || originMoved) applyMatrix(na, ds, origin);
                    if (recolorNeeded || na.colorKey != ds.colorMode) colorize(na, ds);
                    na.actor->GetProperty()->SetPointSize(static_cast<float>(ds.pointSize));
                }
            if (recolorNeeded) changed = true;
            if (const auto* root = r->node("r")) queue.push({1e30, d.id, root, r});
        }

        std::size_t budget = options_.pointBudget;
        while (!queue.empty()) {
            const Cand c = queue.top();
            queue.pop();
            const auto& ds = datasets_[c.dataset];
            // Node cube in render-local coordinates.
            auto lb = BoundingBox::empty();
            const auto& cube = c.node->cube;
            for (int i = 0; i < 8; ++i)
                lb.expand(origin.toLocal(ds.transform.apply(
                    {(i & 1) ? cube.max.x : cube.min.x, (i & 2) ? cube.max.y : cube.min.y, (i & 4) ? cube.max.z : cube.min.z})));
            // Frustum test (normals point inward): reject when the box is fully outside a plane.
            bool outside = false;
            for (int p = 0; p < 6 && !outside; ++p) {
                const double* pl = planes + p * 4;
                const Vec3 v{pl[0] >= 0 ? lb.max.x : lb.min.x, pl[1] >= 0 ? lb.max.y : lb.min.y, pl[2] >= 0 ? lb.max.z : lb.min.z};
                if (pl[0] * v.x + pl[1] * v.y + pl[2] * v.z + pl[3] < 0) outside = true;
            }
            if (outside) continue;
            if (static_cast<std::size_t>(c.node->count) > budget && c.node->level > 0) continue;
            budget -= std::min<std::size_t>(budget, static_cast<std::size_t>(c.node->count));
            const auto key = std::make_pair(c.dataset, c.node->name);
            wanted.insert(key);
            if (!store_.node(ds.assetId, c.node->name)) missing.push_back({c.priority, {ds.assetId, c.node->name}});
            // Descend when this node's sampling is coarser than the target screen spacing.
            const Vec3 nearest = componentMax(lb.min, componentMin(eye, lb.max));
            const double dist = parallel ? 1.0 : std::max(1e-3, distance(nearest, eye));
            const double scale = ds.transform.uniformScale();
            const double px = c.node->spacing * scale * pxPerUnit / dist;
            if (px > options_.screenSpacing * std::max(1.0, ds.pointSize))
                for (const auto* ch : c.reader->children(c.node->name)) {
                    const double chPx = ch->spacing * scale * pxPerUnit / dist;
                    queue.push({chPx, c.dataset, ch, c.reader});
                }
        }
    }

    // Remove actors no longer wanted (or of datasets gone/hidden).
    for (auto it = actors_.begin(); it != actors_.end();) {
        if (!wanted.count(it->first) || !present.count(it->second.datasetId)) {
            renderer_->RemoveActor(it->second.actor);
            it = actors_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    for (auto it = datasets_.begin(); it != datasets_.end();)
        it = present.count(it->first) ? std::next(it) : datasets_.erase(it);
    // Add loaded nodes.
    for (const auto& key : wanted) {
        if (actors_.count(key)) {
            displayed_ += actors_[key].points->count();
            continue;
        }
        const auto& ds = datasets_[key.first];
        auto np = store_.node(ds.assetId, key.second);
        if (!np) continue;
        NodeActor na;
        na.points = np;
        na.datasetId = key.first;
        na.actor = makeActor(*np);
        na.actor->GetProperty()->SetPointSize(static_cast<float>(ds.pointSize));
        applyMatrix(na, ds, origin);
        colorize(na, ds);
        renderer_->AddActor(na.actor);
        displayed_ += np->count();
        actors_[key] = std::move(na);
        changed = true;
    }
    std::sort(missing.begin(), missing.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::pair<Uuid, std::string>> req;
    req.reserve(missing.size());
    for (auto& m2 : missing) req.push_back(std::move(m2.second));
    pending_ = req.size();
    store_.request(requester_, std::move(req));
    return changed;
}

std::optional<PointCloudView::Pick> PointCloudView::pick(double dx, double dy, double tolPx) const {
    if (actors_.empty()) return std::nullopt;
    auto* r = renderer_;
    double nearP[4], farP[4];
    r->SetDisplayPoint(dx, dy, 0.0);
    r->DisplayToWorld();
    r->GetWorldPoint(nearP);
    r->SetDisplayPoint(dx, dy, 1.0);
    r->DisplayToWorld();
    r->GetWorldPoint(farP);
    if (nearP[3] == 0 || farP[3] == 0) return std::nullopt;
    const Vec3 o{nearP[0] / nearP[3], nearP[1] / nearP[3], nearP[2] / nearP[3]};
    const Vec3 f{farP[0] / farP[3], farP[1] / farP[3], farP[2] / farP[3]};
    const Vec3 dir = (f - o).normalized();
    auto* cam = r->GetActiveCamera();
    const int* size = r->GetSize();
    const double h = std::max(1, size[1]);
    const bool parallel = cam->GetParallelProjection() != 0;
    double camPos[3];
    cam->GetPosition(camPos);
    const Vec3 eye{camPos[0], camPos[1], camPos[2]};
    auto worldPerPixel = [&](double depthFromEye) {
        return parallel ? 2.0 * cam->GetParallelScale() / h : 2.0 * std::max(depthFromEye, 1e-6) * std::tan(cam->GetViewAngle() * std::numbers::pi / 360.0) / h;
    };
    std::optional<Pick> best;
    for (const auto& [key, na] : actors_) {
        auto dsIt = datasets_.find(na.datasetId);
        if (dsIt == datasets_.end()) continue;
        const auto& ds = dsIt->second;
        const Transform3 M = Transform3::translation(-origin_.origin()) * ds.transform * Transform3::translation(na.points->centre);
        const auto& np = *na.points;
        const std::size_t n = np.count();
        for (std::size_t i = 0; i < n; ++i) {
            const Vec3 pl = M.apply({np.xyz[i * 3], np.xyz[i * 3 + 1], np.xyz[i * 3 + 2]});
            const Vec3 v = pl - o;
            const double t = dot(v, dir);
            if (t < 0) continue;
            const double d2 = (v - dir * t).lengthSquared();
            const double tol = tolPx * worldPerPixel(distance(pl, eye));
            if (d2 > tol * tol) continue;
            if (best && t >= best->depth) continue;
            Pick p;
            p.datasetId = na.datasetId;
            p.stored = na.points->centre + Vec3{np.xyz[i * 3], np.xyz[i * 3 + 1], np.xyz[i * 3 + 2]};
            p.project = ds.transform.apply(p.stored);
            p.depth = t;
            best = p;
        }
    }
    return best;
}

std::optional<ScalarDisplay> PointCloudView::legend() const {
    for (const auto& [id, ds] : datasets_) {
        ScalarDisplay sd;
        if (ds.colorMode == "Elevation" || ds.colorMode.empty()) {
            sd.title = "Elevation (m)";
            sd.min = ds.zMin;
            sd.max = ds.zMax;
            sd.lut = Lut::Viridis;
            return sd;
        }
        if (ds.colorMode.rfind("Scalar:", 0) == 0) {
            const auto name = ds.colorMode.substr(7);
            if (auto it = ds.scalarRange.find(name); it != ds.scalarRange.end()) {
                sd.title = name;
                sd.min = it->second.first;
                sd.max = it->second.second;
                sd.lut = Lut::Rainbow;
                if (sd.min < 0 && sd.max > 0) {
                    const double r = std::max(-sd.min, sd.max);
                    sd.min = -r;
                    sd.max = r;
                    sd.lut = Lut::Diverging;
                }
                return sd;
            }
        }
    }
    return std::nullopt;
}

} // namespace openmine::app
