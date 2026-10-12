/********************************************************************\
 * dialog-bank-feeds.cpp -- the Bank Feeds window                   *
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

#include "dialog-bank-feeds.h"
#include "gnc-bank-feeds.hpp"
#include "gnc-hmrc.hpp"
#include "gnc-online-book.hpp"
#include "gnc-online-ui.hpp"
#include "gnc-plaid.hpp"

#include <glib/gi18n.h>

#include <Account.h>
#include <gnc-account-sel.h>
#include <gnc-component-manager.h>
#include <gnc-date.h>
#include <gnc-session.h>
#include <gnc-ui-util.h>
#include <import-main-matcher.h>
#include <qoflog.h>

#include <algorithm>
#include <cctype>
#include <memory>

G_GNUC_UNUSED static const char* log_module = "gnc.online";

#define DIALOG_BANK_FEEDS_CM_CLASS "dialog-bank-feeds"

using namespace gnc;

namespace
{

/** Where Hosted Link sends the browser when the user is done. The
 *  embedded window catches the navigation; the page never loads. */
const char* COMPLETION_URI = "https://gnucash.invalid/bank-feeds/complete";

struct FeedsWindow
{
    GtkWidget* window = nullptr;
    GtkSizeGroup* columns[4] = {}; // name, bank balance, book balance, import
    GtkWidget* body = nullptr;
    GtkWidget* pending = nullptr;
    QofBook* book = nullptr;
    gint component_id = 0;
    guint rebuild_idle = 0;
    std::unique_ptr<online::HttpTransport> http;
};

void rebuild (FeedsWindow* fw);

/** Rebuild once the current signal has finished, at most once. */
void
queue_rebuild (FeedsWindow* fw)
{
    if (fw->rebuild_idle)
        return;
    fw->rebuild_idle = g_idle_add ([](gpointer data) -> gboolean {
        auto fw = static_cast<FeedsWindow*> (data);
        fw->rebuild_idle = 0;
        rebuild (fw);
        return G_SOURCE_REMOVE;
    }, fw);
}

std::string
trim (const std::string& s)
{
    auto b = s.find_first_not_of (" \t\r\n");
    auto e = s.find_last_not_of (" \t\r\n");
    return b == std::string::npos ? "" : s.substr (b, e - b + 1);
}

void
clear_container (GtkWidget* container)
{
    auto children = gtk_container_get_children (GTK_CONTAINER (container));
    for (auto n = children; n; n = n->next)
        gtk_widget_destroy (GTK_WIDGET (n->data));
    g_list_free (children);
}

/* ---------------------------------------------------------------- */
/* Plaid keys                                                        */

/** Ask for the Plaid keys. Returns true when saved. */
bool
edit_keys (GtkWindow* parent)
{
    auto& secrets = online::default_secret_store ();
    auto cfg = feeds::load_plaid_config (secrets);

    auto dialog = gtk_dialog_new_with_buttons (_("Plaid API Keys"), parent, GTK_DIALOG_MODAL,
                                               _("_Cancel"), GTK_RESPONSE_CANCEL,
                                               _("_Save"), GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
    ui::add_class (dialog, "gnc-online-window");
    auto content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    auto page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    ui::add_class (page, "gnc-online-page");
    gtk_box_pack_start (GTK_BOX (content), page, TRUE, TRUE, 0);

    gtk_box_pack_start (GTK_BOX (page), ui::heading (_("Connect GnuCash to Plaid")), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (page), ui::muted (
        _("Plaid links GnuCash to UK banks through Open Banking. Sign up at dashboard.plaid.com, "
          "then paste the client ID and the secret for the environment you want. The sandbox is "
          "free and has test banks; production needs Plaid's approval. Keys are kept in your "
          "system keyring.")), FALSE, FALSE, 0);

    auto grid = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    auto label = [](const char* text) {
        auto l = gtk_label_new (text);
        gtk_label_set_xalign (GTK_LABEL (l), 1.0);
        return l;
    };
    auto env = gtk_combo_box_text_new ();
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (env), "sandbox", _("Sandbox"));
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (env), "production", _("Production"));
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (env), cfg.environment.c_str ());
    auto id = gtk_entry_new ();
    gtk_entry_set_text (GTK_ENTRY (id), cfg.client_id.c_str ());
    gtk_widget_set_hexpand (id, TRUE);
    auto secret = gtk_entry_new ();
    gtk_entry_set_visibility (GTK_ENTRY (secret), FALSE);
    gtk_entry_set_text (GTK_ENTRY (secret), cfg.secret.c_str ());
    gtk_entry_set_activates_default (GTK_ENTRY (secret), TRUE);
    gtk_grid_attach (GTK_GRID (grid), label (_("Environment")), 0, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), env, 1, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), label (_("Client ID")), 0, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), id, 1, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), label (_("Secret")), 0, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), secret, 1, 2, 1, 1);
    gtk_box_pack_start (GTK_BOX (page), grid, FALSE, FALSE, 0);
    gtk_window_set_default_size (GTK_WINDOW (dialog), 520, -1);
    gtk_widget_show_all (dialog);

    bool saved = false;
    if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK)
    {
        cfg.client_id = trim (gtk_entry_get_text (GTK_ENTRY (id)));
        cfg.secret = trim (gtk_entry_get_text (GTK_ENTRY (secret)));
        auto env_id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (env));
        cfg.environment = env_id ? env_id : "sandbox";
        feeds::save_plaid_config (secrets, cfg);
        saved = true;
    }
    gtk_widget_destroy (dialog);
    return saved;
}

