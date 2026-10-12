/********************************************************************\
 * gnc-bank-feeds.cpp -- linked bank connections and their mapping  *
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

#include "gnc-bank-feeds.hpp"
#include "gnc-online-book.hpp"

#include <Account.hpp>
#include <Split.h>
#include <gnc-commodity.h>
#include <gnc-date.h>
#include <qofinstance-p.h>

#include <algorithm>

namespace gnc::feeds
{

using online::account_from_guid_string;

bool
Connection::ready () const
{
    return std::all_of (accounts.begin (), accounts.end (), [](const LinkedAccount& a) {
        return a.ignore || !a.gnc_account.empty ();
    });
}

LinkedAccount*
Connection::find_account (const std::string& account_id)
{
    for (auto& a : accounts)
        if (a.account_id == account_id)
            return &a;
    return nullptr;
}

const LinkedAccount*
Connection::find_account (const std::string& account_id) const
{
    for (const auto& a : accounts)
        if (a.account_id == account_id)
            return &a;
    return nullptr;
}

Store
Store::load (QofBook* book)
{
    Store s;
    auto doc = online::load_book_json (book, "bank-feeds");
    s.import_pending = doc.get_bool ("import_pending", false);
    if (auto arr = doc.find ("connections"); arr && arr->is_array ())
        for (const auto& c : arr->as_array ())
        {
            Connection conn;
            conn.item_id = c.get_string ("item_id");
            conn.institution = c.get_string ("institution");
            conn.environment = c.get_string ("environment", "sandbox");
            conn.cursor = c.get_string ("cursor");
            conn.status = c.get_string ("status");
            if (auto ls = c.find ("last_sync"); ls && ls->is_number ())
                conn.last_sync = ls->as_int ();
            if (auto accts = c.find ("accounts"); accts && accts->is_array ())
                for (const auto& a : accts->as_array ())
                    conn.accounts.push_back ({a.get_string ("account_id"), a.get_string ("name"),
                                              a.get_string ("mask"), a.get_string ("type"),
                                              a.get_string ("currency"),
                                              a.get_string ("gnc_account"),
                                              a.get_bool ("ignore"),
                                              a.get_string ("bank_balance")});
            s.connections.push_back (std::move (conn));
        }
    return s;
}

void
Store::save (QofBook* book) const
{
    auto doc = json::Value::object ();
    doc.set ("import_pending", import_pending);
    auto arr = json::Value::array ();
    for (const auto& c : connections)
    {
        auto o = json::Value::object ();
        o.set ("item_id", c.item_id);
        o.set ("institution", c.institution);
        o.set ("environment", c.environment);
        o.set ("cursor", c.cursor);
        o.set ("status", c.status);
        o.set ("last_sync", c.last_sync);
        auto accts = json::Value::array ();
        for (const auto& a : c.accounts)
        {
            auto ao = json::Value::object ();
            ao.set ("account_id", a.account_id);
            ao.set ("name", a.name);
            ao.set ("mask", a.mask);
            ao.set ("type", a.type);
            ao.set ("currency", a.currency);
            ao.set ("gnc_account", a.gnc_account);
            ao.set ("ignore", a.ignore);
            ao.set ("bank_balance", a.bank_balance);
            accts.push (ao);
        }
        o.set ("accounts", accts);
        arr.push (o);
    }
    doc.set ("connections", arr);
    online::save_book_json (book, "bank-feeds", doc);
}

Connection*
Store::find (const std::string& item_id)
{
    for (auto& c : connections)
        if (c.item_id == item_id)
            return &c;
    return nullptr;
}

void
Store::remove (const std::string& item_id)
{
    connections.erase (std::remove_if (connections.begin (), connections.end (),
                                       [&](const Connection& c) { return c.item_id == item_id; }),
                       connections.end ());
}

/* ---------------------------------------------------------------- */

static const char* PLAID_SERVICE = "plaid";

static std::string
token_service (const Connection& c)
{
    return std::string (PLAID_SERVICE) + "-" + c.environment;
}

plaid::Config
load_plaid_config (online::SecretStore& secrets)
{
    if (auto stored = secrets.load (PLAID_SERVICE, "config"))
    {
        try { return plaid::Config::from_json (json::parse (*stored)); }
        catch (const std::exception&) {}
    }
    return {};
}

void
save_plaid_config (online::SecretStore& secrets, const plaid::Config& cfg)
{
    secrets.store (PLAID_SERVICE, "config", "GnuCash Plaid API keys",
                   cfg.to_json ().dump ());
}

