#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Appearance -- follow the desktop's light/dark preference.
//
// Contents:
//   follow_system()  -- read the preference now, and keep following it.
//
// The page: docs/following-the-desktop-appearance.md -- why each guard here
// exists, and the two call-site judgments that don't travel with this file.
//
// Why this file has to exist at all
// ---------------------------------
// GTK4 does not do this by itself. A GTK3 app got dark for free because the
// preference WAS the theme -- one string, `gtk-theme-name`, and picking a dark
// theme was how you asked. GTK4 split the two: the desktop now expresses "I
// want dark" as a separate preference, and the toolkit deliberately leaves
// acting on it to the application. libadwaita is the usual place that acting
// happens (AdwStyleManager), and Cairn does not link libadwaita.
//
// So an app that does nothing renders light on a dark desktop. Nothing in the
// window is at fault: Cairn ships no stylesheet, and every chrome widget is
// stock, so all of them already wear whatever GTK hands them. The only thing
// missing was ASKING.
//
// Ported verbatim from Slate (s22), which took it from Plunk, where the bug
// first surfaced on a Fedora upgrade. Only the namespace changed. Every guard
// in it exists for a failure that was actually hit -- see the notes on
// GSettings aborting, on the variant depth, and on the subscribe/read order.
// Resist tidying them: each one is a scar.
//
// Where the preference lives, and why not GSettings
// -------------------------------------------------
// Two places claim to hold it, and the obvious one is the wrong one:
//
//   * `org.gnome.desktop.interface color-scheme` via GSettings -- readable,
//     and what the GNOME control panel writes. It is GNOME's private key. An
//     app reading it directly works on GNOME and is blind on KDE, Xfce, Sway
//     and everything else, and it hard-codes another desktop's schema name
//     into a tool that has nothing to do with that desktop. Worse, GSettings
//     ABORTS THE PROCESS when asked for a schema that is not installed, so the
//     naive version of this crashes on a machine without GNOME's schemas
//     rather than falling back.
//
//   * `org.freedesktop.appearance color-scheme` via the XDG desktop portal --
//     the cross-desktop interface every desktop is expected to answer, and
//     what GNOME's own guidance points at. It works unsandboxed, it works
//     inside Flatpak, and it is the same call on every desktop.
//
// The portal is what this uses, with GSettings as an explicitly guarded
// fallback for a session that has no portal running -- which happens more than
// it should, because the portal is D-Bus-activated and an app launched early
// enough can start before it is up.
//
// The values are a tri-state, not a boolean: 0 = no preference, 1 = prefer
// dark, 2 = prefer light. "No preference" is NOT "light" -- it means the
// desktop declined to say, and the honest response is to leave GTK's own
// default alone rather than to force light and override a theme the user may
// have chosen by other means.
// ─────────────────────────────────────────────────────────────────────────────
namespace acorn::appearance {

// Apply the desktop's colour-scheme preference to this application, and keep
// applying it as it changes. Safe to call once, after the display exists
// (App::on_activate) and before any window is built, so the first frame is
// already the right colour rather than a light flash that repaints.
//
// Never throws and never blocks for long: the startup read is a D-Bus call
// with a short timeout, and every failure path logs what it found and leaves
// GTK's default in place. A desktop with no portal, no GNOME schemas and no
// opinion is a supported configuration, not an error.
void follow_system();

}  // namespace acorn::appearance
