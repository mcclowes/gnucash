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
/* A small UK sole-trader book for the online tests. */
#ifndef GNC_ONLINE_UK_BOOK_HPP
#define GNC_ONLINE_UK_BOOK_HPP

#include <Account.h>
#include <Split.h>
#include <Transaction.h>
#include <cashobjects.h>
#include <gnc-commodity.h>
#include <gnc-date.h>
#include <qof.h>

#include <gtest/gtest.h>

#include <utility>
#include <vector>

class UkBook : public testing::Test
{
protected:
    static void SetUpTestSuite ()
    {
        static bool initialised = false;
        if (!initialised)
        {
            qof_init ();
            cashobjects_register ();
            initialised = true;
        }
    }

    void SetUp () override
    {
        book = qof_book_new ();
        auto table = gnc_commodity_table_get_table (book);
        gbp = gnc_commodity_new (book, "British Pound", GNC_COMMODITY_NS_CURRENCY,
                                 "GBP", "826", 100);
        gbp = gnc_commodity_table_insert (table, gbp);
        usd = gnc_commodity_new (book, "US Dollar", GNC_COMMODITY_NS_CURRENCY,
                                 "USD", "840", 100);
        usd = gnc_commodity_table_insert (table, usd);

        root = gnc_account_create_root (book);
        assets = add (root, "Assets", ACCT_TYPE_ASSET);
        bank = add (assets, "Current Account", ACCT_TYPE_BANK);
        receivable = add (assets, "Accounts Receivable", ACCT_TYPE_RECEIVABLE);
        liabilities = add (root, "Liabilities", ACCT_TYPE_LIABILITY);
        vat = add (liabilities, "VAT", ACCT_TYPE_LIABILITY);
        vat_output = add (vat, "Output", ACCT_TYPE_LIABILITY);
        vat_input = add (vat, "Input", ACCT_TYPE_LIABILITY);
        hmrc = add (liabilities, "HMRC", ACCT_TYPE_LIABILITY);
        card = add (liabilities, "Credit Card", ACCT_TYPE_CREDIT);
        income = add (root, "Income", ACCT_TYPE_INCOME);
        sales = add (income, "Consulting", ACCT_TYPE_INCOME);
        interest = add (income, "Interest", ACCT_TYPE_INCOME);
        expenses = add (root, "Expenses", ACCT_TYPE_EXPENSE);
        software = add (expenses, "Software", ACCT_TYPE_EXPENSE);
        travel = add (expenses, "Travel", ACCT_TYPE_EXPENSE);
    }

    void TearDown () override
    {
        qof_book_destroy (book);
    }

    Account* add (Account* parent, const char* name, GNCAccountType type,
                  gnc_commodity* comm = nullptr)
    {
        auto acc = xaccMallocAccount (book);
        xaccAccountBeginEdit (acc);
        xaccAccountSetName (acc, name);
        xaccAccountSetType (acc, type);
        xaccAccountSetCommodity (acc, comm ? comm : gbp);
        gnc_account_append_child (parent, acc);
        xaccAccountCommitEdit (acc);
        return acc;
    }

    /** Post a transaction; amounts in pence, debits positive. */
    Transaction* post (int y, int m, int d, const char* desc,
                       std::vector<std::pair<Account*, int64_t>> splits)
    {
        auto t = xaccMallocTransaction (book);
        xaccTransBeginEdit (t);
        xaccTransSetCurrency (t, gbp);
        xaccTransSetDatePostedSecsNormalized (t, gnc_dmy2time64_neutral (d, m, y));
        xaccTransSetDescription (t, desc);
        for (auto [acc, pence] : splits)
        {
            auto s = xaccMallocSplit (book);
            xaccSplitSetParent (s, t);
            xaccSplitSetAccount (s, acc);
            auto v = gnc_numeric_create (pence, 100);
            xaccSplitSetAmount (s, v);
            xaccSplitSetValue (s, v);
        }
        xaccTransCommitEdit (t);
        return t;
    }

    QofBook* book = nullptr;
    gnc_commodity *gbp = nullptr, *usd = nullptr;
    Account *root, *assets, *bank, *receivable, *liabilities, *vat, *vat_output,
            *vat_input, *hmrc, *card, *income, *sales, *interest, *expenses,
            *software, *travel;
};

#endif
