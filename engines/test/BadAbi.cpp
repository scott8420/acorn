// acorn-engine-badabi -- a TEST-ONLY plugin that claims a foreign ABI, so the
// selftest can prove the loader refuses a stale .so. Never installed; built
// into build/engines-test/, not build/engines/.
#include "db/Plugin.hpp"

namespace {
const AcornEngineEntry kEntry{ACORN_ENGINE_ABI + 998u, "badabi", nullptr, nullptr};
}

ACORN_EXPORT const AcornEngineEntry* acorn_engine_entry(void) { return &kEntry; }
