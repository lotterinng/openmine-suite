// SPDX-License-Identifier: Apache-2.0
#include "openmine/assets/AssetStore.h"
#include "openmine/diagnostics/Log.h"
#include "openmine/jobs/Job.h"
#include "openmine/util/Sha256.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

using namespace openmine;
namespace fs = std::filesystem;

TEST(Sha256, Fips180Vectors) {
    EXPECT_EQ(util::sha256Hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(util::sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(util::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    util::Sha256 h;
    const std::string a(1000, 'a');
    for (int i = 0; i < 1000; ++i) h.update(a);
    EXPECT_EQ(h.hexDigest(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"); // 10^6 × 'a'
}

TEST(Assets, FileAndFolderAssetsVerifyAndRelocate) {
    const auto root = fs::temp_directory_path() / ("om-assets-" + Uuid::generate().toString());
    AssetStore store(root / "a");
    const Asset f = store.writeBytes("hello mine", AssetKind::Other, "txt");
    EXPECT_EQ(f.sha256, util::sha256Hex("hello mine"));
    EXPECT_TRUE(store.verify(f, true).ok);
    EXPECT_EQ(store.readBytes(f), "hello mine");

    auto [id, dir] = store.beginFolder(AssetKind::PointCloud);
    std::ofstream(dir / "r.bin") << "node-data";
    fs::create_directories(dir / "sub");
    std::ofstream(dir / "sub" / "r0.bin") << "more";
    const Asset folder = store.finalizeFolder(id, AssetKind::PointCloud);
    EXPECT_TRUE(folder.isDirectory);
    EXPECT_EQ(folder.sizeBytes, 13);
    EXPECT_TRUE(store.verify(folder, true).ok);

    store.relocate(root / "b", {f, folder});
    EXPECT_EQ(store.root(), root / "b");
    EXPECT_TRUE(store.verify(f, true).ok);
    EXPECT_TRUE(store.verify(folder, true).ok);

    // Tampering is detected.
    std::ofstream(store.pathOf(folder) / "r.bin") << "NODE-DATA"; // same size, different content
    EXPECT_TRUE(store.verify(folder, false).ok);
    EXPECT_FALSE(store.verify(folder, true).ok);
    fs::remove(store.pathOf(f));
    EXPECT_FALSE(store.verify(f, false).ok);
    fs::remove_all(root);
}

TEST(Jobs, ProgressCancellationAndErrors) {
    jobs::JobRunner runner(2);
    std::atomic<bool> started{false};
    auto j = runner.submit("long", [&](jobs::Progress& p) {
        started = true;
        for (int i = 0; i < 1000; ++i) {
            p.update(i / 1000.0, "working");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    while (!started) std::this_thread::yield();
    j->progress->cancel();
    j->done.wait();
    EXPECT_EQ(j->state.load(), jobs::JobState::Cancelled);

    auto e = runner.submit("fails", [](jobs::Progress&) { throw std::runtime_error("boom"); });
    e->done.wait();
    EXPECT_EQ(e->state.load(), jobs::JobState::Failed);
    EXPECT_EQ(e->error, "boom");

    auto ok = runner.submit("ok", [](jobs::Progress& p) { p.update(1.0, "done"); });
    ok->done.wait();
    EXPECT_EQ(ok->state.load(), jobs::JobState::Succeeded);
    EXPECT_DOUBLE_EQ(ok->progress->fraction(), 1.0);
}

TEST(Jobs, ParallelForCoversRangeOnce) {
    std::vector<int> hits(100000, 0);
    jobs::parallelFor(hits.size(), [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) ++hits[i];
    });
    for (int h : hits) ASSERT_EQ(h, 1);
}

TEST(Logging, SinksReceiveRecords) {
    std::vector<std::string> got;
    auto& lg = diag::Logger::instance();
    const int t = lg.addSink([&](const diag::Record& r) { got.push_back(r.category + ":" + r.message); });
    diag::info("test", "hello");
    diag::debug("test", "hidden at info level");
    lg.removeSink(t);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], "test:hello");
}
