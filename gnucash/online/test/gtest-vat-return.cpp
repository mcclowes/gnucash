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

#include "gnc-vat-return.hpp"
#include "gnc-online-book.hpp"
#include "uk-book.hpp"

using namespace gnc;
using namespace gnc::vat;

class VatReturn : public UkBook
{
protected:
    void SetUp () override
    {
        UkBook::SetUp ();
        settings.vrn = "123456789";
        settings.set_roles (vat_output, {Role::Box1});
        settings.set_roles (vat_input, {Role::Box4});
        settings.set_roles (income, {Role::Box6});
        settings.set_roles (expenses, {Role::Box7});
        settings.set_roles (hmrc, {Role::Settlement});

        // Q2 2026: invoice, payment, purchases, a refund-style credit note.
        post (2026, 4, 3, "Invoice 101", {{receivable, 1200000}, {sales, -1000000}, {vat_output, -200000}});
        post (2026, 4, 20, "Payment for 101", {{bank, 1200000}, {receivable, -1200000}});
        post (2026, 5, 2, "Laptop software", {{software, 49999}, {vat_input, 10000}, {card, -59999}});
        post (2026, 5, 9, "Train to Leeds", {{travel, 8333}, {vat_input, 1667}, {bank, -10000}});
        post (2026, 6, 1, "Credit note 101a", {{sales, 10000}, {vat_output, 2000}, {receivable, -12000}});
        post (2026, 6, 30, "Bank interest", {{bank, 123}, {interest, -123}});
        // Outside the period on both sides.
        post (2026, 3, 31, "Invoice 100", {{receivable, 600000}, {sales, -500000}, {vat_output, -100000}});
        post (2026, 7, 1, "Invoice 102", {{receivable, 600000}, {sales, -500000}, {vat_output, -100000}});
        // Paying the previous quarter's VAT: excluded via the settlement account.
        post (2026, 5, 7, "HMRC VAT Q1", {{hmrc, 100000}, {bank, -100000}});
    }

    Computed run ()
    {
        return compute (book, settings, parse_date ("2026-04-01", false),
                        parse_date ("2026-06-30", true));
    }

    Settings settings;
};

TEST_F (VatReturn, boxes_follow_the_assigned_accounts)
{
    auto c = run ();
    EXPECT_EQ ("1980.00", format_pence (c.box[1]));   // 2000 - 20
    EXPECT_EQ ("0.00", format_pence (c.box[2]));
    EXPECT_EQ ("1980.00", format_pence (c.box[3]));
    EXPECT_EQ ("116.67", format_pence (c.box[4]));     // 100 + 16.67
    EXPECT_EQ ("1863.33", format_pence (c.box[5]));
    EXPECT_EQ ("9901", format_pounds (c.box[6]));      // 10000 - 100 + 1.23, truncated
    EXPECT_EQ ("583", format_pounds (c.box[7]));       // 499.99 + 83.33, truncated
    EXPECT_EQ ("0", format_pounds (c.box[8]));
    EXPECT_FALSE (c.repayment ());
    EXPECT_EQ (2u, c.lines[1].size ());
    EXPECT_EQ (2u, c.lines[4].size ());

    auto p = c.payload ("26A2");
    EXPECT_EQ ("", p.validate ());
    EXPECT_EQ ("9901", p.total_value_sales_ex_vat);
}

TEST_F (VatReturn, settlement_and_bare_vat_payments_are_excluded_and_listed)
{
    // A VAT payment made straight from the bank to the output VAT account.
    post (2026, 6, 15, "VAT payment", {{vat_output, 50000}, {bank, -50000}});
    auto c = run ();
    EXPECT_EQ ("1980.00", format_pence (c.box[1]));
    ASSERT_EQ (1u, c.excluded.size ());
    EXPECT_EQ ("VAT payment", c.excluded[0].description);
    EXPECT_FALSE (c.warnings.empty ());
}

TEST_F (VatReturn, a_single_vat_account_splits_credits_and_debits)
{
    Settings single = settings;
    single.set_roles (vat_output, {});
    single.set_roles (vat_input, {});
    single.set_roles (vat, {Role::Box1, Role::Box4});
    auto c = compute (book, single, parse_date ("2026-04-01", false),
                      parse_date ("2026-06-30", true));
    // Debits (the credit note and the purchases) all land in Box 4 now.
    EXPECT_EQ ("2000.00", format_pence (c.box[1]));
    EXPECT_EQ ("136.67", format_pence (c.box[4]));
    EXPECT_EQ ("1863.33", format_pence (c.box[5]));
}

