/********************************************************************\
 * gnc-bank-feeds.hpp -- linked bank connections and their mapping  *
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
/** @file gnc-bank-feeds.hpp
 *  @brief The book's bank connections, and turning feed rows into
 *  GnuCash transactions for the generic import matcher.
 *
 *  Feed transactions go through the same matcher as OFX and CSV
 *  imports, so duplicate detection (by online id) and the learned
 *  account guesses come for free.
 */
#ifndef GNC_BANK_FEEDS_HPP
#define GNC_BANK_FEEDS_HPP

#include "gnc-json.hpp"
#include "gnc-plaid.hpp"
#include "gnc-online-secrets.hpp"

#include <Account.h>
#include <Transaction.h>
#include <qofbook.h>

#include <optional>
#include <string>
#include <vector>

namespace gnc::feeds
{

struct LinkedAccount
{
    std::string account_id; ///< Plaid's id
    std::string name, mask, type, currency;
    std::string gnc_account; ///< GUID of the GnuCash account, or empty
    bool ignore = false;
    std::string bank_balance;  ///< as last reported
};

struct Connection
{
    std::string item_id;
    std::string institution;
    std::string environment = "sandbox";
    std::string cursor;
    int64_t last_sync = 0;
    std::string status; ///< last error code, empty when healthy
    std::vector<LinkedAccount> accounts;

    /** True when every account is either mapped or ignored. */
    bool ready () const;
    LinkedAccount* find_account (const std::string& account_id);
    const LinkedAccount* find_account (const std::string& account_id) const;
};

/** All connections in a book ("online/bank-feeds"). */
struct Store
{
    std::vector<Connection> connections;
    bool import_pending = false;

    static Store load (QofBook* book);
    void save (QofBook* book) const;
    Connection* find (const std::string& item_id);
    void remove (const std::string& item_id);
};

/** Plaid credentials and access tokens, held in the secret store. */
plaid::Config load_plaid_config (online::SecretStore& secrets);
void save_plaid_config (online::SecretStore& secrets, const plaid::Config& cfg);
std::optional<std::string> load_access_token (online::SecretStore& secrets,
                                              const Connection& c);
void save_access_token (online::SecretStore& secrets, const Connection& c,
                        const std::string& token);
void erase_access_token (online::SecretStore& secrets, const Connection& c);

/** Merge a fresh account list from Plaid into @a c, keeping mappings. */
void merge_accounts (Connection& c, const std::vector<plaid::Account>& fresh);

/** Why a feed row was not turned into a transaction. */
enum class Skip { None, Pending, Unmapped, Ignored, Currency, BadData };

/** Build an open, uncommitted, single-split transaction for the
 *  matcher. Returns nullptr and sets @a why when the row is skipped. */
::Transaction* build_transaction (QofBook* book, const Connection& c,
                                  const plaid::Transaction& t,
                                  bool include_pending, Skip& why);

/** Find a split already imported from feed transaction @a id. */
Split* find_imported_split (QofBook* book, const Connection& c, const std::string& id);

/** A summary of what a sync will do, for the confirmation line. */
struct SyncSummary
{
    int added = 0, pending = 0, unmapped = 0, currency = 0;
    int modified = 0, removed = 0, already = 0;
    std::vector<std::string> removed_imported; ///< descriptions to review
};

} // namespace gnc::feeds

#endif /* GNC_BANK_FEEDS_HPP */
