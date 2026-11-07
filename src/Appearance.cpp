#include "Appearance.hpp"
#include "Log.hpp"

#include <gtkmm/settings.h>

// The D-Bus work is the GIO C API directly rather than gtkmm's wrappers around
// GDBusProxy, which vary across 4.x point releases. This is plumbing whose
// entire job is to compile on the first try against whatever gtkmm the machine
// has, and it is the one file in Slate that talks to the session bus.
#include <gio/gio.h>

namespace acorn::appearance {

namespace {

// The tri-state the portal speaks. Named rather than left as bare integers
// because `2` appearing in a comparison is exactly the kind of thing that gets
// "simplified" into `!= 1` by a later reader, which silently turns "prefer
// light" into "no preference".
enum class Scheme { NoPreference = 0, PreferDark = 1, PreferLight = 2 };

const char* scheme_name(Scheme s) {
    switch (s) {
        case Scheme::NoPreference: return "no-preference";
        case Scheme::PreferDark:   return "prefer-dark";
        case Scheme::PreferLight:  return "prefer-light";
    }
    return "?";
}

// Apply, or deliberately decline to. `no-preference` leaves GTK's own default
// alone: the desktop said nothing, and overriding a setting the user may have
// made elsewhere (GTK_THEME, a settings.ini) on the strength of silence would
// be this app inventing an opinion it was not given.
void apply(Scheme s) {
    auto lg = log::get(log::Area::App);

    if (s == Scheme::NoPreference) {
        if (lg) lg->info("appearance: no preference expressed; leaving GTK's default");
        return;
    }

    auto settings = Gtk::Settings::get_default();
    if (!settings) {
        if (lg) lg->error("appearance: no Gtk::Settings; cannot apply {}", scheme_name(s));
        return;
    }

    const bool dark = (s == Scheme::PreferDark);
    settings->property_gtk_application_prefer_dark_theme() = dark;
    if (lg) lg->info("appearance: applied {} (prefer_dark={})", scheme_name(s), dark);
}

// The portal hands back a value wrapped in a variant, and HOW MANY layers of
// wrapping depends on which method answered: the older Read() returns a variant
// holding a variant holding the number, the newer ReadOne() returns the number
// directly. Rather than encode that difference at each call site -- which is
// how a working app breaks against a portal version nobody tested -- peel until
// something that is not a variant is reached.
bool unwrap_u32(GVariant* v, guint32& out) {
    if (!v) return false;
    GVariant* cur = g_variant_ref(v);
    for (int depth = 0; depth < 8; ++depth) {
        if (g_variant_is_of_type(cur, G_VARIANT_TYPE_VARIANT)) {
            GVariant* inner = g_variant_get_variant(cur);
            g_variant_unref(cur);
            cur = inner;
            continue;
        }
        if (g_variant_is_of_type(cur, G_VARIANT_TYPE_UINT32)) {
            out = g_variant_get_uint32(cur);
            g_variant_unref(cur);
            return true;
        }
        // A tuple is what a method reply arrives as; step into its first child.
        if (g_variant_is_of_type(cur, G_VARIANT_TYPE_TUPLE) &&
            g_variant_n_children(cur) > 0) {
            GVariant* inner = g_variant_get_child_value(cur, 0);
            g_variant_unref(cur);
            cur = inner;
            continue;
        }
        break;
    }
    g_variant_unref(cur);
    return false;
}

// Live updates. The desktop's dark toggle is a switch a person flips while
// looking at the screen, so an app that only reads at startup is correct
// exactly until the moment the user tests it.
void on_setting_changed(GDBusProxy*, const gchar*, const gchar* signal_name,
                        GVariant* parameters, gpointer) {
    if (g_strcmp0(signal_name, "SettingChanged") != 0) return;
    if (g_variant_n_children(parameters) < 3) return;

    const gchar* ns  = nullptr;
    const gchar* key = nullptr;
    g_variant_get_child(parameters, 0, "&s", &ns);
    g_variant_get_child(parameters, 1, "&s", &key);
    if (g_strcmp0(ns, "org.freedesktop.appearance") != 0) return;
    if (g_strcmp0(key, "color-scheme") != 0) return;

    GVariant* value = g_variant_get_child_value(parameters, 2);
    guint32   raw   = 0;
    if (unwrap_u32(value, raw) && raw <= 2) apply(static_cast<Scheme>(raw));
    g_variant_unref(value);
}

// Returns true if the portal answered -- whatever it answered. A portal that
// says "no preference" HAS answered, and falling through to GSettings after it
// would let GNOME's private key override the cross-desktop one.
bool try_portal() {
    auto lg = log::get(log::Area::App);

    GError* err = nullptr;
    // Held for the process lifetime on purpose: the proxy is what the
    // SettingChanged subscription lives on, so releasing it would silently
    // reduce this to a startup-only read. Never unref'd, and the leak is one
    // object that dies with the process.
    GDBusProxy* proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, nullptr,
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings", nullptr, &err);

    if (!proxy) {
        if (lg) lg->info("appearance: no settings portal ({})",
                         err ? err->message : "no detail");
        g_clear_error(&err);
        return false;
    }

    // ReadOne first: Read is deprecated and, on some portal versions, answers
    // with an extra layer of variant. Both are tried because the newer method
    // is simply absent on older portals, which surfaces as an
    // UNKNOWN_METHOD error rather than as anything a version check could see
    // from here.
    GVariant* args = g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme");
    g_variant_ref_sink(args);
    GVariant* reply = g_dbus_proxy_call_sync(
        proxy, "ReadOne", args, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &err);

    if (!reply) {
        g_clear_error(&err);
        reply = g_dbus_proxy_call_sync(
            proxy, "Read", args, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &err);
    }
    g_variant_unref(args);

    if (!reply) {
        if (lg) lg->info("appearance: portal present but color-scheme unreadable ({})",
                         err ? err->message : "no detail");
        g_clear_error(&err);
        return false;
    }

    guint32 raw = 0;
    const bool got = unwrap_u32(reply, raw);
    g_variant_unref(reply);

    if (!got || raw > 2) {
        if (lg) lg->warn("appearance: portal returned an unusable color-scheme");
        return false;
    }

    if (lg) lg->info("appearance: portal says {}", scheme_name(static_cast<Scheme>(raw)));
    apply(static_cast<Scheme>(raw));

    // Only now subscribe. Subscribing before the read would be a race the
    // wrong way round -- a change arriving between the two would be applied and
    // then overwritten by the stale startup value.
    g_signal_connect(proxy, "g-signal", G_CALLBACK(on_setting_changed), nullptr);
    return true;
}

// GNOME's private key, used only when no portal answered.
//
// The schema is looked up before it is opened. g_settings_new() on a schema
// that is not installed does not return null -- it ABORTS THE PROCESS. So the
// obvious two-line version of this function turns "no GNOME schemas installed"
// into a crash on startup, which is a strictly worse outcome than the light
// window it was written to fix.
void try_gsettings() {
    auto lg = log::get(log::Area::App);

    GSettingsSchemaSource* src = g_settings_schema_source_get_default();
    if (!src) {
        if (lg) lg->info("appearance: no GSettings schema source; leaving GTK's default");
        return;
    }

    GSettingsSchema* schema =
        g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE);
    if (!schema) {
        if (lg) lg->info("appearance: org.gnome.desktop.interface not installed; "
                         "leaving GTK's default");
        return;
    }

