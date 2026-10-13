/********************************************************************\
 * gnc-plaid.hpp -- Plaid API client for bank feeds                 *
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
/** @file gnc-plaid.hpp
 *  @brief The parts of the Plaid API a bank feed needs.
 *
 *  Plaid is reached directly with the user's own client id and secret,
 *  so no GnuCash-operated server sits between the user and their bank
 *  data. Banks are connected through Plaid Hosted Link, shown in an
 *  embedded browser; transactions arrive through /transactions/sync,
 *  whose cursor is kept in the book so each sync picks up where the
 *  last one stopped.
 *
 *  Reference: https://plaid.com/docs/api/
 */
#ifndef GNC_PLAID_HPP
#define GNC_PLAID_HPP

#include "gnc-json.hpp"
#include "gnc-online-http.hpp"

#include <string>
#include <utility>
#include <vector>

namespace gnc::plaid
{

struct Config
{
    std::string client_id;
    std::string secret;
    std::string environment = "sandbox"; ///< "sandbox" or "production"
    std::vector<std::string> country_codes = {"GB"};

    std::string base_url () const;
    bool complete () const { return !client_id.empty () && !secret.empty (); }
    json::Value to_json () const;
    static Config from_json (const json::Value&);
};

struct LinkToken
{
    std::string link_token;
    std::string expiration;
    std::string hosted_link_url;
};

struct Account
{
    std::string account_id, name, official_name, mask, type, subtype;
    std::string currency;
    std::string current_balance, available_balance; ///< decimal literals, may be empty
};

struct Transaction
{
    std::string transaction_id, account_id;
    std::string amount;     ///< Plaid sign: positive is money leaving the account.
    std::string currency;
    std::string date;       ///< YYYY-MM-DD, posted
    std::string authorized_date;
    std::string name, merchant_name, category;
    bool pending = false;
    /** For a posted transaction, the id it had while pending. */
    std::string pending_transaction_id;
};

struct SyncResult
{
    std::vector<Transaction> added, modified;
    std::vector<std::string> removed;
    std::string next_cursor;
    std::string update_status; ///< e.g. "HISTORICAL_UPDATE_COMPLETE"
};

struct Item
{
    std::string item_id, institution_id, institution_name;
};

class Client
{
public:
    Client (online::HttpTransport& http, Config cfg);

    /** A Link token for Hosted Link. Pass @a access_token to reconnect
     *  an existing item (update mode) when its login has expired. */
    LinkToken create_link_token (const std::string& client_user_id,
                                 const std::string& completion_redirect_uri,
                                 const std::string& access_token = "") const;
    /** Public tokens produced by finished Link sessions of @a link_token. */
    std::vector<std::string> link_public_tokens (const std::string& link_token) const;
    /** Returns {access_token, item_id}. */
    std::pair<std::string, std::string> exchange_public_token (const std::string& public_token) const;
    /** Sandbox only: a public token without going through Link. */
    std::string sandbox_public_token (const std::string& institution_id) const;

    Item item (const std::string& access_token) const;
    std::vector<Account> accounts (const std::string& access_token) const;
    /** Pull every page after @a cursor (empty for the full history). */
    SyncResult sync (const std::string& access_token, const std::string& cursor) const;
    void remove_item (const std::string& access_token) const;

private:
    json::Value post (const std::string& path, json::Value body) const;

    online::HttpTransport& m_http;
    Config m_cfg;
};

/** The sandbox's UK test bank ("Royal Bank of Plaid"). */
constexpr const char* SANDBOX_UK_INSTITUTION = "ins_117650";

} // namespace gnc::plaid

#endif /* GNC_PLAID_HPP */