bool
ensure_keys (GtkWindow* parent)
{
    if (feeds::load_plaid_config (online::default_secret_store ()).complete ())
        return true;
    return edit_keys (parent) &&
        feeds::load_plaid_config (online::default_secret_store ()).complete ();
}

/* ---------------------------------------------------------------- */
/* Linking                                                           */

/** Pick a GnuCash account that plausibly matches a bank account. */
std::string
guess_account (QofBook* book, const feeds::LinkedAccount& la)
{
    auto root = gnc_book_get_root_account (book);
    auto all = gnc_account_get_descendants (root);
    Account* best = nullptr;
    auto lower = [](std::string s) {
        std::transform (s.begin (), s.end (), s.begin (), [](unsigned char c) { return std::tolower (c); });
        return s;
    };
    auto want = lower (la.name);
    for (auto n = all; n; n = n->next)
    {
        auto acc = static_cast<Account*> (n->data);
        auto type = xaccAccountGetType (acc);
        if (type != ACCT_TYPE_BANK && type != ACCT_TYPE_CREDIT && type != ACCT_TYPE_CASH)
            continue;
        std::string name = lower (xaccAccountGetName (acc));
        std::string code = xaccAccountGetCode (acc) ? xaccAccountGetCode (acc) : "";
        if ((!la.mask.empty () && (name.find (la.mask) != std::string::npos || code == la.mask)) ||
            name == want)
        {
            best = acc;
            break;
        }
    }
    g_list_free (all);
    return best ? online::account_guid_string (best) : "";
}

struct Linked
{
    std::string access_token;
    plaid::Item item;
    std::vector<plaid::Account> accounts;
};

void
add_connections (FeedsWindow* fw, const plaid::Config& cfg, const std::vector<Linked>& linked)
{
    auto store = feeds::Store::load (fw->book);
    for (const auto& l : linked)
    {
        feeds::Connection conn;
        if (auto existing = store.find (l.item.item_id))
            conn = *existing;
        conn.item_id = l.item.item_id;
        conn.institution = l.item.institution_name.empty () ? _("Bank") : l.item.institution_name;
        conn.environment = cfg.environment;
        conn.status.clear ();
        feeds::merge_accounts (conn, l.accounts);
        for (auto& a : conn.accounts)
            if (a.gnc_account.empty () && !a.ignore)
                a.gnc_account = guess_account (fw->book, a);
        feeds::save_access_token (online::default_secret_store (), conn, l.access_token);
        store.remove (conn.item_id);
        store.connections.push_back (conn);
    }
    store.save (fw->book);
}

/** Exchange public tokens and fetch what we need to show the item. */
std::vector<Linked>
complete_link (online::HttpTransport& http, const plaid::Config& cfg,
               const std::vector<std::string>& public_tokens)
{
    plaid::Client client (http, cfg);
    std::vector<Linked> out;
    for (const auto& pt : public_tokens)
    {
        Linked l;
        auto [access, item_id] = client.exchange_public_token (pt);
        l.access_token = access;
        l.item = client.item (access);
        if (l.item.item_id.empty ())
            l.item.item_id = item_id;
        l.accounts = client.accounts (access);
        out.push_back (std::move (l));
    }
    return out;
}

void
on_connect (GtkButton*, FeedsWindow* fw)
{
    auto parent = GTK_WINDOW (fw->window);
    if (!ensure_keys (parent))
        return;
    auto cfg = feeds::load_plaid_config (online::default_secret_store ());
    auto user_id = hmrc::device_id ();
    try
    {
        auto link = ui::fetch<plaid::LinkToken> (parent, _("Preparing a secure connection…"), [&] {
            plaid::Client client (*fw->http, cfg);
            try
            {
                return client.create_link_token (user_id, COMPLETION_URI);
            }
            catch (const online::OnlineError& e)
            {
                // Some Plaid accounts insist the redirect is allow-listed.
                if (e.code () != "INVALID_FIELD")
                    throw;
                return client.create_link_token (user_id, "");
            }
        });
        if (link.hosted_link_url.empty ())
            throw online::OnlineError (_("Plaid did not offer a hosted connection page. Check that Hosted Link is enabled for your Plaid account."));

        auto result = ui::run_web_flow (parent, _("Connect a Bank"),
                                        _("Choose your bank and sign in. Your bank credentials go to your bank, not to GnuCash."),
                                        link.hosted_link_url, COMPLETION_URI, true);
        if (result.kind == ui::WebFlowResult::Kind::Cancelled)
            return;

        auto linked = ui::fetch<std::vector<Linked>> (parent, _("Fetching your accounts…"), [&] {
            plaid::Client client (*fw->http, cfg);
            auto tokens = client.link_public_tokens (link.link_token);
            return complete_link (*fw->http, cfg, tokens);
        });
        if (linked.empty ())
        {
            ui::show_info (parent, _("No bank was connected"),
                           _("The connection wasn't completed. You can try again at any time."));
            return;
        }
        add_connections (fw, cfg, linked);
        rebuild (fw);
    }
    catch (const std::exception& e)
    {
        ui::show_error (parent, _("The bank could not be connected"), e.what ());
    }
}

