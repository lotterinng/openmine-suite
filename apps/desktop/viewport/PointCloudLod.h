#pragma once
// SPDX-License-Identifier: Apache-2.0
// Out-of-core point-cloud display. Datasets are octrees in the asset store (modules/survey,
// docs/formats/octree.md); each viewport selects, per frame, the nodes whose sampling spacing is
// still visible at the current zoom (screen-space error), inside the view frustum, up to a point
// budget. Nodes are read on background threads into a shared, memory-bounded LRU cache, so
// clouds far larger than memory can be navigated; until a node arrives its parent keeps
// standing in for it.
//
// Precision: node points are kept relative to their node centre (float), and each node actor's
// matrix is render-origin⁻¹ · dataset transform · node-centre translation, composed in double
// precision — so a registration (which changes only the dataset transform) moves the cloud
// without re-reading or re-uploading points.

#include "SceneTypes.h"

#include "openmine/survey/Octree.h"

#include <QObject>

#include <condition_variable>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

class vtkActor;
class vtkCamera;
class vtkRenderer;

namespace openmine::app {

/// Points of one octree node, prepared for display.
struct NodePoints {
    Vec3 centre;                         ///< stored coordinates of the node-cube centre
    std::vector<float> xyz;              ///< 3 per point, relative to `centre`
    std::vector<std::uint16_t> intensity;
    std::vector<std::uint8_t> rgb;       ///< 3 per point (8-bit)
    std::vector<std::uint8_t> classification;
    std::map<std::string, std::vector<float>> scalars;
    std::size_t count() const { return xyz.size() / 3; }
    std::size_t bytes() const;
};

class PointCloudStore : public QObject {
    Q_OBJECT
public:
    explicit PointCloudStore(QObject* parent = nullptr);
    ~PointCloudStore() override;

    /// The octree of a dataset asset (opened on first use; nullptr when unreadable).
    std::shared_ptr<const survey::OctreeReader> reader(const Uuid& assetId, const std::filesystem::path& folder);
    /// Cached node or nullptr; marks it recently used.
    std::shared_ptr<const NodePoints> node(const Uuid& assetId, const std::string& name);
    /// Replaces the queue of node requests of one requester (a viewport) — highest priority first.
    void request(int requester, std::vector<std::pair<Uuid, std::string>> nodesByPriority);
    void cancelRequests(int requester);
    /// Drops everything (project closed).
    void clear();
    void setMemoryBudget(std::size_t bytes) { budget_ = bytes; }
    std::size_t memoryUsed() const;
    std::size_t queued() const;

signals:
    /// A requested node was loaded (emitted on the UI thread, coalesced).
    void nodesLoaded();
    void loadFailed(QString message);

private:
    struct Key {
        Uuid asset;
        std::string node;
        friend bool operator<(const Key& a, const Key& b) { return a.asset != b.asset ? a.asset < b.asset : a.node < b.node; }
        friend bool operator==(const Key&, const Key&) = default;
    };
    void loaderLoop();
    void evict();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<Uuid, std::shared_ptr<const survey::OctreeReader>> readers_;
    std::map<Uuid, std::string> readerErrors_;
    std::map<Key, std::pair<std::shared_ptr<const NodePoints>, std::list<Key>::iterator>> cache_;
    std::list<Key> lru_; ///< front = most recent
    std::map<int, std::vector<Key>> requests_;
    std::set<Key> inFlight_;
    std::size_t used_ = 0;
    std::size_t budget_ = std::size_t(1536) << 20;
    std::vector<std::thread> threads_;
    bool stop_ = false;
    bool notifyPending_ = false;
};

/// The point clouds shown in one viewport.
class PointCloudView {
public:
    PointCloudView(PointCloudStore& store, vtkRenderer* renderer, int requesterId);
    ~PointCloudView();

    struct Options {
        std::size_t pointBudget = 3'000'000;
        double screenSpacing = 1.5;      ///< descend while the node spacing covers more pixels than this × point size
        bool enabled = true;
    };
    Options& options() { return options_; }

    /// Selects nodes for the current camera, updates actors and requests missing nodes. Returns
    /// true when the displayed set changed (a render is needed).
    bool update(const Project& project, const coordinates::RenderOrigin& origin, int viewportWidth, int viewportHeight);
    /// Re-colours displayed nodes (colour mode or range changed).
    void recolor();
    void clear();

    /// Number of points currently displayed and still wanted from the store.
    std::size_t displayedPoints() const { return displayed_; }
    std::size_t pendingNodes() const { return pending_; }

    struct Pick {
        Uuid datasetId;
        Vec3 project;   ///< project coordinates
        Vec3 stored;    ///< the dataset's stored coordinates
        double depth = 0;
    };
    /// Nearest displayed point to the pixel (within `tolerancePx`), closest to the camera.
    std::optional<Pick> pick(double displayX, double displayY, double tolerancePx) const;

    /// The scalar legend of the first displayed dataset coloured by a ramp, if any.
    std::optional<ScalarDisplay> legend() const;

    /// Clip planes applied to node mappers (section views).
    void setClipPlanes(const std::vector<std::pair<Vec3, Vec3>>& localPlanes);

private:
    struct DatasetState {
        Uuid assetId;
        Transform3 transform;
        std::string colorMode;
        double pointSize = 2;
        double zMin = 0, zMax = 1;       ///< project elevation range for Elevation colouring
        double iMin = 0, iMax = 65535;   ///< intensity stretch
        std::map<std::string, std::pair<float, float>> scalarRange;
        std::uint32_t singleColor = 0xC8C8C8;
        bool rangesFromRoot = false;
    };
    struct NodeActor {
        vtkSmartPointer<vtkActor> actor;
        std::shared_ptr<const NodePoints> points;
        Uuid datasetId;
        std::string colorKey;
    };
    void colorize(NodeActor& na, const DatasetState& ds) const;
    void applyMatrix(NodeActor& na, const DatasetState& ds, const coordinates::RenderOrigin& origin) const;
    vtkSmartPointer<vtkActor> makeActor(const NodePoints& np) const;

    PointCloudStore& store_;
    vtkRenderer* renderer_;
    int requester_;
    Options options_;
    std::map<Uuid, DatasetState> datasets_;
    std::map<std::pair<Uuid, std::string>, NodeActor> actors_;
    std::vector<std::pair<Vec3, Vec3>> clipPlanes_;
    coordinates::RenderOrigin origin_;
    std::size_t displayed_ = 0, pending_ = 0;
};

} // namespace openmine::app
