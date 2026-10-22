#pragma once
// SPDX-License-Identifier: Apache-2.0
// Scratch directories for tests.

#include "openmine/objects/Uuid.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace openmine::test {

/// A new directory under the system's temporary directory, removed with its contents when the
/// object goes out of scope. Declare it before the objects that open files in it: those are
/// destroyed first, so their files are closed by the time it is removed (Windows cannot delete a
/// file that is still open). A file that is still open then means a handle was leaked, and the
/// test fails.
class TempDir {
public:
    explicit TempDir(const std::string& prefix)
        : path_(std::filesystem::temp_directory_path() / (prefix + Uuid::generate().toString())) {
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        if (ec) ADD_FAILURE() << "Cannot remove the test directory " << path_.string() << ": " << ec.message();
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    operator const std::filesystem::path&() const { return path_; }
    std::filesystem::path operator/(const std::filesystem::path& p) const { return path_ / p; }
    std::string string() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

} // namespace openmine::test
