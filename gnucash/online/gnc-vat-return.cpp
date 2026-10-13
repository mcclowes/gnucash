/********************************************************************\
 * gnc-vat-return.cpp -- compute a UK VAT return from the book      *
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

#include "gnc-vat-return.hpp"
#include "gnc-online-book.hpp"

#include <glib/gi18n.h>
#include <Account.hpp>
#include <Transaction.h>
#include <Split.h>
#include <gnc-commodity.h>
#include <gnc-date.h>
#include <qofinstance-p.h>

#include <algorithm>
#include <cinttypes>
#include <cctype>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace gnc::vat
{

using online::account_guid_string;
using online::account_from_guid_string;

static const char* JOURNAL_SLOT = "mtd-vat-journal";

const char*
role_key (Role r)
{
    switch (r)
    {
    case Role::Box1: return "box1";
    case Role::Box2: return "box2";
    case Role::Box4: return "box4";
    case Role::Box6: return "box6";
    case Role::Box7: return "box7";
    case Role::Box8: return "box8";
    case Role::Box9: return "box9";
    case Role::Settlement: return "settlement";
    }
    return "";
}

const char*
role_label (Role r)
{
    switch (r)
    {
    case Role::Box1: return _("Box 1 · VAT on sales");
    case Role::Box2: return _("Box 2 · VAT on EU acquisitions");
    case Role::Box4: return _("Box 4 · VAT reclaimed");
    case Role::Box6: return _("Box 6 · Sales");
    case Role::Box7: return _("Box 7 · Purchases");
    case Role::Box8: return _("Box 8 · EU supplies");
    case Role::Box9: return _("Box 9 · EU acquisitions");
    case Role::Settlement: return _("Settlement with HMRC");
    }
    return "";
}

const char*
role_hint (Role r)
{
    switch (r)
    {
    case Role::Box1: return _("Output VAT you charged customers.");
    case Role::Box2: return _("Northern Ireland only: VAT due on goods from the EU.");
    case Role::Box4: return _("Input VAT you paid suppliers.");
    case Role::Box6: return _("Income accounts, net of VAT.");
    case Role::Box7: return _("Expense and asset accounts, net of VAT.");
    case Role::Box8: return _("Northern Ireland only: goods supplied to the EU. Also counted in Box 6.");
    case Role::Box9: return _("Northern Ireland only: goods acquired from the EU. Also counted in Box 7.");
    case Role::Settlement: return _("Where you pay HMRC or receive refunds.");
    }
    return "";
}

static std::optional<Role>
role_from_key (const std::string& k)
{
    for (auto r : all_roles)
        if (k == role_key (r))
            return r;
    return std::nullopt;
}

/* ---------------------------------------------------------------- */
/* Settings                                                          */

json::Value
Settings::to_json () const
{
    auto v = json::Value::object ();
    v.set ("vrn", vrn);
    v.set ("sandbox", sandbox);
    v.set ("flat_rate", flat_rate);
    auto roles = json::Value::object ();
    for (const auto& [role, guids] : accounts)
    {
        if (guids.empty ())
            continue;
        auto arr = json::Value::array ();
        for (const auto& g : guids)
            arr.push (g);
        roles.set (role_key (role), arr);
    }
    v.set ("accounts", roles);
    return v;
}

Settings
Settings::from_json (const json::Value& v)
{
    Settings s;
    s.vrn = v.get_string ("vrn");
    s.sandbox = v.get_bool ("sandbox", true);
    s.flat_rate = v.get_string ("flat_rate");
    if (auto roles = v.find ("accounts"); roles && roles->is_object ())
        for (const auto& [k, arr] : roles->as_object ())
        {
            auto role = role_from_key (k);
            if (!role || !arr.is_array ())
                continue;
            for (const auto& g : arr.as_array ())
                if (g.is_string ())
                    s.accounts[*role].push_back (g.as_string ());
        }
    return s;
}

Settings
Settings::load (QofBook* book)
{
    return from_json (online::load_book_json (book, "mtd-vat"));
}

void
Settings::save (QofBook* book) const
{
    online::save_book_json (book, "mtd-vat", to_json ());
}

std::vector<Role>
Settings::roles_for (const Account* acc) const
{
    std::vector<Role> out;
    auto guid = account_guid_string (acc);
    for (const auto& [role, guids] : accounts)
        if (std::find (guids.begin (), guids.end (), guid) != guids.end ())
            out.push_back (role);
    return out;
}

