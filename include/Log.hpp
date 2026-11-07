#pragma once
#include <spdlog/spdlog.h>
#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
// Log -- per-area logging, runtime-toggleable (CANON: "per-area logging areas
// with runtime toggle"). Every concern gets a named spdlog logger on one shared
// sink; turn the suspect area to TRACE, reproduce, read the trace -- the
// substrate for "two failures = drop to log mode".
//
// Areas are named by CONCERN, not by file. A method that can't decide which
// area it belongs to is probably doing too much -- the discipline pressures
// single-concern functions.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn::log {

enum class Area { App, Shell, Registry, Recents, Io };

// The logger for an area (may be null before init()/after shutdown -- callers
// guard with `if (auto lg = log::get(area))`).
std::shared_ptr<spdlog::logger> get(Area area);

// Stand up all area loggers on a shared stdout sink. Call once at startup.
void init();

// Flip one area's level at runtime (the debug-menu affordance in a real app).
void set_level(Area area, spdlog::level::level_enum level);

}  // namespace acorn::log
