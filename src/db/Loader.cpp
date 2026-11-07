// db/Loader.cpp -- dlopen the engine plugins.
#include "db/Loader.hpp"
#include "db/Plugin.hpp"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>

namespace fs = std::filesystem;

namespace acorn::db {

struct Loader::Loaded {
    void*                   handle = nullptr;
    const AcornEngineEntry* entry = nullptr;
    Engine*                 engine = nullptr;
    fs::path                file;
    ~Loaded() {
        if (engine && entry) entry->destroy(engine);
        if (handle) dlclose(handle);
    }
};

Loader::Loader() = default;

Loader::~Loader() {
    // Destroy in reverse load order.
    while (!loaded_.empty()) loaded_.pop_back();
}

std::vector<fs::path> Loader::default_dirs() {
    std::vector<fs::path> dirs;
    if (const char* env = std::getenv("ACORN_ENGINE_PATH")) {
        std::string s = env;
        std::size_t start = 0;
        while (start <= s.size()) {
            auto end = s.find(':', start);
            if (end == std::string::npos) end = s.size();
            if (end > start) dirs.emplace_back(s.substr(start, end - start));
            start = end + 1;
        }
    }
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        fs::path dir = exe.parent_path();
        dirs.push_back(dir / "engines");
        dirs.push_back((dir / ".." / "lib" / "acorn" / "engines").lexically_normal());
    }
    return dirs;
}

int Loader::scan(const std::vector<fs::path>& dirs) {
    int n = 0;
    for (auto& dir : dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> files;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            auto name = e.path().filename().string();
            if (name.rfind("acorn-engine-", 0) == 0 && e.path().extension() == ".so")
                files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());   // deterministic order

        for (auto& f : files) {
            void* h = dlopen(f.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!h) {
                const char* why = dlerror();
                rejected_.push_back({f, why ? why : "dlopen failed"});
                continue;
            }
            auto fn = reinterpret_cast<AcornEngineEntryFn>(dlsym(h, ACORN_ENGINE_SYMBOL));
            const AcornEngineEntry* entry = fn ? fn() : nullptr;
            if (!entry) {
                rejected_.push_back({f, "no " ACORN_ENGINE_SYMBOL " entry point"});
                dlclose(h);
                continue;
            }
            if (entry->abi != ACORN_ENGINE_ABI) {
                rejected_.push_back({f, "engine ABI " + std::to_string(entry->abi) +
                                        ", Acorn expects " + std::to_string(ACORN_ENGINE_ABI)});
                dlclose(h);
                continue;
            }
            if (find(entry->id)) {
                rejected_.push_back({f, std::string("duplicate engine id '") + entry->id +
                                        "' (an earlier directory won)"});
                dlclose(h);
                continue;
            }
            auto L = std::make_unique<Loaded>();
            L->handle = h;
            L->entry = entry;
            L->file = f;
            L->engine = entry->create();
            if (!L->engine) {
                rejected_.push_back({f, "create() returned null"});
                continue;   // ~Loaded closes the handle
            }
            loaded_.push_back(std::move(L));
            ++n;
        }
    }
    return n;
}

std::vector<Engine*> Loader::engines() const {
    std::vector<Engine*> v;
    for (auto& l : loaded_) v.push_back(l->engine);
    return v;
}

Engine* Loader::find(const std::string& id) const {
    for (auto& l : loaded_) if (l->entry->id == id) return l->engine;
    return nullptr;
}

} // namespace acorn::db