void
Settings::set_roles (const Account* acc, const std::vector<Role>& roles)
{
    auto guid = account_guid_string (acc);
    for (auto& [role, guids] : accounts)
        guids.erase (std::remove (guids.begin (), guids.end (), guid), guids.end ());
    for (auto role : roles)
    {
        // The settlement account is singular.
        if (role == Role::Settlement)
            accounts[role].clear ();
        accounts[role].push_back (guid);
    }
}

bool
Settings::vrn_valid () const
{
    return vrn.size () == 9 &&
        std::all_of (vrn.begin (), vrn.end (),
                     [](unsigned char c) { return std::isdigit (c); });
}

/* ---------------------------------------------------------------- */
/* Formatting                                                        */

static gnc_numeric
to_pence (gnc_numeric n)
{
    return gnc_numeric_convert (n, 100, GNC_HOW_RND_ROUND_HALF_UP);
}

std::string
format_pence (gnc_numeric n)
{
    n = to_pence (n);
    int64_t v = n.num;
    bool neg = v < 0;
    if (neg) v = -v;
    char buf[48];
    snprintf (buf, sizeof buf, "%s%" PRId64 ".%02" PRId64, neg ? "-" : "",
              v / 100, v % 100);
    return buf;
}

std::string
format_pounds (gnc_numeric n)
{
    n = gnc_numeric_convert (n, 1, GNC_HOW_RND_TRUNC);
    return std::to_string (n.num);
}

time64
parse_date (const std::string& ymd, bool end_of_day)
{
    int y = 0, m = 0, d = 0;
    if (sscanf (ymd.c_str (), "%d-%d-%d", &y, &m, &d) != 3)
        return 0;
    return end_of_day ? gnc_dmy2time64_end (d, m, y) : gnc_dmy2time64 (d, m, y);
}

/* ---------------------------------------------------------------- */
/* Computation                                                       */

bool
Computed::repayment () const
{
    return gnc_numeric_compare (box[4], box[3]) > 0;
}

hmrc::ReturnPayload
Computed::payload (const std::string& period_key) const
{
    hmrc::ReturnPayload p;
    p.period_key = period_key;
    p.vat_due_sales = format_pence (box[1]);
    p.vat_due_acquisitions = format_pence (box[2]);
    p.total_vat_due = format_pence (box[3]);
    p.vat_reclaimed_curr_period = format_pence (box[4]);
    p.net_vat_due = format_pence (box[5]);
    p.total_value_sales_ex_vat = format_pounds (box[6]);
    p.total_value_purchases_ex_vat = format_pounds (box[7]);
    p.total_value_goods_supplied_ex_vat = format_pounds (box[8]);
    p.total_acquisitions_ex_vat = format_pounds (box[9]);
    p.finalised = false;
    return p;
}

static int
box_number (Role r)
{
    switch (r)
    {
    case Role::Box1: return 1;
    case Role::Box2: return 2;
    case Role::Box4: return 4;
    case Role::Box6: return 6;
    case Role::Box7: return 7;
    case Role::Box8: return 8;
    case Role::Box9: return 9;
    default: return 0;
    }
}

static bool
is_vat_role (Role r)
{
    return r == Role::Box1 || r == Role::Box2 || r == Role::Box4;
}

static bool
is_money_account (const Account* acc)
{
    switch (xaccAccountGetType (acc))
    {
    case ACCT_TYPE_BANK:
    case ACCT_TYPE_CASH:
    case ACCT_TYPE_CREDIT:
        return true;
    default:
        return false;
    }
}

static bool
is_pl_account (const Account* acc)
{
    auto t = xaccAccountGetType (acc);
    return t == ACCT_TYPE_INCOME || t == ACCT_TYPE_EXPENSE;
}

using RoleMap = std::unordered_map<const Account*, std::set<Role>>;