TEST_F (VatReturn, repayment_when_input_exceeds_output)
{
    post (2026, 6, 20, "Van", {{expenses, 3000000}, {vat_input, 600000}, {bank, -3600000}});
    auto c = run ();
    EXPECT_TRUE (c.repayment ());
    EXPECT_EQ ("6116.67", format_pence (c.box[4]));
    EXPECT_EQ ("4136.67", format_pence (c.box[5]));
    EXPECT_EQ ("", c.payload ("26A2").validate ());
}

TEST_F (VatReturn, flat_rate_scheme_uses_gross_turnover)
{
    settings.flat_rate = "12.5";
    auto c = run ();
    // Gross = net sales 9901.23 + output VAT 1980.00 = 11881.23
    EXPECT_EQ ("11881", format_pounds (c.box[6]));
    EXPECT_EQ ("1485.15", format_pence (c.box[1])); // 11881.23 * 12.5% = 1485.15375
}

TEST_F (VatReturn, settings_round_trip_through_the_book)
{
    settings.flat_rate = "9";
    settings.save (book);
    auto loaded = Settings::load (book);
    EXPECT_EQ ("123456789", loaded.vrn);
    EXPECT_EQ ("9", loaded.flat_rate);
    EXPECT_TRUE (loaded.vrn_valid ());
    ASSERT_EQ (1u, loaded.roles_for (vat_output).size ());
    EXPECT_EQ (Role::Box1, loaded.roles_for (vat_output)[0]);
    EXPECT_EQ (Role::Settlement, loaded.roles_for (hmrc)[0]);
    EXPECT_TRUE (loaded.roles_for (bank).empty ());
}

TEST_F (VatReturn, journal_clears_vat_into_settlement_and_is_then_ignored)
{
    auto c = run ();
    auto journal = post_settlement_journal (book, settings, c, "26A2",
                                            parse_date ("2026-06-30", true));
    ASSERT_NE (nullptr, journal);
    EXPECT_TRUE (xaccTransIsBalanced (journal));

    auto end = parse_date ("2026-06-30", true);
    auto start = parse_date ("2026-04-01", false);
    // The VAT accounts' movement for the quarter is now nil...
    EXPECT_TRUE (gnc_numeric_zero_p (xaccAccountGetBalanceChangeForPeriod (vat_output, start, end, false)));
    EXPECT_TRUE (gnc_numeric_zero_p (xaccAccountGetBalanceChangeForPeriod (vat_input, start, end, false)));
    // ...the net is owed to HMRC (a credit on the liability, after the
    // £1,000 paid for the previous quarter)...
    EXPECT_EQ ("-863.33", format_pence (xaccAccountGetBalanceChangeForPeriod (hmrc, start, end, false)));
    // ...and recomputing the return gives the same answer.
    auto again = run ();
    EXPECT_EQ ("1980.00", format_pence (again.box[1]));
    EXPECT_EQ ("116.67", format_pence (again.box[4]));
}

TEST_F (VatReturn, filings_are_recorded_in_the_book)
{
    Filing f;
    f.period_key = "26A2";
    f.start = "2026-04-01";
    f.end = "2026-06-30";
    f.payload = run ().payload ("26A2");
    f.payload.finalised = true;
    f.receipt.form_bundle_number = "256660290587";
    f.submitted_at = 1790000000;
    record_filing (book, f);
    record_filing (book, f);
    auto all = load_filings (book);
    ASSERT_EQ (2u, all.size ());
    EXPECT_EQ ("256660290587", all[0].receipt.form_bundle_number);
    EXPECT_EQ ("1863.33", all[1].payload.net_vat_due);
    EXPECT_EQ (1790000000, all[1].submitted_at);
}

TEST_F (VatReturn, warns_about_foreign_currency_and_contradictory_roles)
{
    auto usd_sales = add (income, "US Sales", ACCT_TYPE_INCOME, usd);
    (void) usd_sales;
    settings.set_roles (software, {Role::Box6});
    auto c = run ();
    bool currency = false, contradictory = false;
    for (const auto& w : c.warnings)
    {
        currency = currency || w.find ("US Sales") != std::string::npos;
        contradictory = contradictory || w.find ("Software") != std::string::npos;
    }
    EXPECT_TRUE (currency);
    EXPECT_TRUE (contradictory);
}

TEST (VatFormat, rounding_rules)
{
    EXPECT_EQ ("0.00", format_pence (gnc_numeric_zero ()));
    EXPECT_EQ ("-12.35", format_pence (gnc_numeric_create (-12345, 1000)));
    EXPECT_EQ ("1.01", format_pence (gnc_numeric_create (1005, 1000)));
    EXPECT_EQ ("12", format_pounds (gnc_numeric_create (1299, 100)));
    EXPECT_EQ ("-12", format_pounds (gnc_numeric_create (-1299, 100)));
}
