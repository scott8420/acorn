#include "db/NewFile.hpp"

#include <system_error>

namespace acorn::db {

std::expected<void, Error> new_file(Engine& engine, const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    if (engine.caps().needs_server)
        return std::unexpected(Error{Error::Code::Unsupported, engine.name() + " databases live on a server, not in a file"});
    if (path.empty())
        return std::unexpected(Error{Error::Code::NotFound, "no file name"});
    std::error_code ec;
    if (fs::exists(fs::symlink_status(path, ec)) || fs::is_symlink(fs::symlink_status(path, ec)))
        return std::unexpected(Error{Error::Code::Exists, path.string() + " already exists -- Acorn will not overwrite it"});
    if (!path.parent_path().empty() && !fs::is_directory(path.parent_path(), ec))
        return std::unexpected(Error{Error::Code::NotFound, "no such folder: " + path.parent_path().string()});

    const auto drop = [&] { std::error_code e; fs::remove(path, e); };
    ConnectParams p;
    p.path = path.string();
    p.create = true;
    p.read_only = false;
    auto c = engine.open(p);
    if (!c) { drop(); return std::unexpected(c.error()); }
    if (engine.dialect() == Dialect::SQLite) {
        auto r = (*c)->query("PRAGMA application_id = " + std::to_string(kAcornApplicationId));
        if (!r) { c->reset(); drop(); return std::unexpected(r.error()); }
    }
    c->reset();   // closed; the caller opens it the normal way
    if (!fs::exists(path, ec)) return std::unexpected(Error{Error::Code::Internal, "the file was not created"});
    return {};
}

} // namespace acorn::db
