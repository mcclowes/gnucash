/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
\********************************************************************/
#include <config.h>

#include "gnc-plaid.hpp"
#include "fake-transport.hpp"

#include <gtest/gtest.h>

using namespace gnc;
using namespace gnc::plaid;

static Client
make_client (FakeTransport& t)
{
    Config cfg;
    cfg.client_id = "cid";
    cfg.secret = "sec";
    return Client (t, cfg);
}

TEST (Plaid, link_token_requests_hosted_link_for_uk_transactions)
{
    FakeTransport t;
    t.reply ("/link/token/create", 200,
             R"({"link_token":"link-sandbox-1","expiration":"2026-10-12T22:00:00Z","hosted_link_url":"https://secure.plaid.com/hl/abc"})");
    auto lt = make_client (t).create_link_token ("user-1", "https://example.invalid/done");
    EXPECT_EQ ("link-sandbox-1", lt.link_token);
    EXPECT_EQ ("https://secure.plaid.com/hl/abc", lt.hosted_link_url);

    auto body = json::parse (t.requests[0].body);
    EXPECT_EQ ("https://sandbox.plaid.com/link/token/create", t.requests[0].url);
    EXPECT_EQ ("cid", body.get_string ("client_id"));
    EXPECT_EQ ("sec", body.get_string ("secret"));
    EXPECT_EQ ("GB", body["country_codes"].as_array ()[0].as_string ());
    EXPECT_EQ ("transactions", body["products"].as_array ()[0].as_string ());
    EXPECT_EQ ("user-1", body["user"].get_string ("client_user_id"));
    EXPECT_EQ ("https://example.invalid/done",
               body["hosted_link"].get_string ("completion_redirect_uri"));
}

TEST (Plaid, update_mode_link_token_omits_products)
{
    FakeTransport t;
    t.reply ("/link/token/create", 200, R"({"link_token":"lt","hosted_link_url":"u"})");
    make_client (t).create_link_token ("user-1", "", "access-1");
    auto body = json::parse (t.requests[0].body);
    EXPECT_EQ ("access-1", body.get_string ("access_token"));
    EXPECT_FALSE (body.find ("products"));
}

TEST (Plaid, finds_public_tokens_from_finished_link_sessions)
{
    FakeTransport t;
    t.reply ("/link/token/get", 200, R"({"link_sessions":[
        {"link_session_id":"s1","results":{"item_add_results":[{"public_token":"public-1"}]}},
        {"link_session_id":"s2","on_success":{"public_token":"public-2"}},
        {"link_session_id":"s3","exit":{"error":null}}]})");
    auto tokens = make_client (t).link_public_tokens ("lt");
    ASSERT_EQ (2u, tokens.size ());
    EXPECT_EQ ("public-1", tokens[0]);
    EXPECT_EQ ("public-2", tokens[1]);
}

TEST (Plaid, sync_follows_pages_and_returns_final_cursor)
{
    FakeTransport t;
    t.reply ("/transactions/sync", 200, R"({"added":[
        {"transaction_id":"t1","account_id":"a1","amount":12.5,"iso_currency_code":"GBP","date":"2026-10-01","name":"TESCO STORES 3297","merchant_name":"Tesco","pending":false,"personal_finance_category":{"primary":"FOOD_AND_DRINK"}}],
        "modified":[],"removed":[],"next_cursor":"c1","has_more":true,"transactions_update_status":"HISTORICAL_UPDATE_COMPLETE"})");
    t.reply ("/transactions/sync", 200, R"({"added":[
        {"transaction_id":"t2","account_id":"a1","amount":-2500,"iso_currency_code":"GBP","date":"2026-10-02","name":"ACME LTD SALARY","merchant_name":null,"pending":false}],
        "modified":[{"transaction_id":"t0","account_id":"a1","amount":3,"date":"2026-09-30","name":"x"}],
        "removed":[{"transaction_id":"t-old"}],"next_cursor":"c2","has_more":false})");

    auto r = make_client (t).sync ("access", "");
    ASSERT_EQ (2u, r.added.size ());
    EXPECT_EQ ("12.5", r.added[0].amount);
    EXPECT_EQ ("Tesco", r.added[0].merchant_name);
    EXPECT_EQ ("FOOD_AND_DRINK", r.added[0].category);
    EXPECT_EQ ("-2500", r.added[1].amount);
    EXPECT_EQ ("", r.added[1].merchant_name);
    ASSERT_EQ (1u, r.modified.size ());
    ASSERT_EQ (1u, r.removed.size ());
    EXPECT_EQ ("t-old", r.removed[0]);
    EXPECT_EQ ("c2", r.next_cursor);

    EXPECT_FALSE (json::parse (t.requests[0].body).find ("cursor"));
    EXPECT_EQ ("c1", json::parse (t.requests[1].body).get_string ("cursor"));
}

TEST (Plaid, sync_restarts_after_mutation_during_pagination)
{
    FakeTransport t;
    t.reply ("/transactions/sync", 200, R"({"added":[{"transaction_id":"t1","account_id":"a","amount":1,"date":"2026-10-01","name":"n"}],"modified":[],"removed":[],"next_cursor":"p1","has_more":true})");
    t.reply ("/transactions/sync", 400, R"({"error_type":"TRANSACTIONS_ERROR","error_code":"TRANSACTIONS_SYNC_MUTATION_DURING_PAGINATION","error_message":"Underlying transaction data changed"})");
    t.reply ("/transactions/sync", 200, R"({"added":[{"transaction_id":"t1","account_id":"a","amount":1,"date":"2026-10-01","name":"n"}],"modified":[],"removed":[],"next_cursor":"final","has_more":false})");
    auto r = make_client (t).sync ("access", "start");
    EXPECT_EQ (1u, r.added.size ()); // the aborted page is discarded
    EXPECT_EQ ("final", r.next_cursor);
    EXPECT_EQ ("start", json::parse (t.requests[2].body).get_string ("cursor"));
}

TEST (Plaid, errors_prefer_the_display_message)
{
    FakeTransport t;
    t.reply ("/accounts/get", 400, R"({"error_type":"ITEM_ERROR","error_code":"ITEM_LOGIN_REQUIRED","error_message":"the login details of this item have changed","display_message":null})");
    try
    {
        make_client (t).accounts ("access");
        FAIL ();
    }
    catch (const online::OnlineError& e)
    {
        EXPECT_EQ ("ITEM_LOGIN_REQUIRED", e.code ());
        EXPECT_NE (std::string::npos, std::string (e.what ()).find ("Reconnect"));
    }
}

TEST (Plaid, accounts_keep_balances_as_exact_text)
{
    FakeTransport t;
    t.reply ("/accounts/get", 200, R"({"accounts":[{"account_id":"a1","name":"Current","mask":"4321","type":"depository","subtype":"checking","balances":{"current":1043.27,"available":null,"iso_currency_code":"GBP"}}]})");
    auto accts = make_client (t).accounts ("access");
    ASSERT_EQ (1u, accts.size ());
    EXPECT_EQ ("1043.27", accts[0].current_balance);
    EXPECT_EQ ("", accts[0].available_balance);
    EXPECT_EQ ("GBP", accts[0].currency);
}

TEST (Plaid, production_environment_uses_production_host)
{
    Config c;
    c.environment = "production";
    EXPECT_EQ ("https://production.plaid.com", c.base_url ());
    auto round = Config::from_json (c.to_json ());
    EXPECT_EQ ("production", round.environment);
    EXPECT_EQ (std::vector<std::string>{"GB"}, round.country_codes);
}
