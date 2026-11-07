#include "Keyring.hpp"

#include <libsecret/secret.h>

#include <memory>

// Keyring.cpp -- the libsecret calls. Each async call carries its callback on
// the heap through user_data; the ready-callback takes it back, finishes the
// call, and runs it. The password is wiped from libsecret's buffer with
// secret_password_free (it zeroes before freeing).

namespace acorn::keyring {

namespace {

const SecretSchema* schema() {
    static const SecretSchema s = {
        "io.github.scott8420.Acorn.Connection",
        SECRET_SCHEMA_NONE,
        {
            {"engine", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {"host", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {"port", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {"database", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {"user", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
        },
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    return &s;
}

// The attribute table every call uses -- one place, so store / lookup / clear
// can't disagree about what identifies an entry.
GHashTable* attributes(const Account& a) {
    GHashTable* t = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    const auto put = [t](const char* k, const std::string& v) {
        g_hash_table_insert(t, g_strdup(k), g_strdup(v.c_str()));
    };
    put("engine", a.engine);
    put("host", a.host.empty() ? std::string("localhost") : a.host);
    put("port", std::to_string(a.port));
    put("database", a.database);
    put("user", a.user);
    return t;
}

std::string take_error(GError* e) {
    if (!e) return {};
    std::string s = e->message ? e->message : "keyring error";
    g_error_free(e);
    return s;
}

}  // namespace

const char* schema_name() { return schema()->name; }

void store(const Account& a, const std::string& password,
           std::function<void(bool, std::string)> done) {
    using Cb = std::function<void(bool, std::string)>;
    GHashTable* attrs = attributes(a);
    secret_password_storev(
        schema(), attrs, SECRET_COLLECTION_DEFAULT, a.label.c_str(), password.c_str(), nullptr,
        [](GObject*, GAsyncResult* res, gpointer data) {
            std::unique_ptr<Cb> cb(static_cast<Cb*>(data));
            GError* err = nullptr;
            const bool ok = secret_password_store_finish(res, &err);
            const std::string msg = take_error(err);
            if (*cb) (*cb)(ok, msg);
        },
        new Cb(std::move(done)));
    g_hash_table_unref(attrs);
}

void lookup(const Account& a, std::function<void(std::optional<std::string>, std::string)> done) {
    using Cb = std::function<void(std::optional<std::string>, std::string)>;
    GHashTable* attrs = attributes(a);
    secret_password_lookupv(
        schema(), attrs, nullptr,
        [](GObject*, GAsyncResult* res, gpointer data) {
            std::unique_ptr<Cb> cb(static_cast<Cb*>(data));
            GError* err = nullptr;
            gchar* pw = secret_password_lookup_finish(res, &err);
            std::optional<std::string> out;
            if (pw) {
                out = std::string(pw);
                secret_password_free(pw);
            }
            const std::string msg = take_error(err);
            if (*cb) (*cb)(std::move(out), msg);
        },
        new Cb(std::move(done)));
    g_hash_table_unref(attrs);
}

void clear(const Account& a, std::function<void(bool, std::string)> done) {
    using Cb = std::function<void(bool, std::string)>;
    GHashTable* attrs = attributes(a);
    secret_password_clearv(
        schema(), attrs, nullptr,
        [](GObject*, GAsyncResult* res, gpointer data) {
            std::unique_ptr<Cb> cb(static_cast<Cb*>(data));
            GError* err = nullptr;
            const bool removed = secret_password_clear_finish(res, &err);
            const std::string msg = take_error(err);
            if (*cb) (*cb)(removed, msg);
        },
        new Cb(std::move(done)));
    g_hash_table_unref(attrs);
}

}  // namespace acorn::keyring