void
on_sandbox_bank (GtkButton*, FeedsWindow* fw)
{
    auto parent = GTK_WINDOW (fw->window);
    if (!ensure_keys (parent))
        return;
    auto cfg = feeds::load_plaid_config (online::default_secret_store ());
    if (cfg.environment != "sandbox")
    {
        ui::show_error (parent, _("Test banks live in the sandbox"),
                        _("Switch the Plaid environment to Sandbox to add one."));
        return;
    }
    try
    {
        auto linked = ui::fetch<std::vector<Linked>> (parent, _("Adding a test bank…"), [&] {
            plaid::Client client (*fw->http, cfg);
            auto pt = client.sandbox_public_token (plaid::SANDBOX_UK_INSTITUTION);
            return complete_link (*fw->http, cfg, {pt});
        });
        add_connections (fw, cfg, linked);
        rebuild (fw);
    }
    catch (const std::exception& e)
    {
        ui::show_error (parent, _("The test bank could not be added"), e.what ());
    }
}

void
on_keys (GtkButton*, FeedsWindow* fw)
{
    edit_keys (GTK_WINDOW (fw->window));
}

/* ---------------------------------------------------------------- */
/* Syncing                                                           */

struct Fetched
{
    std::string item_id;
    std::vector<plaid::Account> accounts;
    plaid::SyncResult sync;
    std::string error, error_code;
};

struct MatcherCount
{
    int imported = 0;
};

void
on_trans_processed (GNCImportTransInfo*, gboolean imported, gpointer data)
{
    if (imported)
        static_cast<MatcherCount*> (data)->imported++;
}

}  // anonymous namespace

/** Pull new transactions for @a item_ids and hand them to the import
 *  matcher. Cursors only move once the user accepts the import, so a
 *  cancelled review loses nothing. Returns true if the store changed. */
static bool
sync_connections (GtkWindow* parent, QofBook* book, online::HttpTransport& http,
                  const std::vector<std::string>& item_ids)
{
    auto& secrets = online::default_secret_store ();
    auto cfg = feeds::load_plaid_config (secrets);
    auto store = feeds::Store::load (book);

    struct Job { std::string item_id, token, cursor, environment; };
    std::vector<Job> jobs;
    for (const auto& id : item_ids)
        if (auto c = store.find (id))
        {
            auto token = feeds::load_access_token (secrets, *c);
            if (!token)
            {
                c->status = "ITEM_LOGIN_REQUIRED";
                continue;
            }
            jobs.push_back ({c->item_id, *token, c->cursor, c->environment});
        }
    if (jobs.empty ())
    {
        store.save (book);
        return true;
    }

    std::vector<Fetched> fetched;
    try
    {
        fetched = ui::fetch<std::vector<Fetched>> (parent, _("Downloading transactions…"), [&] {
            std::vector<Fetched> out;
            for (const auto& j : jobs)
            {
                auto jc = cfg;
                jc.environment = j.environment;
                plaid::Client client (http, jc);
                Fetched f;
                f.item_id = j.item_id;
                try
                {
                    f.accounts = client.accounts (j.token);
                    f.sync = client.sync (j.token, j.cursor);
                }
                catch (const online::OnlineError& e)
                {
                    f.error = e.what ();
                    f.error_code = e.code ();
                }
                out.push_back (std::move (f));
            }
            return out;
        });
    }
    catch (const std::exception& e)
    {
        ui::show_error (parent, _("Bank feeds could not be updated"), e.what ());
        return false;
    }

    feeds::SyncSummary summary;
    std::vector<std::string> problems, unmapped;
    std::vector<::Transaction*> txns;
    std::vector<std::pair<feeds::Connection*, std::string>> new_cursors;
    bool include_pending = store.import_pending;

    for (auto& f : fetched)
    {
        auto conn = store.find (f.item_id);
        if (!conn)
            continue;
        if (!f.error.empty ())
        {
            conn->status = f.error_code;
            problems.push_back (conn->institution + ": " + f.error);
            continue;
        }
        conn->status.clear ();
        feeds::merge_accounts (*conn, f.accounts);
        if (!conn->ready ())
        {
            for (const auto& a : conn->accounts)
                if (a.gnc_account.empty () && !a.ignore)
                    unmapped.push_back (conn->institution + " · " + a.name);
            continue;
        }

        auto imported = feeds::imported_index (book, *conn);
        auto describe = [](Split* split) {
            auto t = xaccSplitGetParent (split);
            return ui::human_date (xaccTransGetDate (t)) + "  " + xaccTransGetDescription (t);
        };
        auto consider = [&](const plaid::Transaction& t, bool modified) {
            if (auto it = imported.find (t.transaction_id); it != imported.end ())
            {
                if (modified)
                {
                    summary.modified++;
                    summary.modified_imported.push_back (describe (it->second));
                }
                else
                    summary.already++;
                return;
            }
            // A pending row imported earlier has now posted under a new
            // id: adopt the new id rather than importing it twice.
            if (!t.pending_transaction_id.empty ())
                if (auto it = imported.find (t.pending_transaction_id); it != imported.end ())
                {
                    auto trans = xaccSplitGetParent (it->second);
                    xaccTransBeginEdit (trans);
                    xaccSplitSetOnlineID (it->second, t.transaction_id.c_str ());
                    xaccTransCommitEdit (trans);
                    imported.emplace (t.transaction_id, it->second);
                    summary.already++;
                    return;
                }
            feeds::Skip why;
            auto trans = feeds::build_transaction (book, *conn, t, include_pending, why);
            switch (why)
            {
            case feeds::Skip::None: txns.push_back (trans); summary.added++; break;
            case feeds::Skip::Pending: summary.pending++; break;
            case feeds::Skip::Currency: summary.currency++; break;
            default: summary.unmapped++; break;
            }
        };
        for (const auto& t : f.sync.added)
            consider (t, false);
        for (const auto& t : f.sync.modified)
            consider (t, true);
        // Posted rows adopt their pending twin's id above, so the
        // pending id the bank withdraws no longer matches anything.
        for (const auto& id : f.sync.removed)
            if (auto it = imported.find (id); it != imported.end () &&
                xaccSplitGetOnlineID (it->second) && id == xaccSplitGetOnlineID (it->second))
            {
                summary.removed++;
                summary.removed_imported.push_back (describe (it->second));
            }
        new_cursors.emplace_back (conn, f.sync.next_cursor);
    }

    bool accepted = true;
    if (!txns.empty ())
    {
        MatcherCount count;
        auto matcher = gnc_gen_trans_list_new (GTK_WIDGET (parent), _("Bank feed transactions"),
                                               false, 14, false);
        gnc_gen_trans_list_add_tp_cb (matcher, on_trans_processed, &count);
        for (auto t : txns)
            gnc_gen_trans_list_add_trans (matcher, t);
        if (gnc_gen_trans_list_empty (matcher))
            gnc_gen_trans_list_delete (matcher);
        else
        {
            gnc_gen_trans_list_show_all (matcher);
            gnc_gen_trans_list_run (matcher);
            accepted = count.imported > 0;
        }
    }

    if (accepted)
    {
        auto stamp = gnc_time (nullptr);
        for (auto& [conn, cursor] : new_cursors)
        {
            conn->cursor = cursor;
            conn->last_sync = stamp;
        }
    }
    store.save (book);

    std::string report;
    if (!unmapped.empty ())
    {
        report += _("Choose a GnuCash account (or Don't import) for:");
        for (const auto& u : unmapped)
            report += "\n  • " + u;
        report += "\n\n";
    }
    for (const auto& p : problems)
        report += p + "\n";
    if (summary.pending)
        report += ui::format (ngettext ("%d pending transaction will arrive once it clears.\n",
                                        "%d pending transactions will arrive once they clear.\n",
                                        summary.pending), summary.pending);
    if (summary.currency)
        report += ui::format (ngettext ("%d transaction was in a different currency from its account and was left out.\n",
                                        "%d transactions were in a different currency from their account and were left out.\n",
                                        summary.currency), summary.currency);
    if (!summary.modified_imported.empty ())
    {
        report += _("\nThe bank changed these transactions after they were imported. Check the amount and date:");
        for (const auto& m : summary.modified_imported)
            report += "\n  • " + m;
        report += "\n";
    }
    if (!summary.removed_imported.empty ())
    {
        report += _("\nThe bank withdrew these transactions, which are already in GnuCash. Check them:");
        for (const auto& r : summary.removed_imported)
            report += "\n  • " + r;
    }
    if (!report.empty ())
        ui::show_info (parent, unmapped.empty () && problems.empty () ? _("Bank feeds updated")
                                                                      : _("Some feeds need attention"),
                       report);
    else if (txns.empty () && !new_cursors.empty ())
        ui::show_info (parent, _("You're up to date"), _("There was nothing new from your bank."));
    return true;
}

