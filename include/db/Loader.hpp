// db/Loader.hpp -- finds and loads the engine plugins.
//
// Search order (first id wins; later duplicates are reported, not loaded):
//   1. $ACORN_ENGINE_PATH (colon-separated) -- tests, development
//   2. <exe dir>/engines                     -- the build tree
//   3. <exe dir>/../lib/acorn/engines        -- installed (install.sh)
// A plugin whose client library is missing simply fails dlopen and is listed
// in rejected() with dlerror's reason; the app runs without that engine.
//
// LIFETIME: the Loader owns the dlopen handles and the Engine objects. Every
// Connection must be destroyed before the Loader -- its code lives in the .so.
#pragma once

#include "db/Engine.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace acorn::db {

class Loader {
public:
    struct Rejected { std::filesystem::path file; std::string reason; };

    Loader();
    ~Loader();
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    // The default search dirs above, in order.
    static std::vector<std::filesystem::path> default_dirs();

    // Scan dirs for acorn-engine-*.so; returns how many engines loaded.
    int scan(const std::vector<std::filesystem::path>& dirs);

    std::vector<Engine*> engines() const;            // in load order
    Engine* find(const std::string& id) const;       // nullptr if absent
    const std::vector<Rejected>& rejected() const { return rejected_; }

private:
    struct Loaded;
    std::vector<std::unique_ptr<Loaded>> loaded_;
    std::vector<Rejected> rejected_;
};

} // namespace acorn::db