std::optional<std::string>
load_access_token (online::SecretStore& secrets, const Connection& c)
{
    return secrets.load (token_service (c), c.item_id);
}

void
save_access_token (online::SecretStore& secrets, const Connection& c,
                   const std::string& token)
{
    secrets.store (token_service (c), c.item_id,
                   "GnuCash bank feed: " + (c.institution.empty () ? c.item_id : c.institution),
                   token);
}

void
erase_access_token (online::SecretStore& secrets, const Connection& c)
{
    secrets.erase (token_service (c), c.item_id);
}

void
merge_accounts (Connection& c, const std::vector<plaid::Account>& fresh)
{
    std::vector<LinkedAccount> merged;
    for (const auto& f : fresh)
    {
        LinkedAccount la;
        if (auto existing = c.find_account (f.account_id))
            la = *existing;
        la.account_id = f.account_id;
        la.name = f.name.empty () ? f.official_name : f.name;
        la.mask = f.mask;
        la.type = f.subtype.empty () ? f.type : f.subtype;
        la.currency = f.currency;
        la.bank_balance = f.current_balance;
        merged.push_back (std::move (la));
    }
    c.accounts = std::move (merged);
}

/* ---------------------------------------------------------------- */

::Transaction*
build_transaction (QofBook* book, const Connection& c, const plaid::Transaction& t,
                   bool include_pending, Skip& why)
{
    why = Skip::None;
    auto linked = c.find_account (t.account_id);
    if (!linked || linked->gnc_account.empty ())
    {
        why = (linked && linked->ignore) ? Skip::Ignored : Skip::Unmapped;
        return nullptr;
    }
    if (linked->ignore)
    {
        why = Skip::Ignored;
        return nullptr;
    }
    if (t.pending && !include_pending)
    {
        why = Skip::Pending;
        return nullptr;
    }
    auto acc = account_from_guid_string (book, linked->gnc_account);
    if (!acc)
    {
        why = Skip::Unmapped;
        return nullptr;
    }
    auto comm = xaccAccountGetCommodity (acc);
    if (!comm || !gnc_commodity_is_currency (comm) ||
        (!t.currency.empty () && t.currency != gnc_commodity_get_mnemonic (comm)))
    {
        why = Skip::Currency;
        return nullptr;
    }

    auto parsed = online::parse_decimal (t.amount);
    int y = 0, m = 0, d = 0;
    if (!parsed || sscanf (t.date.c_str (), "%d-%d-%d", &y, &m, &d) != 3)
    {
        why = Skip::BadData;
        return nullptr;
    }
    // Plaid reports outflows as positive; GnuCash records them as credits.
    auto value = gnc_numeric_convert (gnc_numeric_neg (*parsed),
                                      xaccAccountGetCommoditySCU (acc),
                                      GNC_HOW_RND_ROUND_HALF_UP);

    auto trans = xaccMallocTransaction (book);
    xaccTransBeginEdit (trans);
    xaccTransSetCurrency (trans, comm);
    xaccTransSetDatePostedSecsNormalized (trans, gnc_dmy2time64_neutral (d, m, y));
    xaccTransSetDateEnteredSecs (trans, gnc_time (nullptr));
    const auto& desc = t.merchant_name.empty () ? t.name : t.merchant_name;
    xaccTransSetDescription (trans, desc.c_str ());
    if (!t.category.empty ())
        xaccTransSetNotes (trans, t.category.c_str ());

    auto split = xaccMallocSplit (book);
    xaccSplitSetParent (split, trans);
    xaccSplitSetAccount (split, acc);
    xaccSplitSetAmount (split, value);
    xaccSplitSetValue (split, value);
    if (!t.merchant_name.empty () && t.name != t.merchant_name)
        xaccSplitSetMemo (split, t.name.c_str ());
    xaccSplitSetOnlineID (split, t.transaction_id.c_str ());
    return trans;
}

Split*
find_imported_split (QofBook* book, const Connection& c, const std::string& id)
{
    for (const auto& la : c.accounts)
    {
        auto acc = account_from_guid_string (book, la.gnc_account);
        if (!acc)
            continue;
        for (auto split : xaccAccountGetSplits (acc))
        {
            auto oid = xaccSplitGetOnlineID (split);
            if (oid && id == oid)
                return split;
        }
    }
    return nullptr;
}

} // namespace gnc::feeds
