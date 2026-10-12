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

#include "gnc-bank-feeds.hpp"
#include "gnc-online-book.hpp"
#include "uk-book.hpp"

using namespace gnc;
using namespace gnc::feeds;

class BankFeeds : public UkBook
{
protected:
    void SetUp () override
    {
        UkBook::SetUp ();
        conn.item_id = "item-1";
        conn.institution = "Royal Bank of Plaid";
        conn.accounts = {
            {"acc-current", "Current", "4321", "checking", "GBP",
             online::account_guid_string (bank), false, "1043.27"},
            {"acc-card", "Card", "9999", "credit card", "GBP",
             online::account_guid_string (card), false, ""},
            {"acc-isa", "ISA", "0001", "savings", "GBP", "", true, ""},
            {"acc-new", "New", "0002", "savings", "GBP", "", false, ""},
        };
    }

    plaid::Transaction txn (const char* id, const char* account, const char* amount)
    {
        plaid::Transaction t;
        t.transaction_id = id;
        t.account_id = account;
        t.amount = amount;
        t.currency = "GBP";
        t.date = "2026-10-01";
        t.name = "TESCO STORES 3297 LONDON";
        t.merchant_name = "Tesco";
        t.category = "FOOD_AND_DRINK";
        return t;
    }

    Connection conn;
};

TEST_F (BankFeeds, outflows_become_credits_with_an_online_id)
{
    Skip why;
    auto t = build_transaction (book, conn, txn ("tx-1", "acc-current", "12.5"), false, why);
    ASSERT_NE (nullptr, t);
    EXPECT_EQ (Skip::None, why);
    EXPECT_STREQ ("Tesco", xaccTransGetDescription (t));
    EXPECT_STREQ ("FOOD_AND_DRINK", xaccTransGetNotes (t));
    ASSERT_EQ (1, xaccTransCountSplits (t));
    auto s = xaccTransGetSplit (t, 0);
    EXPECT_EQ (bank, xaccSplitGetAccount (s));
    EXPECT_TRUE (gnc_numeric_equal (gnc_numeric_create (-1250, 100), xaccSplitGetAmount (s)));
    EXPECT_STREQ ("tx-1", xaccSplitGetOnlineID (s));
    EXPECT_STREQ ("TESCO STORES 3297 LONDON", xaccSplitGetMemo (s));
    EXPECT_TRUE (xaccTransIsOpen (t)); // the matcher commits it
    xaccTransDestroy (t);
    xaccTransCommitEdit (t);
}

TEST_F (BankFeeds, inflows_become_debits)
{
    Skip why;
    auto t = build_transaction (book, conn, txn ("tx-2", "acc-current", "-2500"), false, why);
    ASSERT_NE (nullptr, t);
    EXPECT_TRUE (gnc_numeric_equal (gnc_numeric_create (250000, 100),
                                    xaccSplitGetAmount (xaccTransGetSplit (t, 0))));
    xaccTransDestroy (t);
    xaccTransCommitEdit (t);
}

TEST_F (BankFeeds, rows_that_cannot_be_imported_say_why)
{
    Skip why;
    auto pending = txn ("p", "acc-current", "1");
    pending.pending = true;
    EXPECT_EQ (nullptr, build_transaction (book, conn, pending, false, why));
    EXPECT_EQ (Skip::Pending, why);

    EXPECT_EQ (nullptr, build_transaction (book, conn, txn ("i", "acc-isa", "1"), false, why));
    EXPECT_EQ (Skip::Ignored, why);

    EXPECT_EQ (nullptr, build_transaction (book, conn, txn ("n", "acc-new", "1"), false, why));
    EXPECT_EQ (Skip::Unmapped, why);

    EXPECT_EQ (nullptr, build_transaction (book, conn, txn ("u", "acc-unknown", "1"), false, why));
    EXPECT_EQ (Skip::Unmapped, why);

    auto dollars = txn ("d", "acc-current", "1");
    dollars.currency = "USD";
    EXPECT_EQ (nullptr, build_transaction (book, conn, dollars, false, why));
    EXPECT_EQ (Skip::Currency, why);

    auto junk = txn ("j", "acc-current", "twelve");
    EXPECT_EQ (nullptr, build_transaction (book, conn, junk, false, why));
    EXPECT_EQ (Skip::BadData, why);
}