namespace
{

void
on_sync_one (GtkButton* button, FeedsWindow* fw)
{
    auto id = static_cast<const char*> (g_object_get_data (G_OBJECT (button), "item-id"));
    if (!ensure_keys (GTK_WINDOW (fw->window)))
        return;
    sync_connections (GTK_WINDOW (fw->window), fw->book, *fw->http, {id});
    rebuild (fw);
}

void
on_sync_all (GtkButton*, FeedsWindow* fw)
{
    if (!ensure_keys (GTK_WINDOW (fw->window)))
        return;
    std::vector<std::string> ids;
    for (const auto& c : feeds::Store::load (fw->book).connections)
        ids.push_back (c.item_id);
    sync_connections (GTK_WINDOW (fw->window), fw->book, *fw->http, ids);
    rebuild (fw);
}

void
on_resync_history (GtkMenuItem*, FeedsWindow* fw)
{
    auto parent = GTK_WINDOW (fw->window);
    auto dialog = gtk_message_dialog_new (parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_OK_CANCEL, "%s",
                                          _("Download the full history again?"));
    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s",
        _("Transactions already in GnuCash are recognised and skipped. Use this to recover "
          "anything you skipped in an earlier review."));
    auto r = gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);
    if (r != GTK_RESPONSE_OK)
        return;
    auto store = feeds::Store::load (fw->book);
    for (auto& c : store.connections)
        c.cursor.clear ();
    store.save (fw->book);
    on_sync_all (nullptr, fw);
}

void
on_reconnect (GtkButton* button, FeedsWindow* fw)
{
    std::string id = static_cast<const char*> (g_object_get_data (G_OBJECT (button), "item-id"));
    auto parent = GTK_WINDOW (fw->window);
    auto store = feeds::Store::load (fw->book);
    auto conn = store.find (id);
    if (!conn || !ensure_keys (parent))
        return;
    auto cfg = feeds::load_plaid_config (online::default_secret_store ());
    cfg.environment = conn->environment;
    auto token = feeds::load_access_token (online::default_secret_store (), *conn);
    if (!token)
    {
        ui::show_error (parent, _("This connection's key is missing"),
                        _("Remove it and connect the bank again."));
        return;
    }
    try
    {
        auto user_id = hmrc::device_id ();
        auto link = ui::fetch<plaid::LinkToken> (parent, _("Preparing to reconnect…"), [&] {
            return plaid::Client (*fw->http, cfg).create_link_token (user_id, COMPLETION_URI, *token);
        });
        auto result = ui::run_web_flow (parent, _("Reconnect ") + conn->institution,
                                        _("Sign in to your bank again to renew access."),
                                        link.hosted_link_url, COMPLETION_URI, true);
        if (result.kind == ui::WebFlowResult::Kind::Cancelled)
            return;
        conn->status.clear ();
        store.save (fw->book);
        rebuild (fw);
    }
    catch (const std::exception& e)
    {
        ui::show_error (parent, _("Could not reconnect"), e.what ());
    }
}