    const bool has_key = g_settings_schema_has_key(schema, "color-scheme");
    if (!has_key) {
        if (lg) lg->info("appearance: schema present but has no color-scheme key");
        g_settings_schema_unref(schema);
        return;
    }

    GSettings*  s     = g_settings_new_full(schema, nullptr, nullptr);
    gchar*      value = g_settings_get_string(s, "color-scheme");
    const Scheme sch  = g_strcmp0(value, "prefer-dark") == 0  ? Scheme::PreferDark
                      : g_strcmp0(value, "prefer-light") == 0 ? Scheme::PreferLight
                                                              : Scheme::NoPreference;
    if (lg) lg->info("appearance: gsettings says '{}'", value ? value : "(null)");
    apply(sch);

    g_free(value);
    g_settings_schema_unref(schema);
    // `s` is deliberately kept: it carries the change subscription below.
    g_signal_connect(s, "changed::color-scheme",
                     G_CALLBACK(+[](GSettings* gs, const gchar*, gpointer) {
                         gchar* v = g_settings_get_string(gs, "color-scheme");
                         apply(g_strcmp0(v, "prefer-dark") == 0  ? Scheme::PreferDark
                             : g_strcmp0(v, "prefer-light") == 0 ? Scheme::PreferLight
                                                                 : Scheme::NoPreference);
                         g_free(v);
                     }),
                     nullptr);
}

}  // namespace

void follow_system() {
    if (!try_portal()) try_gsettings();
}

}  // namespace acorn::appearance