TEST_F (BankFeeds, pending_rows_import_when_asked)
{
    Skip why;
    auto pending = txn ("p", "acc-current", "1");
    pending.pending = true;
    auto t = build_transaction (book, conn, pending, true, why);
    ASSERT_NE (nullptr, t);
    xaccTransDestroy (t);
    xaccTransCommitEdit (t);
}

TEST_F (BankFeeds, finds_previously_imported_rows)
{
    Skip why;
    auto t = build_transaction (book, conn, txn ("tx-9", "acc-card", "40"), false, why);
    auto other = xaccMallocSplit (book);
    xaccSplitSetParent (other, t);
    xaccSplitSetAccount (other, software);
    xaccSplitSetAmount (other, gnc_numeric_create (4000, 100));
    xaccSplitSetValue (other, gnc_numeric_create (4000, 100));
    xaccTransCommitEdit (t);

    auto found = find_imported_split (book, conn, "tx-9");
    ASSERT_NE (nullptr, found);
    EXPECT_EQ (card, xaccSplitGetAccount (found));
    EXPECT_EQ (nullptr, find_imported_split (book, conn, "tx-nope"));
}

TEST_F (BankFeeds, store_round_trips_and_merges_accounts)
{
    EXPECT_FALSE (conn.ready ());
    conn.find_account ("acc-new")->ignore = true;
    EXPECT_TRUE (conn.ready ());

    Store store;
    conn.cursor = "cursor-7";
    conn.last_sync = 1790000000;
    store.connections.push_back (conn);
    store.save (book);

    auto loaded = Store::load (book);
    ASSERT_EQ (1u, loaded.connections.size ());
    auto& c = loaded.connections[0];
    EXPECT_EQ ("cursor-7", c.cursor);
    EXPECT_EQ (1790000000, c.last_sync);
    EXPECT_EQ (online::account_guid_string (bank), c.find_account ("acc-current")->gnc_account);

    // A fresh list from the bank: one account closed, one opened.
    plaid::Account current{"acc-current", "Current Account", "", "4321", "depository",
                           "checking", "GBP", "999.00", ""};
    plaid::Account fresh{"acc-fresh", "Bills", "", "5555", "depository", "savings",
                         "GBP", "10.00", ""};
    merge_accounts (c, {current, fresh});
    ASSERT_EQ (2u, c.accounts.size ());
    EXPECT_EQ (online::account_guid_string (bank), c.accounts[0].gnc_account);
    EXPECT_EQ ("999.00", c.accounts[0].bank_balance);
    EXPECT_EQ ("", c.accounts[1].gnc_account);
    EXPECT_FALSE (c.ready ());

    loaded.remove ("item-1");
    EXPECT_TRUE (loaded.connections.empty ());
}

TEST_F (BankFeeds, credentials_live_in_the_secret_store_not_the_book)
{
    online::MemorySecretStore secrets;
    plaid::Config cfg;
    cfg.client_id = "cid";
    cfg.secret = "s3cret";
    save_plaid_config (secrets, cfg);
    save_access_token (secrets, conn, "access-sandbox-abc");

    Store store;
    store.connections.push_back (conn);
    store.save (book);
    auto doc = online::load_book_json (book, "bank-feeds").dump ();
    EXPECT_EQ (std::string::npos, doc.find ("access-sandbox-abc"));
    EXPECT_EQ (std::string::npos, doc.find ("s3cret"));

    EXPECT_EQ ("s3cret", load_plaid_config (secrets).secret);
    EXPECT_EQ ("access-sandbox-abc", *load_access_token (secrets, conn));
    erase_access_token (secrets, conn);
    EXPECT_FALSE (load_access_token (secrets, conn));
}