void
on_remove (GtkButton* button, FeedsWindow* fw)
{
    std::string id = static_cast<const char*> (g_object_get_data (G_OBJECT (button), "item-id"));
    auto parent = GTK_WINDOW (fw->window);
    auto store = feeds::Store::load (fw->book);
    auto conn = store.find (id);
    if (!conn)
        return;
    auto dialog = gtk_message_dialog_new (parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_NONE, _("Disconnect %s?"),
                                          conn->institution.c_str ());
    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s",
        _("GnuCash will stop receiving its transactions and Plaid will revoke access. "
          "Transactions already imported stay where they are."));
    gtk_dialog_add_buttons (GTK_DIALOG (dialog), _("_Cancel"), GTK_RESPONSE_CANCEL,
                            _("_Disconnect"), GTK_RESPONSE_OK, nullptr);
    auto ok = gtk_dialog_get_widget_for_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
    ui::add_class (ok, "destructive-action");
    auto r = gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);
    if (r != GTK_RESPONSE_OK)
        return;

    auto& secrets = online::default_secret_store ();
    if (auto token = feeds::load_access_token (secrets, *conn))
    {
        auto cfg = feeds::load_plaid_config (secrets);
        cfg.environment = conn->environment;
        try
        {
            ui::run_in_background (parent, _("Disconnecting…"), [&] {
                plaid::Client (*fw->http, cfg).remove_item (*token);
            });
        }
        catch (const std::exception& e)
        {
            PWARN ("Plaid item removal failed: %s", e.what ());
        }
    }
    feeds::erase_access_token (secrets, *conn);
    store.remove (id);
    store.save (fw->book);
    rebuild (fw);
}

void
on_account_changed (GNCAccountSel* gas, FeedsWindow* fw)
{
    auto item = static_cast<const char*> (g_object_get_data (G_OBJECT (gas), "item-id"));
    auto account = static_cast<const char*> (g_object_get_data (G_OBJECT (gas), "account-id"));
    auto store = feeds::Store::load (fw->book);
    auto conn = store.find (item);
    if (!conn)
        return;
    auto la = conn->find_account (account);
    if (!la)
        return;
    auto acc = gnc_account_sel_get_account (gas);
    auto guid = online::account_guid_string (acc);
    if (guid == la->gnc_account)
        return;
    la->gnc_account = guid;
    if (acc)
        la->ignore = false;
    store.save (fw->book);
    // Rebuild later: we're inside the selector's own signal.
    queue_rebuild (fw);
}

void
on_import_switched (GObject* sw, GParamSpec*, FeedsWindow* fw)
{
    auto item = static_cast<const char*> (g_object_get_data (sw, "item-id"));
    auto account = static_cast<const char*> (g_object_get_data (sw, "account-id"));
    auto store = feeds::Store::load (fw->book);
    auto conn = store.find (item);
    if (!conn)
        return;
    if (auto la = conn->find_account (account))
    {
        la->ignore = !gtk_switch_get_active (GTK_SWITCH (sw));
        store.save (fw->book);
    }
    queue_rebuild (fw);
}

void
on_pending_toggled (GtkToggleButton* toggle, FeedsWindow* fw)
{
    auto store = feeds::Store::load (fw->book);
    store.import_pending = gtk_toggle_button_get_active (toggle);
    store.save (fw->book);
}

/* ---------------------------------------------------------------- */
/* Layout                                                            */

