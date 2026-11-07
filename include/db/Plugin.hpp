// db/Plugin.hpp -- the one C entry point an engine plugin exports.
//
// Each engine is a shared object, acorn-engine-<id>.so, exporting
//     extern "C" const AcornEngineEntry* acorn_engine_entry(void);
// The loader dlopens it, reads the entry, and refuses it unless `abi` matches
// ACORN_ENGINE_ABI. Plugin and app are built by one compiler from these same
// headers, so the C++ Engine behind `create` is safe to use directly; the C
// symbol and the ABI number are what guard against a stale .so left in a
// directory. Bump ACORN_ENGINE_ABI whenever Engine.hpp, Value.hpp or
// Schema.hpp changes shape.
#pragma once

#include <cstdint>

namespace acorn::db { class Engine; }

#define ACORN_ENGINE_ABI 2u   // 2: Capabilities::partial_indexes (s003)
#define ACORN_ENGINE_SYMBOL "acorn_engine_entry"

extern "C" {
struct AcornEngineEntry {
    std::uint32_t abi;
    const char*   id;                           // "sqlite"
    acorn::db::Engine* (*create)();
    void (*destroy)(acorn::db::Engine*);
};
typedef const AcornEngineEntry* (*AcornEngineEntryFn)(void);
}

#define ACORN_EXPORT extern "C" __attribute__((visibility("default")))
