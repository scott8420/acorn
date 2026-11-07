cmake_minimum_required(VERSION 3.20)
project(acorn VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED gtkmm-4.0)
find_package(spdlog REQUIRED)
# s004b: server passwords live in the GNOME keyring (Fedora: libsecret-devel).
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

# ── acorn_db -- the database layer's shared code (s002) ────────────────────────
# Value / Result / Schema helpers and the type map. Linked by acorn_core AND by
# every engine plugin, so it is position-independent and GTK-free. The plugins
# never see acorn_core -- they implement db::Engine and nothing else.
add_library(acorn_db STATIC
    src/db/Value.cpp
    src/db/Types.cpp
)
target_include_directories(acorn_db PUBLIC include)
set_target_properties(acorn_db PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ── Engine plugins -- one .so per engine, dlopen'd at runtime ──────────────────
# Output to build/engines/ (the Loader's build-tree dir); install.sh copies them
# to <prefix>/lib/acorn/engines/. An engine whose client library is missing at
# build time is skipped with a message -- the app still builds and runs.
set(ACORN_ENGINE_DIR "${CMAKE_CURRENT_BINARY_DIR}/engines")
set(ACORN_ENGINES "")
function(acorn_engine id)
    add_library(acorn-engine-${id} MODULE ${ARGN})
    set_target_properties(acorn-engine-${id} PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${ACORN_ENGINE_DIR}"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    target_link_libraries(acorn-engine-${id} PRIVATE acorn_db)
    set(ACORN_ENGINES ${ACORN_ENGINES} acorn-engine-${id} PARENT_SCOPE)
endfunction()

find_package(SQLite3)
if(SQLite3_FOUND)
    acorn_engine(sqlite engines/sqlite/SqliteEngine.cpp)
    # CMake 4.x names the target SQLite3::SQLite3 and deprecates the old
    # SQLite::SQLite3; older CMake (Ubuntu 24.04's 3.28) has only the old name.
    if(TARGET SQLite3::SQLite3)
        target_link_libraries(acorn-engine-sqlite PRIVATE SQLite3::SQLite3)
    else()
        target_link_libraries(acorn-engine-sqlite PRIVATE SQLite::SQLite3)
    endif()
else()
    message(STATUS "SQLite3 dev files not found -- the SQLite engine will not be built "
                   "(Fedora: sqlite-devel)")
endif()

pkg_check_modules(MARIADB IMPORTED_TARGET libmariadb)
if(MARIADB_FOUND)
    acorn_engine(mariadb engines/mariadb/MariadbEngine.cpp)
    target_link_libraries(acorn-engine-mariadb PRIVATE PkgConfig::MARIADB)
else()
    message(STATUS "libmariadb not found -- the MariaDB engine will not be built "
                   "(Fedora: mariadb-connector-c-devel)")
endif()

pkg_check_modules(LIBPQ IMPORTED_TARGET libpq)
if(LIBPQ_FOUND)
    acorn_engine(postgres engines/postgres/PostgresEngine.cpp)
    target_link_libraries(acorn-engine-postgres PRIVATE PkgConfig::LIBPQ)
else()
    message(STATUS "libpq not found -- the PostgreSQL engine will not be built "
                   "(Fedora: libpq-devel)")
endif()

# Test-only plugin: a foreign ABI number, so the selftest proves the loader
# refuses a stale .so. Lives in engines-test/, never installed.
add_library(acorn-engine-badabi MODULE engines/test/BadAbi.cpp)
set_target_properties(acorn-engine-badabi PROPERTIES
    PREFIX "" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/engines-test")
target_include_directories(acorn-engine-badabi PRIVATE include)

# ── acorn_core -- the GTK-free core (the model side of the one real seam) ──────
# No gtkmm on this library's link line, BY DESIGN: a stray UI include under
# core/ fails to compile, so the seam is enforced by the build graph and the
# core stays headless-testable. (docs/two-layer-seam.md.)
add_library(acorn_core STATIC
    # s004b: recent connections (no passwords) + their JSON pump.
    src/core/Connections.cpp
    src/core/TextMap.cpp
    # The SHORTCUT-REGISTRY paradigm: one list, two consumers (App wires accels
    # from it, ShortcutsDialog renders from it) so a key and its advertised row
    # can't drift. GTK-free, so it lives in core and the selftest guards it
    # headless -- a pure test proves no two chords collide.
    src/core/Shortcuts.cpp
    # The UNDO paradigm (s046): core/Undo.hpp is the journal (header-only, a
    # template over the app's store); Stones is the demo model + its verbs +
    # the verb table the selftest gate walks. docs/undo.md.
    src/core/Stones.cpp
    # The engine plugin loader (dlopen). The app's side of the plugin seam.
    src/db/Loader.cpp
    # s005a: the one place Acorn creates a database file (never overwrites).
    src/db/NewFile.cpp
)
target_include_directories(acorn_core PUBLIC include)
target_link_libraries(acorn_core PUBLIC acorn_db ${CMAKE_DL_LIBS})
# Vendored nlohmann/json (header-only, MIT), PRIVATE -- an implementation detail
# of the pump, no consumer inherits the include (CANON: surprise-free build).
target_include_directories(acorn_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ── acorn_keyring -- passwords via libsecret (s004b) ─────────────────────────
# glib + libsecret, GTK-free, so the selftest can drive it headless too.
add_library(acorn_keyring STATIC src/Keyring.cpp)
target_include_directories(acorn_keyring PUBLIC include)
target_link_libraries(acorn_keyring PUBLIC PkgConfig::LIBSECRET)

# ── Headless selftest -- exercises the core with no display ────────────────────
add_executable(acorn_selftest src/selftest.cpp src/selftest_db.cpp)
target_link_libraries(acorn_selftest PRIVATE acorn_core acorn_keyring)
# The selftest loads the real plugins from build/engines/ and the test plugin
# from build/engines-test/, so building it builds them.
if(ACORN_ENGINES)
    add_dependencies(acorn_selftest ${ACORN_ENGINES})
endif()
add_dependencies(acorn_selftest acorn-engine-badabi)
target_compile_definitions(acorn_selftest PRIVATE
    ACORN_TEST_ENGINE_DIR="${ACORN_ENGINE_DIR}"
    ACORN_TEST_BADABI_DIR="${CMAKE_CURRENT_BINARY_DIR}/engines-test")

# ── App resources (gresource) -- the RESOURCE-PIPELINE paradigm ────────────────
# glib-compile-resources embeds resources/acorn.gresource.xml into a C source;
# App::on_activate registers it and points the icon theme at the bundle so
# set_icon_name("acorn-logo-symbolic") resolves out of the binary. The generated
# .c compiles as C (project enables C above) so acorn_resources_get_resource()
# has C linkage, matching the extern "C" prototype in App.cpp. Adding an icon:
# drop the .svg in resources/icons/scalable/apps/, list it in the .xml, rebuild.
# (docs/resources-and-dialog-lifetime.md)
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(ACORN_GRESOURCE_XML "${CMAKE_CURRENT_SOURCE_DIR}/resources/acorn.gresource.xml")
set(ACORN_RESOURCES_C   "${CMAKE_CURRENT_BINARY_DIR}/acorn-resources.c")
file(GLOB ACORN_RESOURCE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/scalable/apps/*.svg"
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/*.css")
add_custom_command(
    OUTPUT  "${ACORN_RESOURCES_C}"
    COMMAND "${GLIB_COMPILE_RESOURCES}"
            --target "${ACORN_RESOURCES_C}"
            --generate-source
            --c-name acorn_resources
            acorn.gresource.xml
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    DEPENDS "${ACORN_GRESOURCE_XML}" ${ACORN_RESOURCE_FILES}
    COMMENT "Compiling gresource bundle (acorn-resources.c)"
    VERBATIM)

# ── acorn -- the GTK app ───────────────────────────────────────────────────────
# The Shell is a SPLIT CLASS across TUs by category (see the index in
# include/Shell.hpp); all parts compile into the one acorn::Shell.
add_executable(acorn
    ${ACORN_RESOURCES_C}
    src/main.cpp
    src/App.cpp
    # Follows the desktop's light/dark preference (GTK4 will not do it for us
    # and we do not link libadwaita). Needs no new dependency: the D-Bus and
    # GSettings calls are GIO, which gtkmm-4.0 already brings in.
    src/Appearance.cpp
    src/Shell.cpp
    src/Shell_zones.cpp
    src/Shell_bindings.cpp
    src/Shell_handlers.cpp
    src/Shell_helpers.cpp
    # s004: the Connect page -- engines, the form, connect + introspect on a
    # worker. Its own category file so the area grows without crowding zones.
    src/Shell_connect.cpp
    # s004c: the Tables area -- the read-only schema browser.
    src/Shell_tables.cpp
    src/AboutWindow.cpp
    src/ShortcutsDialog.cpp
    src/Registry.cpp
    src/Log.cpp
)
target_include_directories(acorn PRIVATE include ${GTKMM_INCLUDE_DIRS})
target_link_libraries(acorn     PRIVATE ${GTKMM_LIBRARIES} acorn_core acorn_keyring spdlog::spdlog)
target_link_directories(acorn   PRIVATE ${GTKMM_LIBRARY_DIRS})
target_compile_options(acorn    PRIVATE ${GTKMM_CFLAGS_OTHER})
target_compile_definitions(acorn PRIVATE ACORN_VERSION="${PROJECT_VERSION}")
# The app loads engines at runtime; building it builds them.
if(ACORN_ENGINES)
    add_dependencies(acorn ${ACORN_ENGINES})
endif()