GtkWidget*
icon_button (const char* icon, const char* tooltip)
{
    auto b = gtk_button_new_from_icon_name (icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (b, tooltip);
    ui::add_class (b, "flat");
    return b;
}

GtkWidget*
account_row (FeedsWindow* fw, const feeds::Connection& c, const feeds::LinkedAccount& a,
             GtkGrid* grid, int row)
{
    auto name_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
    auto name = gtk_label_new ((a.name + (a.mask.empty () ? "" : "  ••" + a.mask)).c_str ());
    gtk_label_set_xalign (GTK_LABEL (name), 0.0);
    ui::add_class (name, "gnc-online-strong");
    gtk_box_pack_start (GTK_BOX (name_box), name, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (name_box), ui::muted (a.type, false), FALSE, FALSE, 0);
    gtk_grid_attach (grid, name_box, 0, row, 1, 1);
    gtk_size_group_add_widget (fw->columns[0], name_box);

    std::string bank_text;
    auto bank_bal = online::parse_decimal (a.bank_balance);
    if (bank_bal)
        bank_text = ui::money (*bank_bal, a.currency.empty () ? "GBP" : a.currency.c_str ());
    auto bank = gtk_label_new (bank_text.c_str ());
    gtk_label_set_xalign (GTK_LABEL (bank), 1.0);
    ui::add_class (bank, "gnc-online-figure");
    gtk_grid_attach (grid, bank, 1, row, 1, 1);
    gtk_size_group_add_widget (fw->columns[1], bank);

    auto arrow = gtk_image_new_from_icon_name ("go-next-symbolic", GTK_ICON_SIZE_MENU);
    ui::add_class (arrow, "gnc-online-muted");
    gtk_grid_attach (grid, arrow, 2, row, 1, 1);

    auto gas = gnc_account_sel_new ();
    GList* types = nullptr;
    for (auto t : {ACCT_TYPE_BANK, ACCT_TYPE_CREDIT, ACCT_TYPE_CASH, ACCT_TYPE_ASSET,
                   ACCT_TYPE_LIABILITY})
        types = g_list_prepend (types, GINT_TO_POINTER (t));
    gnc_account_sel_set_acct_filters (GNC_ACCOUNT_SEL (gas), types, nullptr);
    g_list_free (types);
    gnc_account_sel_set_new_account_ability (GNC_ACCOUNT_SEL (gas), TRUE);
    auto acc = online::account_from_guid_string (fw->book, a.gnc_account);
    gnc_account_sel_set_account (GNC_ACCOUNT_SEL (gas), acc, FALSE);
    gtk_widget_set_hexpand (gas, TRUE);
    gtk_widget_set_sensitive (gas, !a.ignore);
    g_object_set_data_full (G_OBJECT (gas), "item-id", g_strdup (c.item_id.c_str ()), g_free);
    g_object_set_data_full (G_OBJECT (gas), "account-id", g_strdup (a.account_id.c_str ()), g_free);
    g_signal_connect (gas, "account_sel_changed", G_CALLBACK (on_account_changed), fw);
    gtk_grid_attach (grid, gas, 3, row, 1, 1);

    // How far the book is from the bank.
    std::string diff_text;
    const char* diff_class = "gnc-online-muted";
    if (acc && bank_bal)
    {
        auto book_bal = xaccAccountGetPresentBalance (acc);
        auto type = xaccAccountGetType (acc);
        if (type == ACCT_TYPE_CREDIT || type == ACCT_TYPE_LIABILITY)
            book_bal = gnc_numeric_neg (book_bal);
        auto diff = gnc_numeric_sub (*bank_bal, book_bal, GNC_DENOM_AUTO, GNC_HOW_DENOM_LCD);
        if (gnc_numeric_zero_p (diff))
        {
            diff_text = _("✓ In balance");
            diff_class = "gnc-online-ok";
        }
        else
        {
            auto amount = ui::money (gnc_numeric_abs (diff));
            diff_text = ui::format (gnc_numeric_positive_p (diff) ? _("Book is %s behind")
                                                                  : _("Book is %s ahead"),
                                    amount.c_str ());
            diff_class = "gnc-online-attention";
        }
    }
    auto diff = gtk_label_new (diff_text.c_str ());
    gtk_label_set_xalign (GTK_LABEL (diff), 0.0);
    ui::add_class (diff, diff_class);
    gtk_grid_attach (grid, diff, 4, row, 1, 1);
    gtk_size_group_add_widget (fw->columns[2], diff);

    auto import = gtk_switch_new ();
    gtk_switch_set_active (GTK_SWITCH (import), !a.ignore);
    gtk_widget_set_valign (import, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (import, _("Import this account's transactions"));
    gtk_size_group_add_widget (fw->columns[3], import);
    g_object_set_data_full (G_OBJECT (import), "item-id", g_strdup (c.item_id.c_str ()), g_free);
    g_object_set_data_full (G_OBJECT (import), "account-id", g_strdup (a.account_id.c_str ()), g_free);
    g_signal_connect (import, "notify::active", G_CALLBACK (on_import_switched), fw);
    gtk_grid_attach (grid, import, 5, row, 1, 1);
    return gas;
}

GtkWidget*
connection_card (FeedsWindow* fw, const feeds::Connection& c)
{
    auto card = ui::card ();
    ui::add_class (card, "gnc-online-connection");

    auto head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    auto mark = gtk_label_new (c.institution.substr (0, 1).c_str ());
    ui::add_class (mark, "gnc-online-monogram");
    gtk_box_pack_start (GTK_BOX (head), mark, FALSE, FALSE, 0);
    auto titles = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    auto title_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start (GTK_BOX (title_row), ui::heading (c.institution, "gnc-online-subtitle"), FALSE, FALSE, 0);
    if (c.environment == "sandbox")
        gtk_box_pack_start (GTK_BOX (title_row), ui::pill (_("Sandbox"), "sandbox"), FALSE, FALSE, 0);
    if (c.status == "ITEM_LOGIN_REQUIRED" || c.status == "PENDING_EXPIRATION")
        gtk_box_pack_start (GTK_BOX (title_row), ui::pill (_("Needs sign-in"), "overdue"), FALSE, FALSE, 0);
    else if (!c.status.empty ())
        gtk_box_pack_start (GTK_BOX (title_row), ui::pill (_("Error"), "overdue"), FALSE, FALSE, 0);
    else if (!c.ready ())
        gtk_box_pack_start (GTK_BOX (title_row), ui::pill (_("Choose accounts"), "open"), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (titles), title_row, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (titles),
                        ui::muted (std::string (_("Last synced ")) +
                                   ui::relative_time (c.last_sync, gnc_time (nullptr)), false),
                        FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (head), titles, TRUE, TRUE, 0);

    auto remove = icon_button ("user-trash-symbolic", _("Disconnect this bank"));
    g_object_set_data_full (G_OBJECT (remove), "item-id", g_strdup (c.item_id.c_str ()), g_free);
    g_signal_connect (remove, "clicked", G_CALLBACK (on_remove), fw);
    gtk_box_pack_end (GTK_BOX (head), remove, FALSE, FALSE, 0);
    if (!c.status.empty ())
    {
        auto reconnect = gtk_button_new_with_mnemonic (_("_Reconnect…"));
        g_object_set_data_full (G_OBJECT (reconnect), "item-id", g_strdup (c.item_id.c_str ()), g_free);
        g_signal_connect (reconnect, "clicked", G_CALLBACK (on_reconnect), fw);
        gtk_widget_set_valign (reconnect, GTK_ALIGN_CENTER);
        gtk_box_pack_end (GTK_BOX (head), reconnect, FALSE, FALSE, 0);
    }
    auto sync = gtk_button_new_with_mnemonic (_("S_ync"));
    gtk_widget_set_valign (sync, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive (sync, c.ready ());
    g_object_set_data_full (G_OBJECT (sync), "item-id", g_strdup (c.item_id.c_str ()), g_free);
    g_signal_connect (sync, "clicked", G_CALLBACK (on_sync_one), fw);
    gtk_box_pack_end (GTK_BOX (head), sync, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (card), head, FALSE, FALSE, 0);

    auto grid = gtk_grid_new ();
    ui::add_class (grid, "gnc-online-accounts");
    gtk_grid_set_column_spacing (GTK_GRID (grid), 14);
    gtk_grid_set_row_spacing (GTK_GRID (grid), 10);
    const std::pair<int, const char*> heads[] = {
        {1, _("At the bank")}, {3, _("In GnuCash")}, {4, _("Balance")}, {5, _("Import")}};
    for (auto [col, text] : heads)
    {
        auto l = gtk_label_new (text);
        gtk_label_set_xalign (GTK_LABEL (l), col == 1 ? 1.0 : 0.0);
        ui::add_class (l, "gnc-online-column-head");
        gtk_grid_attach (GTK_GRID (grid), l, col, 0, 1, 1);
    }
    int row;
    row = 1;
    for (const auto& a : c.accounts)
        account_row (fw, c, a, GTK_GRID (grid), row++);
    gtk_box_pack_start (GTK_BOX (card), grid, FALSE, FALSE, 0);
    return card;
}

void
rebuild (FeedsWindow* fw)
{
    clear_container (fw->body);
    auto store = feeds::Store::load (fw->book);
    auto cfg = feeds::load_plaid_config (online::default_secret_store ());

    auto intro = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start (GTK_BOX (intro), ui::heading (_("Bank feeds")), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (intro), ui::muted (
        _("Transactions come straight from your bank through Open Banking, then pass through the "
          "usual import review so nothing lands in your books unseen.")), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (fw->body), intro, FALSE, FALSE, 0);

    if (!cfg.complete ())
    {
        auto c = ui::card ();
        ui::add_class (c, "gnc-online-empty");
        gtk_box_pack_start (GTK_BOX (c), ui::heading (_("First, your Plaid keys"), "gnc-online-subtitle"), FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (c), ui::muted (
            _("GnuCash connects to Plaid with your own developer keys, so your data goes "
              "from your bank to you and nowhere else.")), FALSE, FALSE, 0);
        auto b = gtk_button_new_with_mnemonic (_("Add _Plaid keys…"));
        ui::add_class (b, "suggested-action");
        gtk_widget_set_halign (b, GTK_ALIGN_START);
        g_signal_connect (b, "clicked", G_CALLBACK (+[](GtkButton*, gpointer d) {
            auto fw = static_cast<FeedsWindow*> (d);
            if (edit_keys (GTK_WINDOW (fw->window)))
                rebuild (fw);
        }), fw);
        gtk_box_pack_start (GTK_BOX (c), b, FALSE, FALSE, 4);
        gtk_box_pack_start (GTK_BOX (fw->body), c, FALSE, FALSE, 0);
    }
    else if (store.connections.empty ())
    {
        auto c = ui::card ();
        ui::add_class (c, "gnc-online-empty");
        gtk_box_pack_start (GTK_BOX (c), ui::heading (_("No banks connected yet"), "gnc-online-subtitle"), FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (c), ui::muted (
            _("Connect a current account or credit card. You'll choose which GnuCash account each one feeds.")),
            FALSE, FALSE, 0);
        auto buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        auto b = gtk_button_new_with_mnemonic (_("_Connect a bank…"));
        ui::add_class (b, "suggested-action");
        g_signal_connect (b, "clicked", G_CALLBACK (on_connect), fw);
        gtk_box_pack_start (GTK_BOX (buttons), b, FALSE, FALSE, 0);
        if (cfg.environment == "sandbox")
        {
            auto t = gtk_button_new_with_mnemonic (_("Add a _test bank"));
            g_signal_connect (t, "clicked", G_CALLBACK (on_sandbox_bank), fw);
            gtk_box_pack_start (GTK_BOX (buttons), t, FALSE, FALSE, 0);
        }
        gtk_box_pack_start (GTK_BOX (c), buttons, FALSE, FALSE, 4);
        gtk_box_pack_start (GTK_BOX (fw->body), c, FALSE, FALSE, 0);
    }

    for (const auto& conn : store.connections)
        gtk_box_pack_start (GTK_BOX (fw->body), connection_card (fw, conn), FALSE, FALSE, 0);

    if (fw->pending)
    {
        g_signal_handlers_block_by_func (fw->pending, (gpointer) on_pending_toggled, fw);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (fw->pending), store.import_pending);
        g_signal_handlers_unblock_by_func (fw->pending, (gpointer) on_pending_toggled, fw);
    }
    gtk_widget_show_all (fw->body);
}

void
close_handler (gpointer data)
{
    gtk_widget_destroy (static_cast<FeedsWindow*> (data)->window);
}

void
on_destroy (GtkWidget*, FeedsWindow* fw)
{
    gnc_unregister_gui_component (fw->component_id);
    if (fw->rebuild_idle)
        g_source_remove (fw->rebuild_idle);
    for (auto group : fw->columns)
        g_object_unref (group);
    delete fw;
}

gboolean
find_any (gpointer, gpointer)
{
    return TRUE;
}

} // anonymous namespace

void
gnc_ui_bank_feeds_dialog (GtkWindow* parent)
{
    if (auto existing = static_cast<FeedsWindow*> (
            gnc_find_first_gui_component (DIALOG_BANK_FEEDS_CM_CLASS, find_any, nullptr)))
    {
        gtk_window_present (GTK_WINDOW (existing->window));
        return;
    }

    auto fw = new FeedsWindow;
    fw->book = gnc_get_current_book ();
    fw->http = online::make_default_transport ();
    for (auto& group : fw->columns)
        group = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

    fw->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name (fw->window, "gnc-id-bank-feeds");
    ui::add_class (fw->window, "gnc-online-window");
    gtk_window_set_title (GTK_WINDOW (fw->window), _("Bank Feeds"));
    gtk_window_set_default_size (GTK_WINDOW (fw->window), 1000, 640);
    if (parent)
        gtk_window_set_transient_for (GTK_WINDOW (fw->window), parent);

    auto header = gtk_header_bar_new ();
    gtk_header_bar_set_show_close_button (GTK_HEADER_BAR (header), TRUE);
    gtk_header_bar_set_title (GTK_HEADER_BAR (header), _("Bank Feeds"));
    gtk_header_bar_set_subtitle (GTK_HEADER_BAR (header), _("Open Banking via Plaid"));
    auto connect = gtk_button_new_with_mnemonic (_("_Connect a Bank…"));
    ui::add_class (connect, "suggested-action");
    g_signal_connect (connect, "clicked", G_CALLBACK (on_connect), fw);
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header), connect);
    auto sync_all = gtk_button_new_with_mnemonic (_("Sync _All"));
    g_signal_connect (sync_all, "clicked", G_CALLBACK (on_sync_all), fw);
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header), sync_all);

    auto menu_button = gtk_menu_button_new ();
    gtk_button_set_image (GTK_BUTTON (menu_button),
                          gtk_image_new_from_icon_name ("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
    auto menu = gtk_menu_new ();
    auto keys = gtk_menu_item_new_with_mnemonic (_("Plaid _Keys…"));
    g_signal_connect (keys, "activate", G_CALLBACK (on_keys), fw);
    auto test = gtk_menu_item_new_with_mnemonic (_("Add a Sandbox _Test Bank"));
    g_signal_connect (test, "activate", G_CALLBACK (on_sandbox_bank), fw);
    auto history = gtk_menu_item_new_with_mnemonic (_("Download Full _History Again…"));
    g_signal_connect (history, "activate", G_CALLBACK (on_resync_history), fw);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), keys);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), test);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), history);
    gtk_widget_show_all (menu);
    gtk_menu_button_set_popup (GTK_MENU_BUTTON (menu_button), menu);
    gtk_header_bar_pack_end (GTK_HEADER_BAR (header), menu_button);
    gtk_window_set_titlebar (GTK_WINDOW (fw->window), header);

    auto outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    fw->body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
    ui::add_class (fw->body, "gnc-online-page");
    auto sw = gtk_scrolled_window_new (nullptr, nullptr);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (sw), fw->body);
    gtk_box_pack_start (GTK_BOX (outer), sw, TRUE, TRUE, 0);

    auto foot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    ui::add_class (foot, "gnc-online-footer");
    fw->pending = gtk_check_button_new_with_mnemonic (_("Include _pending transactions"));
    gtk_widget_set_tooltip_text (fw->pending,
        _("Pending card payments can change or vanish before they clear. Most people leave this off."));
    g_signal_connect (fw->pending, "toggled", G_CALLBACK (on_pending_toggled), fw);
    gtk_box_pack_start (GTK_BOX (foot), fw->pending, FALSE, FALSE, 0);
    auto where = std::string (_("Keys and access tokens are kept in ")) +
        online::default_secret_store ().description () + ".";
    gtk_box_pack_end (GTK_BOX (foot), ui::muted (where, false), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (outer), foot, FALSE, FALSE, 0);
    gtk_container_add (GTK_CONTAINER (fw->window), outer);

    fw->component_id = gnc_register_gui_component (DIALOG_BANK_FEEDS_CM_CLASS, nullptr,
                                                   close_handler, fw);
    gnc_gui_component_set_session (fw->component_id, gnc_get_current_session ());
    g_signal_connect (fw->window, "destroy", G_CALLBACK (on_destroy), fw);

    rebuild (fw);
    gtk_widget_show_all (fw->window);
}

void
gnc_ui_bank_feeds_sync_all (GtkWindow* parent)
{
    auto book = gnc_get_current_book ();
    auto store = feeds::Store::load (book);
    if (store.connections.empty () ||
        !feeds::load_plaid_config (online::default_secret_store ()).complete ())
    {
        gnc_ui_bank_feeds_dialog (parent);
        return;
    }
    std::vector<std::string> ids;
    for (const auto& c : store.connections)
        ids.push_back (c.item_id);
    auto http = online::make_default_transport ();
    sync_connections (parent, book, *http, ids);
}
