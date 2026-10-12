/********************************************************************\
 * gnc-vat-return.hpp -- compute a UK VAT return from the book      *
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
/** @file gnc-vat-return.hpp
 *  @brief From splits to the nine boxes.
 *
 *  Each box is fed by the accounts the user assigns to it (with their
 *  sub-accounts). The rules, stated plainly so they can be checked:
 *
 *  - Box 1 and Box 2 (VAT due) count credits as positive; Box 4 (VAT
 *    reclaimed) counts debits as positive. One account may be given to
 *    both Box 1 (or 2) and Box 4, for books that keep a single "VAT"
 *    account: a split then goes by what its transaction is about, so a
 *    customer's credit note reduces Box 1 and a supplier's reduces
 *    Box 4; otherwise credits feed Box 1 and debits Box 4.
 *  - Box 6 and Box 8 (sales) count credits; Box 7 and Box 9
 *    (purchases) count debits. Box 6 includes Box 8 and Box 7 includes
 *    Box 9, as HMRC requires. Boxes 6 to 9 are truncated to whole
 *    pounds.
 *  - Box 3 = Box 1 + Box 2; Box 5 = |Box 3 - Box 4|.
 *  - Transactions that settle VAT with HMRC are excluded: any that
 *    touch the settlement account, any journal GnuCash posted when
 *    filing, and any that move money only between output VAT accounts
 *    and bank, cash or credit accounts. Excluded transactions are listed,
 *    never silently dropped.
 *  - Under the Flat Rate Scheme, Box 6 is gross turnover (net sales
 *    plus any output VAT recorded) and Box 1 is that turnover times
 *    the flat rate, rounded to the penny.
 *
 *  Accounting is on the accruals basis: a transaction counts on its
 *  posted date.
 */
#ifndef GNC_VAT_RETURN_HPP
#define GNC_VAT_RETURN_HPP

#include "gnc-hmrc.hpp"
#include "gnc-json.hpp"

#include <Account.h>
#include <gnc-numeric.h>
#include <qofbook.h>

#include <array>
#include <map>
#include <string>
#include <vector>

namespace gnc::vat
{

enum class Role
{
    Box1, ///< VAT due on sales and other outputs
    Box2, ///< VAT due on acquisitions (Northern Ireland, from the EU)
    Box4, ///< VAT reclaimed on purchases and other inputs
    Box6, ///< Total value of sales, excluding VAT
    Box7, ///< Total value of purchases, excluding VAT
    Box8, ///< Goods supplied to the EU (Northern Ireland)
    Box9, ///< Goods acquired from the EU (Northern Ireland)
    Settlement, ///< Where VAT owed to or by HMRC is settled
};

constexpr std::array<Role, 8> all_roles = {Role::Box1, Role::Box2, Role::Box4,
                                           Role::Box6, Role::Box7, Role::Box8,
                                           Role::Box9, Role::Settlement};

const char* role_key (Role r);
const char* role_label (Role r);
const char* role_hint (Role r);

/** Book-level settings, stored as "online/mtd-vat". */
struct Settings
{
    std::string vrn;
    bool sandbox = true;
    std::map<Role, std::vector<std::string>> accounts; ///< account GUIDs
    std::string flat_rate; ///< e.g. "12.5"; empty when not on the scheme

    json::Value to_json () const;
    static Settings from_json (const json::Value&);
    static Settings load (QofBook* book);
    void save (QofBook* book) const;

    /** Roles held by @a acc directly (not through a parent). */
    std::vector<Role> roles_for (const Account* acc) const;
    void set_roles (const Account* acc, const std::vector<Role>& roles);

    bool vrn_valid () const;
};

struct Line
{
    std::string trans_guid;
    time64 date = 0;
    std::string description;
    std::string account;
    gnc_numeric amount = gnc_numeric_zero (); ///< As it contributes to the box.
};

struct Computed
{
    /** Exact amounts, indexed by box number 1..9 (index 0 unused). */
    std::array<gnc_numeric, 10> box;
    /** Contributions to boxes 1, 2, 4, 6, 7, 8 and 9. */
    std::array<std::vector<Line>, 10> lines;
    /** Per-account contribution to boxes 1, 2 and 4 (for the journal). */
    std::map<std::string, gnc_numeric> vat_by_account[10];
    std::vector<Line> excluded;
    std::vector<std::string> warnings;

    /** True when HMRC owes the business. */
    bool repayment () const;
    /** Format as the API payload for @a period_key (finalised = false). */
    hmrc::ReturnPayload payload (const std::string& period_key) const;
};

Computed compute (QofBook* book, const Settings& settings,
                  time64 start, time64 end);

/** "YYYY-MM-DD" to the first or last second of that day. */
time64 parse_date (const std::string& ymd, bool end_of_day);

/** Format with two decimals, or as whole pounds truncated toward zero. */
std::string format_pence (gnc_numeric n);
std::string format_pounds (gnc_numeric n);

/** Filed returns, stored as "online/mtd-vat-filed". */
struct Filing
{
    std::string period_key, start, end;
    hmrc::ReturnPayload payload;
    hmrc::Receipt receipt;
    int64_t submitted_at = 0;
    std::string journal_guid;
};

std::vector<Filing> load_filings (QofBook* book);
void record_filing (QofBook* book, const Filing& f);

/** Post a journal on @a date that clears the period's VAT accounts into
 *  the settlement account. Returns the new transaction, or nullptr when
 *  no settlement account is set or nothing needs moving. */
Transaction* post_settlement_journal (QofBook* book, const Settings& settings,
                                      const Computed& c, const std::string& period_key,
                                      time64 date);

} // namespace gnc::vat

#endif /* GNC_VAT_RETURN_HPP */