static RoleMap
resolve_roles (QofBook* book, const Settings& settings, std::vector<std::string>& warnings)
{
    RoleMap map;
    for (const auto& [role, guids] : settings.accounts)
        for (const auto& g : guids)
        {
            auto acc = account_from_guid_string (book, g);
            if (!acc)
            {
                warnings.push_back (_("An account assigned to a VAT box no longer exists."));
                continue;
            }
            map[acc].insert (role);
            auto desc = gnc_account_get_descendants (acc);
            for (auto n = desc; n; n = n->next)
                map[static_cast<Account*> (n->data)].insert (role);
            g_list_free (desc);
        }

    for (const auto& [acc, roles] : map)
    {
        bool sales = roles.count (Role::Box6) || roles.count (Role::Box8);
        bool purchases = roles.count (Role::Box7) || roles.count (Role::Box9);
        auto name = gnc_account_get_full_name (acc);
        if (sales && purchases)
            warnings.push_back (std::string (name) +
                                _(": assigned to both a sales box and a purchases box."));
        auto comm = xaccAccountGetCommodity (acc);
        if (comm && g_strcmp0 (gnc_commodity_get_mnemonic (comm), "GBP") != 0 &&
            roles.size () && !roles.count (Role::Settlement))
            warnings.push_back (std::string (name) +
                                _(": not in pounds sterling; amounts are used unconverted."));
        g_free (name);
    }
    return map;
}

static std::string
trans_guid_string (const Transaction* t)
{
    char buf[GUID_ENCODING_LENGTH + 1];
    guid_to_string_buff (qof_entity_get_guid (QOF_INSTANCE (t)), buf);
    return buf;
}

static Line
make_line (const Transaction* t, const Account* acc, gnc_numeric amount)
{
    Line l;
    l.trans_guid = trans_guid_string (t);
    l.date = xaccTransGetDate (t);
    l.description = xaccTransGetDescription (t) ? xaccTransGetDescription (t) : "";
    auto name = gnc_account_get_full_name (acc);
    l.account = name;
    g_free (name);
    l.amount = amount;
    return l;
}

