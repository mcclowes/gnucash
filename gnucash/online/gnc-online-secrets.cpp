/********************************************************************\
 * gnc-online-secrets.cpp -- credential storage for online services *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652       *
 * Boston, MA  02110-1301,  USA       gnu@gnu.org                   *
\********************************************************************/

#include <config.h>

#include "gnc-online-secrets.hpp"
#include "gnc-json.hpp"

#include <glib.h>
#include <glib/gstdio.h>
#include <gnc-filepath-utils.h>
#include <qoflog.h>

#ifdef HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

G_GNUC_UNUSED static const char* log_module = "gnc.online";

namespace gnc::online
{

namespace
{

/** A JSON file in the user data directory, mode 0600. It is the
 *  fallback when no secret service is reachable (headless sessions,
 *  macOS and Windows builds without libsecret). */
class FileSecretStore : public SecretStore
{
public:
    FileSecretStore ()
    {
        auto path = gnc_build_userdata_path ("online-credentials.json");
        m_path = path;
        g_free (path);
    }

    std::optional<std::string> load (const std::string& service,
                                     const std::string& key) override
    {
        auto doc = read ();
        if (auto v = doc.find (service + "/" + key); v && v->is_string ())
            return v->as_string ();
        return std::nullopt;
    }

    void store (const std::string& service, const std::string& key,
                const std::string&, const std::string& secret) override
    {
        auto doc = read ();
        doc.set (service + "/" + key, secret);
        write (doc);
    }

    void erase (const std::string& service, const std::string& key) override
    {
        auto doc = read ();
        doc.erase (service + "/" + key);
        write (doc);
    }

    std::string description () const override
    {
        return "a private file (" + m_path + ")";
    }

private:
    json::Value read () const
    {
        gchar* contents = nullptr;
        if (!g_file_get_contents (m_path.c_str (), &contents, nullptr, nullptr))
            return json::Value::object ();
        std::string text (contents);
        g_free (contents);
        try
        {
            auto v = json::parse (text);
            if (v.is_object ())
                return v;
        }
        catch (const json::ParseError& e)
        {
            PWARN ("Ignoring unreadable credential file %s: %s",
                   m_path.c_str (), e.what ());
        }
        return json::Value::object ();
    }

    void write (const json::Value& doc) const
    {
        auto text = doc.dump (true);
        GError* error = nullptr;
        if (!g_file_set_contents_full (m_path.c_str (), text.c_str (),
                                       static_cast<gssize> (text.size ()),
                                       G_FILE_SET_CONTENTS_CONSISTENT,
                                       0600, &error))
        {
            PERR ("Unable to save credentials to %s: %s", m_path.c_str (),
                  error->message);
            g_error_free (error);
            return;
        }
        g_chmod (m_path.c_str (), 0600);
    }

    std::string m_path;
};

#ifdef HAVE_LIBSECRET
const SecretSchema*
online_schema ()
{
    static const SecretSchema schema = {
        "org.gnucash.Online", SECRET_SCHEMA_NONE,
        {
            { "service", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "key", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING },
        },
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
    };
    return &schema;
}

class LibsecretStore : public SecretStore
{
public:
    /** True when a secret service actually answers. Getting a proxy
     *  is not enough: the bus may exist with nothing behind it. */
    static bool available ()
    {
        GError* error = nullptr;
        auto pw = secret_password_lookup_sync (online_schema (), nullptr, &error,
                                               "service", "gnucash-probe",
                                               "key", "probe", nullptr);
        if (pw)
            secret_password_free (pw);
        if (error)
        {
            PINFO ("No usable secret service: %s", error->message);
            g_error_free (error);
            return false;
        }
        return true;
    }

    std::optional<std::string> load (const std::string& service,
                                     const std::string& key) override
    {
        GError* error = nullptr;
        auto pw = secret_password_lookup_sync (online_schema (), nullptr, &error,
                                               "service", service.c_str (),
                                               "key", key.c_str (), nullptr);
        if (error)
        {
            PWARN ("libsecret lookup failed: %s", error->message);
            g_error_free (error);
            return m_fallback.load (service, key);
        }
        if (!pw)
            return m_fallback.load (service, key);
        std::string out (pw);
        secret_password_free (pw);
        return out;
    }

    void store (const std::string& service, const std::string& key,
                const std::string& label, const std::string& secret) override
    {
        GError* error = nullptr;
        secret_password_store_sync (online_schema (), SECRET_COLLECTION_DEFAULT,
                                    label.c_str (), secret.c_str (), nullptr, &error,
                                    "service", service.c_str (),
                                    "key", key.c_str (), nullptr);
        if (error)
        {
            PWARN ("libsecret store failed, using file: %s", error->message);
            g_error_free (error);
            m_fallback.store (service, key, label, secret);
        }
    }

    void erase (const std::string& service, const std::string& key) override
    {
        secret_password_clear_sync (online_schema (), nullptr, nullptr,
                                    "service", service.c_str (),
                                    "key", key.c_str (), nullptr);
        m_fallback.erase (service, key);
    }

    std::string description () const override
    {
        return "the desktop keyring";
    }

private:
    FileSecretStore m_fallback;
};
#endif

} // anonymous namespace

SecretStore&
default_secret_store ()
{
#ifdef HAVE_LIBSECRET
    static std::unique_ptr<SecretStore> store =
        LibsecretStore::available ()
            ? std::unique_ptr<SecretStore> (std::make_unique<LibsecretStore> ())
            : std::unique_ptr<SecretStore> (std::make_unique<FileSecretStore> ());
#else
    static std::unique_ptr<SecretStore> store = std::make_unique<FileSecretStore> ();
#endif
    return *store;
}

} // namespace gnc::online
