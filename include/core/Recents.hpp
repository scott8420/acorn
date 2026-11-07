#pragma once
#include <cstddef>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// core/Recents -- a most-recently-used list + its JSON persistence pump.
// (Cairn: ported from Optik's util::Recents. The pump discipline it teaches is
// Folio's Format lesson too: the core owns its serializable shape and the UI
// never leaks in -- see docs/two-layer-seam.md.)
//
// GTK-free, headless-testable: pure list ops + encode/decode IN ONE FILE so a
// write can't skew from its read (CANON: pumps at conceptual seams). The CALLER
// resolves the on-disk path and hands it in -- the UI does the XDG resolution
// and passes a plain string, so the core stays free of GTK (data in the core,
// dir resolution + behaviour in the UI).
//
// On disk: { "paths": [ "/abs/a", "/abs/b", ... ] }, first == most recent. load
// is first-run tolerant, prunes entries whose path no longer exists, dedupes.
// Round-trip fidelity is the bar (CANON): what you save is what you load.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn::core {

inline constexpr std::size_t kMaxRecents = 8;

// Pure list ops (headless-testable). add: move-to-front + trim to `max`.
void recents_add(std::vector<std::string>& list, const std::string& path,
                 std::size_t max = kMaxRecents);
void recents_remove(std::vector<std::string>& list, const std::string& path);

// Persistence pump -- encode + decode adjacent so they can't drift.
std::vector<std::string> load_recents(const std::string& file);
bool                     save_recents(const std::string& file,
                                      const std::vector<std::string>& list);

}  // namespace acorn::core
