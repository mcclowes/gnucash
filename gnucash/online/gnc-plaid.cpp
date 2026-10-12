/********************************************************************\
 * gnc-plaid.cpp -- Plaid API client for bank feeds                 *
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

#include "gnc-plaid.hpp"

#include <algorithm>

namespace gnc::plaid
{

using online::OnlineError;

std::string
Config::base_url () const
{
    return environment == "production" ? "https://production.plaid.com"
                                       : "https://sandbox.plaid.com";
}

json::Value
Config::to_json () const
{
    auto v = json::Value::object ();
    v.set ("client_id", client_id);
    v.set ("secret", secret);
    v.set ("environment", environment);
    auto cc = json::Value::array ();
    for (const auto& c : country_codes)
        cc.push (c);
    v.set ("country_codes", cc);
    return v;
}

Config
Config::from_json (const json::Value& v)
{
    Config c;
    c.client_id = v.get_string ("client_id");
    c.secret = v.get_string ("secret");
    c.environment = v.get_string ("environment", "sandbox");
    if (auto cc = v.find ("country_codes"); cc && cc->is_array () && !cc->as_array ().empty ())
    {
        c.country_codes.clear ();
        for (const auto& x : cc->as_array ())
            if (x.is_string ())
                c.country_codes.push_back (x.as_string ());
    }
    return c;
}

Client::Client (online::HttpTransport& http, Config cfg)
    : m_http{http}, m_cfg{std::move (cfg)} {}

json::Value
Client::post (const std::string& path, json::Value body) const
{
    body.set ("client_id", m_cfg.client_id);
    body.set ("secret", m_cfg.secret);

    online::HttpRequest req;
    req.method = "POST";
    req.url = m_cfg.base_url () + path;
    req.headers = {{"Content-Type", "application/json"},
                   {"Plaid-Version", "2020-09-14"}};
    req.body = body.dump ();
    auto resp = m_http.send (req);

    json::Value v;
    try
    {
        v = json::parse (resp.body);
    }
    catch (const json::ParseError&)
    {
        throw OnlineError ("Plaid returned an unreadable response (HTTP " +
                           std::to_string (resp.status) + ").", resp.status);
    }
    if (!resp.ok ())
    {
        auto code = v.get_string ("error_code");
        auto display = v.get_string ("display_message");
        auto message = display.empty () ? v.get_string ("error_message") : display;
        if (code == "ITEM_LOGIN_REQUIRED")
            message = "The bank needs you to sign in again. Use Reconnect.";
        else if (code == "INVALID_API_KEYS")
            message = "Plaid rejected the client id or secret for this environment.";
        if (message.empty ())
            message = "Plaid returned HTTP " + std::to_string (resp.status) + ".";
        throw OnlineError (message, resp.status, code);
    }
    return v;
}

LinkToken
Client::create_link_token (const std::string& client_user_id,
                           const std::string& completion_redirect_uri,
                           const std::string& access_token) const
{
    auto body = json::Value::object ();
    body.set ("client_name", "GnuCash");
    body.set ("language", "en");
    auto cc = json::Value::array ();
    for (const auto& c : m_cfg.country_codes)
        cc.push (c);
    body.set ("country_codes", cc);
    auto user = json::Value::object ();
    user.set ("client_user_id", client_user_id);
    body.set ("user", user);
    if (access_token.empty ())
    {
        body.set ("products", json::Array{json::Value ("transactions")});
        auto tx = json::Value::object ();
        tx.set ("days_requested", 730);
        body.set ("transactions", tx);
    }
    else
        body.set ("access_token", access_token);
    auto hosted = json::Value::object ();
    if (!completion_redirect_uri.empty ())
        hosted.set ("completion_redirect_uri", completion_redirect_uri);
    body.set ("hosted_link", hosted);

    auto v = post ("/link/token/create", body);
    return {v.get_string ("link_token"), v.get_string ("expiration"),
            v.get_string ("hosted_link_url")};
}

std::vector<std::string>
Client::link_public_tokens (const std::string& link_token) const
{
    auto body = json::Value::object ();
    body.set ("link_token", link_token);
    auto v = post ("/link/token/get", body);

    std::vector<std::string> out;
    auto sessions = v.find ("link_sessions");
    if (!sessions || !sessions->is_array ())
        return out;
    for (const auto& s : sessions->as_array ())
    {
        if (auto results = s["results"].find ("item_add_results");
            results && results->is_array ())
            for (const auto& r : results->as_array ())
                if (auto pt = r.get_string ("public_token"); !pt.empty ())
                    out.push_back (pt);
        if (auto pt = s["on_success"].get_string ("public_token"); !pt.empty () &&
            std::find (out.begin (), out.end (), pt) == out.end ())
            out.push_back (pt);
    }
    return out;
}

std::pair<std::string, std::string>
Client::exchange_public_token (const std::string& public_token) const
{
    auto body = json::Value::object ();
    body.set ("public_token", public_token);
    auto v = post ("/item/public_token/exchange", body);
    return {v.get_string ("access_token"), v.get_string ("item_id")};
}

std::string
Client::sandbox_public_token (const std::string& institution_id) const
{
    auto body = json::Value::object ();
    body.set ("institution_id", institution_id);
    body.set ("initial_products", json::Array{json::Value ("transactions")});
    return post ("/sandbox/public_token/create", body).get_string ("public_token");
}

Item
Client::item (const std::string& access_token) const
{
    auto body = json::Value::object ();
    body.set ("access_token", access_token);
    auto v = post ("/item/get", body);
    const auto& item = v["item"];
    Item out{item.get_string ("item_id"), item.get_string ("institution_id"),
             item.get_string ("institution_name")};
    if (out.institution_name.empty () && !out.institution_id.empty ())
    {
        try
        {
            auto q = json::Value::object ();
            q.set ("institution_id", out.institution_id);
            auto cc = json::Value::array ();
            for (const auto& c : m_cfg.country_codes)
                cc.push (c);
            q.set ("country_codes", cc);
            out.institution_name = post ("/institutions/get_by_id", q)["institution"].get_string ("name");
        }
        catch (const OnlineError&) {} // a name is a nicety
    }
    return out;
}

static std::string
balance (const json::Value& balances, const char* key)
{
    auto v = balances.find (key);
    return (v && v->is_number ()) ? v->as_string () : std::string{};
}

std::vector<Account>
Client::accounts (const std::string& access_token) const
{
    auto body = json::Value::object ();
    body.set ("access_token", access_token);
    auto v = post ("/accounts/get", body);
    std::vector<Account> out;
    if (auto arr = v.find ("accounts"); arr && arr->is_array ())
        for (const auto& a : arr->as_array ())
        {
            const auto& bal = a["balances"];
            out.push_back ({a.get_string ("account_id"), a.get_string ("name"),
                            a.get_string ("official_name"), a.get_string ("mask"),
                            a.get_string ("type"), a.get_string ("subtype"),
                            bal.get_string ("iso_currency_code",
                                            bal.get_string ("unofficial_currency_code")),
                            balance (bal, "current"), balance (bal, "available")});
        }
    return out;
}

static Transaction
parse_transaction (const json::Value& t)
{
    Transaction out;
    out.transaction_id = t.get_string ("transaction_id");
    out.account_id = t.get_string ("account_id");
    out.amount = t.get_string ("amount", "0");
    out.currency = t.get_string ("iso_currency_code",
                                 t.get_string ("unofficial_currency_code"));
    out.date = t.get_string ("date");
    out.authorized_date = t.get_string ("authorized_date");
    out.name = t.get_string ("name");
    out.merchant_name = t.get_string ("merchant_name");
    out.category = t["personal_finance_category"].get_string ("primary");
    out.pending = t.get_bool ("pending");
    return out;
}

SyncResult
Client::sync (const std::string& access_token, const std::string& cursor) const
{
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        SyncResult result;
        std::string next = cursor;
        try
        {
            while (true)
            {
                auto body = json::Value::object ();
                body.set ("access_token", access_token);
                if (!next.empty ())
                    body.set ("cursor", next);
                body.set ("count", 500);
                auto v = post ("/transactions/sync", body);

                for (const auto& t : v["added"].is_array () ? v["added"].as_array () : json::Array{})
                    result.added.push_back (parse_transaction (t));
                for (const auto& t : v["modified"].is_array () ? v["modified"].as_array () : json::Array{})
                    result.modified.push_back (parse_transaction (t));
                for (const auto& t : v["removed"].is_array () ? v["removed"].as_array () : json::Array{})
                    result.removed.push_back (t.get_string ("transaction_id"));
                next = v.get_string ("next_cursor");
                result.update_status = v.get_string ("transactions_update_status");
                if (!v.get_bool ("has_more"))
                    break;
            }
            result.next_cursor = next;
            return result;
        }
        catch (const OnlineError& e)
        {
            // Plaid asks that pagination restart from the first cursor.
            if (e.code () != "TRANSACTIONS_SYNC_MUTATION_DURING_PAGINATION")
                throw;
        }
    }
    throw OnlineError ("The bank's data kept changing during the download. Try again shortly.");
}

void
Client::remove_item (const std::string& access_token) const
{
    auto body = json::Value::object ();
    body.set ("access_token", access_token);
    post ("/item/remove", body);
}

} // namespace gnc::plaid