Computed
compute (QofBook* book, const Settings& settings, time64 start, time64 end)
{
    Computed c;
    c.box.fill (gnc_numeric_zero ());
    auto roles = resolve_roles (book, settings, c.warnings);

    // Gather every transaction in the period touching a mapped account.
    std::vector<Transaction*> txns;
    std::unordered_set<Transaction*> seen;
    for (const auto& [acc, r] : roles)
    {
        if (r.size () == 1 && r.count (Role::Settlement))
            continue;
        for (auto split : xaccAccountGetSplits (acc))
        {
            auto t = xaccSplitGetParent (split);
            auto when = xaccTransGetDate (t);
            if (when < start || when > end || seen.count (t))
                continue;
            seen.insert (t);
            txns.push_back (t);
        }
    }
    std::sort (txns.begin (), txns.end (), [](Transaction* a, Transaction* b) {
        return xaccTransGetDate (a) < xaccTransGetDate (b);
    });

    std::array<gnc_numeric, 10> raw;
    raw.fill (gnc_numeric_zero ());
    auto add = [&](int box, const Transaction* t, const Account* acc, gnc_numeric amt) {
        raw[box] = gnc_numeric_add_fixed (raw[box], amt);
        c.lines[box].push_back (make_line (t, acc, amt));
        if (box == 1 || box == 2 || box == 4)
        {
            auto [slot, inserted] = c.vat_by_account[box].try_emplace (
                account_guid_string (acc), gnc_numeric_zero ());
            slot->second = gnc_numeric_add_fixed (slot->second, amt);
        }
    };

    for (auto t : txns)
    {
        bool settles = false, has_vat = false, has_pl = false, only_money = true;
        bool vat_output_only = true;
        // Which way the earning or spending runs: a customer credit note
        // debits income; a supplier credit note credits an expense.
        bool sales_in = false, sales_out = false, spend_in = false, spend_out = false;
        auto journal = qof_instance_get_path_kvp<const char*> (QOF_INSTANCE (t),
                                                              {"online", JOURNAL_SLOT});
        for (auto n = xaccTransGetSplitList (t); n; n = n->next)
        {
            auto acc = xaccSplitGetAccount (static_cast<Split*> (n->data));
            auto it = roles.find (acc);
            bool vat_here = false, output_here = false;
            if (it != roles.end ())
            {
                for (auto r : it->second)
                {
                    if (r == Role::Settlement) settles = true;
                    if (is_vat_role (r)) vat_here = true;
                    if (r == Role::Box1 || r == Role::Box2) output_here = true;
                    if (box_number (r) >= 6) has_pl = true;
                }
            }
            has_vat = has_vat || vat_here;
            if (vat_here && !output_here)
                vat_output_only = false;
            bool sales_acc = xaccAccountGetType (acc) == ACCT_TYPE_INCOME ||
                (it != roles.end () && (it->second.count (Role::Box6) || it->second.count (Role::Box8)));
            bool spend_acc = xaccAccountGetType (acc) == ACCT_TYPE_EXPENSE ||
                (it != roles.end () && (it->second.count (Role::Box7) || it->second.count (Role::Box9)));
            bool debit = gnc_numeric_positive_p (xaccSplitGetAmount (static_cast<Split*> (n->data)));
            if (sales_acc)
                (debit ? sales_out : sales_in) = true;
            if (spend_acc)
                (debit ? spend_in : spend_out) = true;
            if (is_pl_account (acc))
                has_pl = true;
            if (!vat_here && !is_money_account (acc))
                only_money = false;
        }

        // Money moving between output VAT and the bank, with nothing
        // earned or spent, is a payment to (or refund from) HMRC. Input
        // VAT paid straight from the bank (import VAT, say) is not.
        bool payment = has_vat && vat_output_only && !has_pl && only_money;
        if (settles || payment || (journal && *journal))
        {
            auto first = xaccTransGetSplit (t, 0);
            c.excluded.push_back (make_line (t, xaccSplitGetAccount (first),
                                             xaccSplitGetAmount (first)));
            continue;
        }

        for (auto n = xaccTransGetSplitList (t); n; n = n->next)
        {
            auto split = static_cast<Split*> (n->data);
            auto acc = xaccSplitGetAccount (split);
            auto it = roles.find (acc);
            if (it == roles.end ())
                continue;
            const auto& r = it->second;
            auto amount = xaccSplitGetAmount (split);
            auto credit = gnc_numeric_neg (amount);
            int output_box = r.count (Role::Box1) ? 1 : r.count (Role::Box2) ? 2 : 0;

            if (output_box && r.count (Role::Box4))
            {
                // One account holds both sides. Route by what the
                // transaction is about: a credit note to a customer
                // reduces output VAT, one from a supplier reduces input.
                bool credit_side = gnc_numeric_negative_p (amount);
                bool customer_credit_note = sales_out && !sales_in && !spend_in;
                bool supplier_credit_note = spend_out && !spend_in && !sales_in;
                if (credit_side && supplier_credit_note)
                    add (4, t, acc, amount);
                else if (!credit_side && customer_credit_note)
                    add (output_box, t, acc, credit);
                else if (credit_side)
                    add (output_box, t, acc, credit);
                else
                    add (4, t, acc, amount);
            }
            else
            {
                if (r.count (Role::Box1)) add (1, t, acc, credit);
                if (r.count (Role::Box2)) add (2, t, acc, credit);
                if (r.count (Role::Box4)) add (4, t, acc, amount);
            }
            // HMRC: Box 6 includes Box 8, and Box 7 includes Box 9.
            if (r.count (Role::Box6) || r.count (Role::Box8)) add (6, t, acc, credit);
            if (r.count (Role::Box8)) add (8, t, acc, credit);
            if (r.count (Role::Box7) || r.count (Role::Box9)) add (7, t, acc, amount);
            if (r.count (Role::Box9)) add (9, t, acc, amount);
        }
    }

    if (!settings.flat_rate.empty ())
    {
        auto parsed = online::parse_decimal (settings.flat_rate);
        auto rate = parsed ? *parsed : gnc_numeric_zero ();
        if (!parsed || gnc_numeric_negative_p (rate) ||
            gnc_numeric_compare (rate, gnc_numeric_create (100, 1)) > 0)
        {
            c.warnings.push_back (_("The flat rate percentage is not a number between 0 and 100."));
        }
        else
        {
            // Box 6 under the scheme is VAT-inclusive turnover.
            auto gross = gnc_numeric_add_fixed (raw[6], raw[1]);
            raw[6] = gross;
            auto fraction = gnc_numeric_div (rate, gnc_numeric_create (100, 1),
                                             GNC_DENOM_AUTO, GNC_HOW_DENOM_EXACT);
            raw[1] = gnc_numeric_mul (gross, fraction, 100, GNC_HOW_RND_ROUND_HALF_UP);
        }
    }

    for (int b : {1, 2, 4})
        c.box[b] = to_pence (raw[b]);
    for (int b : {6, 7, 8, 9})
        c.box[b] = gnc_numeric_convert (raw[b], 1, GNC_HOW_RND_TRUNC);
    c.box[3] = gnc_numeric_add (c.box[1], c.box[2], 100, GNC_HOW_RND_ROUND_HALF_UP);
    c.box[5] = gnc_numeric_abs (gnc_numeric_sub (c.box[3], c.box[4], 100,
                                                 GNC_HOW_RND_ROUND_HALF_UP));

    if (!c.excluded.empty ())
        c.warnings.push_back (_("Some transactions were treated as VAT payments or refunds and left out. Check the list before filing."));
    return c;
}

