/********************************************************************\
 * gnc-online-secrets.hpp -- credential storage for online services *
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
/** @file gnc-online-secrets.hpp
 *  @brief Where API keys and tokens live.
 *
 *  Secrets never go in the book: a book is a file that gets copied,
 *  mailed to accountants and backed up. They go to the desktop secret
 *  service (libsecret) when one is running, otherwise to a file in the
 *  user's GnuCash data directory readable only by the owner.
 */
#ifndef GNC_ONLINE_SECRETS_HPP
#define GNC_ONLINE_SECRETS_HPP

#include <map>
#include <optional>
#include <string>

namespace gnc::online
{

class SecretStore
{
public:
    virtual ~SecretStore () = default;
    virtual std::optional<std::string> load (const std::string& service,
                                             const std::string& key) = 0;
    virtual void store (const std::string& service, const std::string& key,
                        const std::string& label, const std::string& secret) = 0;
    virtual void erase (const std::string& service, const std::string& key) = 0;
    /** A short human description, e.g. "the GNOME keyring". */
    virtual std::string description () const = 0;
};

/** The process-wide store: libsecret if usable, else the private file. */
SecretStore& default_secret_store ();

/** An in-memory store, for tests. */
class MemorySecretStore : public SecretStore
{
public:
    std::optional<std::string> load (const std::string& service,
                                     const std::string& key) override
    {
        auto it = m_items.find (service + "\n" + key);
        if (it == m_items.end ())
            return std::nullopt;
        return it->second;
    }
    void store (const std::string& service, const std::string& key,
                const std::string&, const std::string& secret) override
    {
        m_items[service + "\n" + key] = secret;
    }
    void erase (const std::string& service, const std::string& key) override
    {
        m_items.erase (service + "\n" + key);
    }
    std::string description () const override { return "memory"; }
private:
    std::map<std::string, std::string> m_items;
};

} // namespace gnc::online

#endif /* GNC_ONLINE_SECRETS_HPP */