/* ---------------------------------------------------------------- */
/* Filings and the settlement journal                                */

std::vector<Filing>
load_filings (QofBook* book)
{
    std::vector<Filing> out;
    auto doc = online::load_book_json (book, "mtd-vat-filed");
    if (auto arr = doc.find ("filings"); arr && arr->is_array ())
        for (const auto& f : arr->as_array ())
        {
            Filing fl;
            fl.period_key = f.get_string ("period_key");
            fl.start = f.get_string ("start");
            fl.end = f.get_string ("end");
            fl.payload = hmrc::ReturnPayload::from_json (f["payload"]);
            fl.receipt = hmrc::Receipt::from_json (f["receipt"]);
            if (auto s = f.find ("submitted_at"); s && s->is_number ())
                fl.submitted_at = s->as_int ();
            fl.journal_guid = f.get_string ("journal");
            out.push_back (std::move (fl));
        }
    return out;
}

void
record_filing (QofBook* book, const Filing& f)
{
    auto doc = online::load_book_json (book, "mtd-vat-filed");
    if (!doc.find ("filings") || !doc["filings"].is_array ())
        doc.set ("filings", json::Value::array ());
    auto entry = json::Value::object ();
    entry.set ("period_key", f.period_key);
    entry.set ("start", f.start);
    entry.set ("end", f.end);
    auto payload = f.payload.to_json ();
    entry.set ("payload", payload);
    entry.set ("receipt", f.receipt.to_json ());
    entry.set ("submitted_at", f.submitted_at);
    entry.set ("journal", f.journal_guid);
    auto filings = doc["filings"];
    filings.push (entry);
    doc.set ("filings", filings);
    online::save_book_json (book, "mtd-vat-filed", doc);
}

Transaction*
post_settlement_journal (QofBook* book, const Settings& settings,
                         const Computed& c, const std::string& period_key,
                         time64 date)
{
    auto it = settings.accounts.find (Role::Settlement);
    if (it == settings.accounts.end () || it->second.empty () || !settings.flat_rate.empty ())
        return nullptr;
    auto settlement = account_from_guid_string (book, it->second.front ());
    if (!settlement)
        return nullptr;

    auto table = gnc_commodity_table_get_table (book);
    auto gbp = gnc_commodity_table_lookup (table, GNC_COMMODITY_NS_CURRENCY, "GBP");
    if (!gbp)
        return nullptr;

    std::vector<std::pair<Account*, gnc_numeric>> lines;
    gnc_numeric net = gnc_numeric_zero ();
    for (int box : {1, 2, 4})
        for (const auto& [guid, amount] : c.vat_by_account[box])
        {
            auto acc = account_from_guid_string (book, guid);
            if (!acc || gnc_numeric_zero_p (amount))
                continue;
            // Clearing output VAT is a debit; clearing input VAT a credit.
            auto clear = box == 4 ? gnc_numeric_neg (amount) : amount;
            lines.emplace_back (acc, clear);
            net = gnc_numeric_add_fixed (net, clear);
        }
    if (lines.empty ())
        return nullptr;

    auto trans = xaccMallocTransaction (book);
    xaccTransBeginEdit (trans);
    xaccTransSetCurrency (trans, gbp);
    xaccTransSetDatePostedSecsNormalized (trans, date);
    xaccTransSetDateEnteredSecs (trans, gnc_time (nullptr));
    auto desc = g_strdup_printf (_("VAT return %s filed with HMRC"), period_key.c_str ());
    xaccTransSetDescription (trans, desc);
    g_free (desc);

    auto add_split = [&](Account* acc, gnc_numeric amount) {
        auto split = xaccMallocSplit (book);
        xaccSplitSetParent (split, trans);
        xaccSplitSetAccount (split, acc);
        xaccSplitSetAmount (split, amount);
        xaccSplitSetValue (split, amount);
    };
    for (const auto& [acc, amount] : lines)
        add_split (acc, amount);
    add_split (settlement, gnc_numeric_neg (net));

    std::optional<const char*> tag{g_strdup (period_key.c_str ())};
    qof_instance_set_path_kvp<const char*> (QOF_INSTANCE (trans), tag,
                                            {"online", JOURNAL_SLOT});
    xaccTransCommitEdit (trans);
    return trans;
}

} // namespace gnc::vat
