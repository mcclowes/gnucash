/********************************************************************\
 * dialog-mtd-vat.cpp -- HMRC Making Tax Digital VAT returns window *
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

#include "dialog-mtd-vat.h"
#include "gnc-hmrc.hpp"
#include "gnc-online-book.hpp"
#include "gnc-online-ui.hpp"
#include "gnc-vat-return.hpp"

#include <glib/gi18n.h>

#include <Account.h>
#include <Transaction.h>
#include <gnc-component-manager.h>
#include <gnc-date-edit.h>
#include <gnc-date.h>
#include <gnc-session.h>
#include <gnc-ui-util.h>
#include <qoflog.h>

#include <algorithm>
#include <cctype>
#include <memory>

G_GNUC_UNUSED static const char* log_module = "gnc.online";

#define DIALOG_MTD_VAT_CM_CLASS "dialog-mtd-vat"

using namespace gnc;

namespace
{

/** HMRC's required declaration, verbatim. */
const char* DECLARATION =
    N_("When you submit this VAT information you are making a legal declaration "
       "that the information is true and complete. A false declaration can result "
       "in prosecution.");

enum MapColumn
{
    MAP_NAME, MAP_GUID, MAP_TYPE,
    MAP_ROLE0, // followed by one boolean column per role
    MAP_N_COLUMNS = MAP_ROLE0 + static_cast<int> (vat::all_roles.size ())
};

struct MtdWindow
{
    GtkWidget* window = nullptr;
    GtkWidget* stack = nullptr;
    QofBook* book = nullptr;
    gint component_id = 0;

    vat::Settings settings;
    std::unique_ptr<online::HttpTransport> http;
    std::unique_ptr<hmrc::Session> session;

    // Connection page
    GtkWidget* env_sandbox = nullptr;
    GtkWidget* client_id = nullptr;
    GtkWidget* client_secret = nullptr;
    GtkWidget* redirect_uri = nullptr;
    GtkWidget* vrn = nullptr;
    GtkWidget* status = nullptr;
    GtkWidget* signin = nullptr;
    GtkWidget* signout = nullptr;
    GtkWidget* sandbox_tools = nullptr;

    // Mapping page
    GtkTreeStore* map_store = nullptr;
    GtkWidget* map_view = nullptr;
    GtkWidget* flat_rate = nullptr;

    // Returns page
    GtkWidget* obligations_list = nullptr;
    GtkWidget* detail = nullptr;
    GtkWidget* preview_from = nullptr;
    GtkWidget* preview_to = nullptr;
    std::vector<hmrc::Obligation> obligations;

    // The return on show
    std::optional<hmrc::Obligation> current;
    vat::Computed computed;
    GtkWidget* declare = nullptr;
    GtkWidget* journal = nullptr;
    GtkWidget* submit = nullptr;

    // Filed page
    GtkWidget* filed_box = nullptr;
};

void rebuild_detail (MtdWindow* mw);
void rebuild_obligations (MtdWindow* mw);
void rebuild_filed (MtdWindow* mw);
void update_connection_state (MtdWindow* mw);

std::string
trim (const std::string& s)
{
    auto b = s.find_first_not_of (" \t\r\n");
    auto e = s.find_last_not_of (" \t\r\n");
    return b == std::string::npos ? "" : s.substr (b, e - b + 1);
}

GtkWindow*
win (MtdWindow* mw)
{
    return GTK_WINDOW (mw->window);
}

void
clear_container (GtkWidget* container)
{
    auto children = gtk_container_get_children (GTK_CONTAINER (container));
    for (auto n = children; n; n = n->next)
        gtk_widget_destroy (GTK_WIDGET (n->data));
    g_list_free (children);
}

GtkWidget*
page_box ()
{
    auto box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
    ui::add_class (box, "gnc-online-page");
    return box;
}

GtkWidget*
scrolled (GtkWidget* child)
{
    auto sw = gtk_scrolled_window_new (nullptr, nullptr);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (sw), child);
    return sw;
}

int64_t
now ()
{
    return gnc_time (nullptr);
}

std::string
today_ymd (int offset_days = 0)
{
    auto dt = g_date_time_new_now_local ();
    auto shifted = g_date_time_add_days (dt, offset_days);
    auto s = g_date_time_format (shifted, "%Y-%m-%d");
    std::string out (s);
    g_free (s);
    g_date_time_unref (shifted);
    g_date_time_unref (dt);
    return out;
}

void
make_session (MtdWindow* mw)
{
    mw->session = std::make_unique<hmrc::Session> (
        *mw->http, online::default_secret_store (),
        [mw] { return ui::client_environment (win (mw)); },
        mw->settings.sandbox);
}

/** Run a call that needs a fresh access token, in the background. */
template <typename T> std::optional<T>
with_token (MtdWindow* mw, const std::string& message,
            std::function<T (const hmrc::Client&, const std::string&)> call)
{
    try
    {
        // The environment is gathered here, on the main thread.
        auto env = ui::client_environment (win (mw));
        auto session = mw->session.get ();
        auto cfg = session->config ();
        return ui::fetch<T> (win (mw), message, [&]() -> T {
            auto token = session->access_token (now ());
            hmrc::Client client (*mw->http, cfg, [&] { return env; });
            return call (client, token);
        });
    }
    catch (const online::OnlineError& e)
    {
        ui::show_error (win (mw), _("HMRC could not complete the request"), e.what ());
        if (e.code () == "MISSING_CREDENTIALS" || e.status () == 401)
            update_connection_state (mw);
    }
    catch (const std::exception& e)
    {
        ui::show_error (win (mw), _("Something went wrong"), e.what ());
    }
    return std::nullopt;
}

/* ---------------------------------------------------------------- */
/* Connection page                                                   */

void
read_connection_form (MtdWindow* mw)
{
    auto& cfg = mw->session->config ();
    cfg.client_id = trim (gtk_entry_get_text (GTK_ENTRY (mw->client_id)));
    cfg.client_secret = gtk_entry_get_text (GTK_ENTRY (mw->client_secret));
    cfg.redirect_uri = gtk_entry_get_text (GTK_ENTRY (mw->redirect_uri));
    std::string vrn = gtk_entry_get_text (GTK_ENTRY (mw->vrn));
    vrn.erase (std::remove_if (vrn.begin (), vrn.end (),
                               [](unsigned char c) { return !std::isdigit (c); }),
               vrn.end ());
    mw->settings.vrn = vrn;
}

void
fill_connection_form (MtdWindow* mw)
{
    const auto& cfg = mw->session->config ();
    gtk_entry_set_text (GTK_ENTRY (mw->client_id), cfg.client_id.c_str ());
    gtk_entry_set_text (GTK_ENTRY (mw->client_secret), cfg.client_secret.c_str ());
    gtk_entry_set_text (GTK_ENTRY (mw->redirect_uri), cfg.redirect_uri.c_str ());
    gtk_entry_set_text (GTK_ENTRY (mw->vrn), mw->settings.vrn.c_str ());
}

void
update_connection_state (MtdWindow* mw)
{
    bool signed_in = mw->session->signed_in ();
    std::string env = mw->settings.sandbox ? _("the HMRC sandbox") : _("HMRC");
    std::string text;
    if (!mw->session->config ().complete ())
        text = _("Add your HMRC application's client id and secret to begin.");
    else if (signed_in)
        text = _("Signed in to ") + env + _(". Access renews itself; you will be asked to sign in again after 18 months.");
    else
        text = _("Not signed in to ") + env + ".";
    gtk_label_set_text (GTK_LABEL (mw->status), text.c_str ());
    gtk_widget_set_sensitive (mw->signin, mw->session->config ().complete ());
    gtk_widget_set_sensitive (mw->signout, signed_in);
    gtk_widget_set_visible (mw->sandbox_tools, mw->settings.sandbox);
}

void
on_save_connection (GtkButton*, MtdWindow* mw)
{
    read_connection_form (mw);
    mw->session->save_config ();
    mw->settings.save (mw->book);
    update_connection_state (mw);
    if (!mw->settings.vrn.empty () && !mw->settings.vrn_valid ())
        ui::show_error (win (mw), _("That VAT number doesn't look right"),
                        _("A UK VAT registration number has nine digits."));
}

void
on_environment_toggled (GtkToggleButton* button, MtdWindow* mw)
{
    bool sandbox = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (mw->env_sandbox));
    if (sandbox == mw->settings.sandbox)
        return;
    mw->settings.sandbox = sandbox;
    mw->settings.save (mw->book);
    make_session (mw);
    fill_connection_form (mw);
    update_connection_state (mw);
    mw->obligations.clear ();
    rebuild_obligations (mw);
    (void) button;
}

void
on_sign_in (GtkButton*, MtdWindow* mw)
{
    read_connection_form (mw);
    mw->session->save_config ();
    auto state_raw = g_uuid_string_random ();
    std::string state (state_raw);
    g_free (state_raw);
    auto client = mw->session->client ();
    auto result = ui::run_web_flow (
        win (mw), _("Sign in to HMRC"),
        _("Sign in with your Government Gateway user ID, then grant GnuCash access to your VAT records."),
        client.authorize_url (state), mw->session->config ().redirect_uri, false);
    if (result.kind != ui::WebFlowResult::Kind::Intercepted)
        return;

    auto q = online::parse_query (result.uri);
    if (q.count ("error"))
    {
        ui::show_error (win (mw), _("HMRC did not grant access"),
                        q.count ("error_description") ? q["error_description"] : q["error"]);
        return;
    }
    if (q["state"] != state || q["code"].empty ())
    {
        ui::show_error (win (mw), _("The sign-in could not be verified"),
                        _("The response did not match this request. Please try again."));
        return;
    }
    try
    {
        auto session = mw->session.get ();
        auto code = q["code"];
        ui::run_in_background (win (mw), _("Completing sign-in…"),
                               [&] { session->complete_sign_in (code, now ()); });
    }
    catch (const std::exception& e)
    {
        ui::show_error (win (mw), _("Sign-in failed"), e.what ());
    }
    update_connection_state (mw);
    rebuild_obligations (mw);
}

void
on_sign_out (GtkButton*, MtdWindow* mw)
{
    mw->session->sign_out ();
    mw->obligations.clear ();
    update_connection_state (mw);
    rebuild_obligations (mw);
}

void
on_create_test_org (GtkButton*, MtdWindow* mw)
{
    read_connection_form (mw);
    mw->session->save_config ();
    auto cfg = mw->session->config ();
    auto env = ui::client_environment (win (mw));
    try
    {
        auto org = ui::fetch<hmrc::TestOrganisation> (win (mw), _("Creating a test business…"), [&] {
            hmrc::Client client (*mw->http, cfg, [&] { return env; });
            return client.create_test_organisation (client.application_token ());
        });
        mw->settings.vrn = org.vrn;
        mw->settings.save (mw->book);
        gtk_entry_set_text (GTK_ENTRY (mw->vrn), org.vrn.c_str ());
        ui::show_info (win (mw), _("Sandbox business created"),
                       std::string (_("Use these details when HMRC asks you to sign in:")) +
                       "\n\n" + _("User ID: ") + org.user_id + "\n" +
                       _("Password: ") + org.password + "\n" +
                       _("VAT number: ") + org.vrn + "\n" + _("Name: ") + org.name);
    }
    catch (const std::exception& e)
    {
        ui::show_error (win (mw), _("Could not create a test business"), e.what ());
    }
}

void
on_check_headers (GtkButton*, MtdWindow* mw)
{
    read_connection_form (mw);
    auto cfg = mw->session->config ();
    auto env = ui::client_environment (win (mw));
    try
    {
        auto report = ui::fetch<json::Value> (win (mw), _("Asking HMRC to check…"), [&] {
            hmrc::Client client (*mw->http, cfg, [&] { return env; });
            return client.validate_fraud_headers (client.application_token ());
        });
        std::string text = report.get_string ("message");
        for (const char* section : {"errors", "warnings"})
            if (auto arr = report.find (section); arr && arr->is_array ())
                for (const auto& item : arr->as_array ())
                    text += "\n• " + item.get_string ("message") + " (" +
                            item.get_string ("headers", item.get_string ("header")) + ")";
        ui::show_info (win (mw), report.get_string ("code", _("Result")), text);
    }
    catch (const std::exception& e)
    {
        ui::show_error (win (mw), _("Could not check the headers"), e.what ());
    }
}

GtkWidget*
form_row (GtkGrid* grid, int row, const char* label, GtkWidget* field, const char* hint = nullptr)
{
    auto l = gtk_label_new (label);
    gtk_label_set_xalign (GTK_LABEL (l), 1.0);
    ui::add_class (l, "gnc-online-field-label");
    gtk_grid_attach (grid, l, 0, row * 2, 1, 1);
    gtk_widget_set_hexpand (field, TRUE);
    gtk_grid_attach (grid, field, 1, row * 2, 1, 1);
    if (hint)
    {
        auto h = ui::muted (hint);
        gtk_grid_attach (grid, h, 1, row * 2 + 1, 1, 1);
    }
    return field;
}

GtkWidget*
build_connection_page (MtdWindow* mw)
{
    auto page = page_box ();
    gtk_box_pack_start (GTK_BOX (page), ui::heading (_("Connection to HMRC")), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (page), ui::muted (
        _("GnuCash talks to HMRC directly, as your own registered application. "
          "Create one at developer.service.hmrc.gov.uk, subscribe it to the VAT (MTD) API, "
          "and add the redirect address below to it. Your client secret and sign-in are "
          "kept in your system keyring, never in the book.")), FALSE, FALSE, 0);

    auto c = ui::card ();
    auto grid = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 14);

    auto env_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    ui::add_class (env_box, "linked");
    mw->env_sandbox = gtk_radio_button_new_with_label (nullptr, _("Sandbox"));
    auto live = gtk_radio_button_new_with_label_from_widget (
        GTK_RADIO_BUTTON (mw->env_sandbox), _("Production"));
    gtk_toggle_button_set_mode (GTK_TOGGLE_BUTTON (mw->env_sandbox), FALSE);
    gtk_toggle_button_set_mode (GTK_TOGGLE_BUTTON (live), FALSE);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (mw->settings.sandbox ? mw->env_sandbox : live), TRUE);
    gtk_box_pack_start (GTK_BOX (env_box), mw->env_sandbox, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (env_box), live, FALSE, FALSE, 0);
    gtk_widget_set_halign (env_box, GTK_ALIGN_START);
    g_signal_connect (mw->env_sandbox, "toggled", G_CALLBACK (on_environment_toggled), mw);
    form_row (GTK_GRID (grid), 0, _("Environment"), env_box,
              _("Practise in the sandbox with a test business before filing for real."));

    mw->vrn = form_row (GTK_GRID (grid), 1, _("VAT number"), gtk_entry_new (),
                        _("Nine digits, as on your VAT registration certificate."));
    gtk_entry_set_placeholder_text (GTK_ENTRY (mw->vrn), "123456789");
    mw->client_id = form_row (GTK_GRID (grid), 2, _("Client ID"), gtk_entry_new ());
    mw->client_secret = form_row (GTK_GRID (grid), 3, _("Client secret"), gtk_entry_new ());
    gtk_entry_set_visibility (GTK_ENTRY (mw->client_secret), FALSE);
    gtk_entry_set_input_purpose (GTK_ENTRY (mw->client_secret), GTK_INPUT_PURPOSE_PASSWORD);
    mw->redirect_uri = form_row (GTK_GRID (grid), 4, _("Redirect address"), gtk_entry_new (),
                                 _("Register exactly this address with your application. Nothing needs to listen on it: GnuCash watches for it in the sign-in window."));
    gtk_box_pack_start (GTK_BOX (c), grid, FALSE, FALSE, 0);

    auto buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    auto save = gtk_button_new_with_mnemonic (_("_Save"));
    g_signal_connect (save, "clicked", G_CALLBACK (on_save_connection), mw);
    mw->signin = gtk_button_new_with_mnemonic (_("Sign _in with HMRC…"));
    ui::add_class (mw->signin, "suggested-action");
    g_signal_connect (mw->signin, "clicked", G_CALLBACK (on_sign_in), mw);
    mw->signout = gtk_button_new_with_mnemonic (_("Sign _out"));
    g_signal_connect (mw->signout, "clicked", G_CALLBACK (on_sign_out), mw);
    gtk_box_pack_end (GTK_BOX (buttons), mw->signin, FALSE, FALSE, 0);
    gtk_box_pack_end (GTK_BOX (buttons), save, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (buttons), mw->signout, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (c), buttons, FALSE, FALSE, 4);

    mw->status = ui::muted ("");
    ui::add_class (mw->status, "gnc-online-status");
    gtk_box_pack_start (GTK_BOX (page), mw->status, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (page), c, FALSE, FALSE, 0);

    mw->sandbox_tools = ui::card ();
    gtk_box_pack_start (GTK_BOX (mw->sandbox_tools), ui::heading (_("Sandbox tools"), "gnc-online-subtitle"), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (mw->sandbox_tools), ui::muted (
        _("Create a test business to sign in with, and ask HMRC whether the fraud prevention "
          "headers GnuCash sends meet its specification.")), FALSE, FALSE, 0);
    auto tools = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    auto create = gtk_button_new_with_mnemonic (_("Create _test business"));
    g_signal_connect (create, "clicked", G_CALLBACK (on_create_test_org), mw);
    auto check = gtk_button_new_with_mnemonic (_("_Check fraud prevention headers"));
    g_signal_connect (check, "clicked", G_CALLBACK (on_check_headers), mw);
    gtk_box_pack_start (GTK_BOX (tools), create, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (tools), check, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (mw->sandbox_tools), tools, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (page), mw->sandbox_tools, FALSE, FALSE, 0);
    // Shown or hidden with the environment, not by the window's show_all.
    gtk_widget_show_all (mw->sandbox_tools);
    gtk_widget_set_no_show_all (mw->sandbox_tools, TRUE);

    fill_connection_form (mw);
    return scrolled (page);
}

/* ---------------------------------------------------------------- */
/* Box mapping page                                                  */

void
add_accounts (MtdWindow* mw, Account* parent, GtkTreeIter* parent_iter)
{
    auto children = gnc_account_get_children_sorted (parent);
    for (auto n = children; n; n = n->next)
    {
        auto acc = static_cast<Account*> (n->data);
        if (xaccAccountGetPlaceholder (acc) && !gnc_account_n_children (acc))
            continue;
        GtkTreeIter iter;
        gtk_tree_store_append (mw->map_store, &iter, parent_iter);
        auto guid = online::account_guid_string (acc);
        gtk_tree_store_set (mw->map_store, &iter, MAP_NAME, xaccAccountGetName (acc),
                            MAP_GUID, guid.c_str (),
                            MAP_TYPE, xaccAccountGetTypeStr (xaccAccountGetType (acc)), -1);
        auto roles = mw->settings.roles_for (acc);
        for (size_t r = 0; r < vat::all_roles.size (); ++r)
        {
            bool on = std::find (roles.begin (), roles.end (), vat::all_roles[r]) != roles.end ();
            gtk_tree_store_set (mw->map_store, &iter, MAP_ROLE0 + r, on, -1);
        }
        add_accounts (mw, acc, &iter);
    }
    g_list_free (children);
}

/** True when an ancestor of @a iter already carries role column @a col. */
bool
inherited (GtkTreeModel* model, GtkTreeIter* iter, int col)
{
    GtkTreeIter child = *iter, parent;
    while (gtk_tree_model_iter_parent (model, &parent, &child))
    {
        gboolean on = FALSE;
        gtk_tree_model_get (model, &parent, col, &on, -1);
        if (on)
            return true;
        child = parent;
    }
    return false;
}

void
role_cell_data (GtkTreeViewColumn*, GtkCellRenderer* cell, GtkTreeModel* model,
                GtkTreeIter* iter, gpointer data)
{
    int col = GPOINTER_TO_INT (data);
    gboolean on = FALSE;
    gtk_tree_model_get (model, iter, col, &on, -1);
    g_object_set (cell, "active", on, "inconsistent",
                  !on && inherited (model, iter, col), nullptr);
}

void
on_role_toggled (GtkCellRendererToggle* cell, gchar* path_str, MtdWindow* mw)
{
    int col = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (cell), "column"));
    auto role = vat::all_roles[col - MAP_ROLE0];
    GtkTreeIter iter;
    auto model = GTK_TREE_MODEL (mw->map_store);
    if (!gtk_tree_model_get_iter_from_string (model, &iter, path_str))
        return;
    gboolean on = FALSE;
    gchar* guid = nullptr;
    gtk_tree_model_get (model, &iter, col, &on, MAP_GUID, &guid, -1);
    auto acc = online::account_from_guid_string (mw->book, guid);
    g_free (guid);
    if (!acc)
        return;

    auto roles = mw->settings.roles_for (acc);
    if (on)
        roles.erase (std::remove (roles.begin (), roles.end (), role), roles.end ());
    else
        roles.push_back (role);
    mw->settings.set_roles (acc, roles);
    mw->settings.save (mw->book);

    // The settlement account is singular: redraw every row.
    gtk_tree_model_foreach (model, [](GtkTreeModel* m, GtkTreePath*, GtkTreeIter* it,
                                      gpointer data) -> gboolean {
        auto mw = static_cast<MtdWindow*> (data);
        gchar* g = nullptr;
        gtk_tree_model_get (m, it, MAP_GUID, &g, -1);
        auto a = online::account_from_guid_string (mw->book, g);
        g_free (g);
        auto rs = mw->settings.roles_for (a);
        for (size_t r = 0; r < vat::all_roles.size (); ++r)
            gtk_tree_store_set (GTK_TREE_STORE (m), it, MAP_ROLE0 + r,
                                std::find (rs.begin (), rs.end (), vat::all_roles[r]) != rs.end (),
                                -1);
        return FALSE;
    }, mw);
    rebuild_detail (mw);
}

void
on_flat_rate_changed (GtkEditable* e, MtdWindow* mw)
{
    mw->settings.flat_rate = trim (gtk_entry_get_text (GTK_ENTRY (e)));
    mw->settings.save (mw->book);
    rebuild_detail (mw);
}

GtkWidget*
build_mapping_page (MtdWindow* mw)
{
    auto page = page_box ();
    gtk_box_pack_start (GTK_BOX (page), ui::heading (_("Which accounts feed which box")), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (page), ui::muted (
        _("Tick an account to send it to a box; its sub-accounts follow (shown as a dash). "
          "Give one account both 1 and 4 if you keep a single VAT account: credits go to "
          "Box 1, debits to Box 4. Mark the account you pay HMRC from so those payments "
          "stay out of the return.")), FALSE, FALSE, 0);

    std::vector<GType> types (MAP_N_COLUMNS, G_TYPE_BOOLEAN);
    types[MAP_NAME] = types[MAP_GUID] = types[MAP_TYPE] = G_TYPE_STRING;
    mw->map_store = gtk_tree_store_newv (MAP_N_COLUMNS, types.data ());
    add_accounts (mw, gnc_book_get_root_account (mw->book), nullptr);

    mw->map_view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (mw->map_store));
    ui::add_class (mw->map_view, "gnc-online-mapping");
    auto name_cell = gtk_cell_renderer_text_new ();
    auto name_col = gtk_tree_view_column_new_with_attributes (_("Account"), name_cell,
                                                              "text", MAP_NAME, nullptr);
    gtk_tree_view_column_set_expand (name_col, TRUE);
    gtk_tree_view_append_column (GTK_TREE_VIEW (mw->map_view), name_col);
    auto type_cell = gtk_cell_renderer_text_new ();
    g_object_set (type_cell, "foreground-rgba", nullptr, "scale", 0.85, nullptr);
    auto type_col = gtk_tree_view_column_new_with_attributes (_("Type"), type_cell,
                                                              "text", MAP_TYPE, nullptr);
    gtk_tree_view_append_column (GTK_TREE_VIEW (mw->map_view), type_col);

    static const char* short_names[] = {"1", "2", "4", "6", "7", "8", "9", N_("HMRC")};
    for (size_t r = 0; r < vat::all_roles.size (); ++r)
    {
        auto cell = gtk_cell_renderer_toggle_new ();
        if (vat::all_roles[r] == vat::Role::Settlement)
            gtk_cell_renderer_toggle_set_radio (GTK_CELL_RENDERER_TOGGLE (cell), TRUE);
        g_object_set_data (G_OBJECT (cell), "column", GINT_TO_POINTER (MAP_ROLE0 + r));
        g_signal_connect (cell, "toggled", G_CALLBACK (on_role_toggled), mw);
        auto col = gtk_tree_view_column_new ();
        auto header = gtk_label_new (_(short_names[r]));
        gtk_widget_set_tooltip_text (header, (std::string (vat::role_label (vat::all_roles[r])) +
                                              "\n" + vat::role_hint (vat::all_roles[r])).c_str ());
        gtk_widget_show (header);
        gtk_tree_view_column_set_widget (col, header);
        gtk_tree_view_column_set_alignment (col, 0.5);
        gtk_tree_view_column_pack_start (col, cell, FALSE);
        gtk_tree_view_column_set_cell_data_func (col, cell, role_cell_data,
                                                 GINT_TO_POINTER (MAP_ROLE0 + r), nullptr);
        gtk_tree_view_column_set_min_width (col, 52);
        gtk_tree_view_append_column (GTK_TREE_VIEW (mw->map_view), col);
    }
    gtk_tree_view_expand_all (GTK_TREE_VIEW (mw->map_view));
    gtk_tree_selection_set_mode (gtk_tree_view_get_selection (GTK_TREE_VIEW (mw->map_view)),
                                 GTK_SELECTION_NONE);

    auto sw = gtk_scrolled_window_new (nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (sw), GTK_SHADOW_IN);
    gtk_container_add (GTK_CONTAINER (sw), mw->map_view);
    gtk_box_pack_start (GTK_BOX (page), sw, TRUE, TRUE, 0);

    auto fr = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start (GTK_BOX (fr), gtk_label_new (_("Flat Rate Scheme percentage")), FALSE, FALSE, 0);
    mw->flat_rate = gtk_entry_new ();
    gtk_entry_set_width_chars (GTK_ENTRY (mw->flat_rate), 6);
    gtk_entry_set_placeholder_text (GTK_ENTRY (mw->flat_rate), _("none"));
    gtk_entry_set_text (GTK_ENTRY (mw->flat_rate), mw->settings.flat_rate.c_str ());
    g_signal_connect (mw->flat_rate, "changed", G_CALLBACK (on_flat_rate_changed), mw);
    gtk_box_pack_start (GTK_BOX (fr), mw->flat_rate, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (fr), ui::muted (_("Leave empty unless you are on the scheme.")), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (page), fr, FALSE, FALSE, 0);
    return page;
}

/* ---------------------------------------------------------------- */
/* Returns page                                                      */

std::string
period_title (const std::string& start, const std::string& end)
{
    return ui::human_date (start) + " – " + ui::human_date (end);
}

bool
overdue (const hmrc::Obligation& o)
{
    return o.open () && o.due < today_ymd ();
}

void
on_obligation_selected (GtkListBox*, GtkListBoxRow* row, MtdWindow* mw)
{
    if (!row)
        return;
    auto idx = gtk_list_box_row_get_index (row);
    if (idx < 0 || static_cast<size_t> (idx) >= mw->obligations.size ())
        return;
    mw->current = mw->obligations[idx];
    rebuild_detail (mw);
}

void
rebuild_obligations (MtdWindow* mw)
{
    clear_container (mw->obligations_list);
    if (mw->obligations.empty ())
    {
        auto empty = ui::muted (mw->session->signed_in ()
                                ? _("No obligations yet. Refresh to ask HMRC.")
                                : _("Sign in on the Connection page to see the periods HMRC expects returns for."));
        gtk_widget_set_margin_start (empty, 12);
        gtk_widget_set_margin_end (empty, 12);
        gtk_widget_set_margin_top (empty, 12);
        gtk_list_box_insert (GTK_LIST_BOX (mw->obligations_list), empty, -1);
        gtk_list_box_row_set_activatable (
            gtk_list_box_get_row_at_index (GTK_LIST_BOX (mw->obligations_list), 0), FALSE);
    }
    for (const auto& o : mw->obligations)
    {
        auto box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
        ui::add_class (box, "gnc-online-obligation");
        auto top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        auto title = gtk_label_new (period_title (o.start, o.end).c_str ());
        gtk_label_set_xalign (GTK_LABEL (title), 0.0);
        ui::add_class (title, "gnc-online-strong");
        gtk_box_pack_start (GTK_BOX (top), title, TRUE, TRUE, 0);
        if (!o.open ())
            gtk_box_pack_end (GTK_BOX (top), ui::pill (_("Filed"), "filed"), FALSE, FALSE, 0);
        else if (overdue (o))
            gtk_box_pack_end (GTK_BOX (top), ui::pill (_("Overdue"), "overdue"), FALSE, FALSE, 0);
        else
            gtk_box_pack_end (GTK_BOX (top), ui::pill (_("Open"), "open"), FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box), top, FALSE, FALSE, 0);
        auto sub = o.open () ? std::string (_("Due ")) + ui::human_date (o.due)
                             : std::string (_("Received ")) + ui::human_date (o.received);
        gtk_box_pack_start (GTK_BOX (box), ui::muted (sub, false), FALSE, FALSE, 0);
        gtk_list_box_insert (GTK_LIST_BOX (mw->obligations_list), box, -1);
    }
    gtk_widget_show_all (mw->obligations_list);
}

void
on_refresh_obligations (GtkButton*, MtdWindow* mw)
{
    read_connection_form (mw);
    if (!mw->settings.vrn_valid ())
    {
        ui::show_error (win (mw), _("Add your VAT number first"),
                        _("It goes on the Connection page."));
        return;
    }
    auto vrn = mw->settings.vrn;
    auto from = today_ymd (-365), to = today_ymd ();
    auto result = with_token<std::vector<hmrc::Obligation>> (
        mw, _("Fetching your VAT periods…"),
        [&] (const hmrc::Client& c, const std::string& token) {
            auto open = c.obligations (token, vrn, "", "", "O");
            auto recent = c.obligations (token, vrn, from, to);
            for (const auto& r : recent)
                if (std::none_of (open.begin (), open.end (), [&](const hmrc::Obligation& o) {
                        return o.period_key == r.period_key; }))
                    open.push_back (r);
            return open;
        });
    if (!result)
        return;
    mw->obligations = *result;
    std::sort (mw->obligations.begin (), mw->obligations.end (),
               [](const hmrc::Obligation& a, const hmrc::Obligation& b) {
                   if (a.open () != b.open ()) return a.open ();
                   return a.end > b.end;
               });
    rebuild_obligations (mw);
    if (!mw->obligations.empty ())
        gtk_list_box_select_row (GTK_LIST_BOX (mw->obligations_list),
                                 gtk_list_box_get_row_at_index (GTK_LIST_BOX (mw->obligations_list), 0));
}

void
on_preview (GtkButton*, MtdWindow* mw)
{
    auto start = gnc_date_edit_get_date (GNC_DATE_EDIT (mw->preview_from));
    auto end = gnc_date_edit_get_date (GNC_DATE_EDIT (mw->preview_to));
    auto fmt = [](time64 t) {
        char buf[16];
        auto tm = gnc_localtime (&t);
        strftime (buf, sizeof buf, "%Y-%m-%d", tm);
        gnc_tm_free (tm);
        return std::string (buf);
    };
    hmrc::Obligation o;
    o.start = fmt (start);
    o.end = fmt (end);
    o.status = "";
    mw->current = o;
    gtk_list_box_unselect_all (GTK_LIST_BOX (mw->obligations_list));
    rebuild_detail (mw);
}

void
update_submit_sensitivity (MtdWindow* mw)
{
    if (!mw->submit)
        return;
    bool ok = mw->current && mw->current->open () && mw->session->signed_in () &&
        mw->current->end < today_ymd () &&
        gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (mw->declare));
    gtk_widget_set_sensitive (mw->submit, ok);
}

void
on_declare_toggled (GtkToggleButton*, MtdWindow* mw)
{
    update_submit_sensitivity (mw);
}

void
on_submit (GtkButton*, MtdWindow* mw)
{
    if (!mw->current)
        return;
    auto ob = *mw->current;
    auto payload = mw->computed.payload (ob.period_key);
    payload.finalised = true;
    if (auto problem = payload.validate (); !problem.empty ())
    {
        ui::show_error (win (mw), _("This return can't be filed as it stands"), problem);
        return;
    }
    auto vrn = mw->settings.vrn;
    auto receipt = with_token<hmrc::Receipt> (
        mw, _("Filing your VAT return with HMRC…"),
        [&] (const hmrc::Client& c, const std::string& token) {
            return c.submit_return (token, vrn, payload);
        });
    if (!receipt)
        return;

    vat::Filing f;
    f.period_key = ob.period_key;
    f.start = ob.start;
    f.end = ob.end;
    f.payload = payload;
    f.receipt = *receipt;
    f.submitted_at = now ();
    if (mw->journal && gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (mw->journal)))
    {
        if (auto t = vat::post_settlement_journal (mw->book, mw->settings, mw->computed,
                                                  ob.period_key, vat::parse_date (ob.end, true)))
        {
            char buf[GUID_ENCODING_LENGTH + 1];
            guid_to_string_buff (qof_entity_get_guid (QOF_INSTANCE (t)), buf);
            f.journal_guid = buf;
        }
    }
    vat::record_filing (mw->book, f);

    ui::show_info (win (mw), _("Your VAT return has been filed"),
                   std::string (_("HMRC received it on ")) + receipt->processing_date +
                   ".\n\n" + _("Form bundle number: ") + receipt->form_bundle_number +
                   (receipt->charge_ref_number.empty () ? "" :
                    "\n" + std::string (_("Charge reference: ")) + receipt->charge_ref_number) +
                   (receipt->receipt_id.empty () ? "" :
                    "\n" + std::string (_("Receipt ID: ")) + receipt->receipt_id) +
                   "\n\n" + _("The receipt is saved in this book under Filed."));
    for (auto& o : mw->obligations)
        if (o.period_key == ob.period_key)
        {
            o.status = "F";
            o.received = today_ymd ();
        }
    mw->current->status = "F";
    rebuild_obligations (mw);
    rebuild_detail (mw);
    rebuild_filed (mw);
}

void
toggle_revealer (GtkListBox*, GtkListBoxRow* row, gpointer)
{
    auto revealer = static_cast<GtkWidget*> (g_object_get_data (G_OBJECT (row), "revealer"));
    if (revealer)
        gtk_revealer_set_reveal_child (GTK_REVEALER (revealer),
                                       !gtk_revealer_get_reveal_child (GTK_REVEALER (revealer)));
}

GtkWidget*
lines_table (const std::vector<vat::Line>& lines)
{
    auto grid = gtk_grid_new ();
    ui::add_class (grid, "gnc-online-lines");
    gtk_grid_set_column_spacing (GTK_GRID (grid), 16);
    gtk_grid_set_row_spacing (GTK_GRID (grid), 2);
    int row = 0;
    const size_t limit = 200;
    for (const auto& l : lines)
    {
        if (static_cast<size_t> (row) >= limit)
            break;
        auto date = ui::muted (ui::human_date (l.date), false);
        auto desc = gtk_label_new (l.description.c_str ());
        gtk_label_set_xalign (GTK_LABEL (desc), 0.0);
        gtk_label_set_ellipsize (GTK_LABEL (desc), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand (desc, TRUE);
        auto acct = ui::muted (l.account, false);
        gtk_label_set_ellipsize (GTK_LABEL (acct), PANGO_ELLIPSIZE_START);
        gtk_label_set_max_width_chars (GTK_LABEL (acct), 28);
        auto amt = gtk_label_new (ui::money (l.amount).c_str ());
        gtk_label_set_xalign (GTK_LABEL (amt), 1.0);
        ui::add_class (amt, "gnc-online-figure");
        gtk_grid_attach (GTK_GRID (grid), date, 0, row, 1, 1);
        gtk_grid_attach (GTK_GRID (grid), desc, 1, row, 1, 1);
        gtk_grid_attach (GTK_GRID (grid), acct, 2, row, 1, 1);
        gtk_grid_attach (GTK_GRID (grid), amt, 3, row, 1, 1);
        ++row;
    }
    if (lines.size () > limit)
        gtk_grid_attach (GTK_GRID (grid),
                         ui::muted (ui::format (_("%zu more not shown"), lines.size () - limit), false),
                         1, row, 3, 1);
    if (lines.empty ())
        gtk_grid_attach (GTK_GRID (grid), ui::muted (_("Nothing in this period."), false), 0, 0, 4, 1);
    return grid;
}

GtkWidget*
box_row (int number, const char* label, const char* hint, const std::string& amount,
         const std::vector<vat::Line>* lines, bool total)
{
    auto row = gtk_list_box_row_new ();
    ui::add_class (row, "gnc-online-box-row");
    if (total)
        ui::add_class (row, "total");
    auto outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    auto line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
    auto badge = gtk_label_new (std::to_string (number).c_str ());
    ui::add_class (badge, "gnc-online-box-number");
    gtk_widget_set_valign (badge, GTK_ALIGN_CENTER);
    gtk_box_pack_start (GTK_BOX (line), badge, FALSE, FALSE, 0);
    auto text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
    auto title = gtk_label_new (label);
    gtk_label_set_xalign (GTK_LABEL (title), 0.0);
    gtk_box_pack_start (GTK_BOX (text), title, FALSE, FALSE, 0);
    if (hint)
        gtk_box_pack_start (GTK_BOX (text), ui::muted (hint, false), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (line), text, TRUE, TRUE, 0);
    auto amt = gtk_label_new (amount.c_str ());
    ui::add_class (amt, "gnc-online-amount");
    gtk_widget_set_valign (amt, GTK_ALIGN_CENTER);
    gtk_box_pack_end (GTK_BOX (line), amt, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (outer), line, FALSE, FALSE, 0);

    if (lines)
    {
        auto revealer = gtk_revealer_new ();
        gtk_container_add (GTK_CONTAINER (revealer), lines_table (*lines));
        gtk_box_pack_start (GTK_BOX (outer), revealer, FALSE, FALSE, 0);
        g_object_set_data (G_OBJECT (row), "revealer", revealer);
        gtk_widget_set_tooltip_text (row, _("Show the transactions behind this figure"));
    }
    else
        gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
    gtk_container_add (GTK_CONTAINER (row), outer);
    return row;
}

void
rebuild_detail (MtdWindow* mw)
{
    if (!mw->detail)
        return;
    clear_container (mw->detail);
    mw->declare = mw->journal = mw->submit = nullptr;

    if (!mw->current)
    {
        auto empty = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_valign (empty, GTK_ALIGN_CENTER);
        gtk_widget_set_vexpand (empty, TRUE);
        auto h = ui::heading (_("Choose a period"), "gnc-online-title");
        gtk_label_set_xalign (GTK_LABEL (h), 0.5);
        gtk_box_pack_start (GTK_BOX (empty), h, FALSE, FALSE, 0);
        auto m = ui::muted (_("Pick an obligation from HMRC on the left, or preview any dates. "
                              "GnuCash works the boxes out from your book; nothing is sent "
                              "until you declare and file."));
        gtk_label_set_justify (GTK_LABEL (m), GTK_JUSTIFY_CENTER);
        gtk_label_set_xalign (GTK_LABEL (m), 0.5);
        gtk_box_pack_start (GTK_BOX (empty), m, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (mw->detail), empty, TRUE, TRUE, 0);
        gtk_widget_show_all (mw->detail);
        return;
    }

    const auto& ob = *mw->current;
    mw->computed = vat::compute (mw->book, mw->settings, vat::parse_date (ob.start, false),
                                 vat::parse_date (ob.end, true));
    const auto& c = mw->computed;

    // Heading
    auto head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start (GTK_BOX (head), ui::heading (period_title (ob.start, ob.end)), FALSE, FALSE, 0);
    if (ob.period_key.empty ())
        gtk_box_pack_start (GTK_BOX (head), ui::pill (_("Preview"), "preview"), FALSE, FALSE, 0);
    else if (!ob.open ())
        gtk_box_pack_start (GTK_BOX (head), ui::pill (_("Filed"), "filed"), FALSE, FALSE, 0);
    else if (overdue (ob))
        gtk_box_pack_start (GTK_BOX (head), ui::pill (_("Overdue"), "overdue"), FALSE, FALSE, 0);
    if (mw->settings.sandbox)
        gtk_box_pack_end (GTK_BOX (head), ui::pill (_("Sandbox"), "sandbox"), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (mw->detail), head, FALSE, FALSE, 0);
    if (!ob.period_key.empty ())
        gtk_box_pack_start (GTK_BOX (mw->detail),
                            ui::muted (std::string (_("Due ")) + ui::human_date (ob.due) +
                                       "  ·  " + _("Period key ") + ob.period_key, false),
                            FALSE, FALSE, 0);

    // The one number that matters most.
    auto hero = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    ui::add_class (hero, "gnc-online-hero");
    auto hero_label = ui::muted (c.repayment () ? _("HMRC owes you") : _("You owe HMRC"), false);
    gtk_box_pack_start (GTK_BOX (hero), hero_label, FALSE, FALSE, 0);
    auto hero_amt = gtk_label_new (ui::money (c.box[5]).c_str ());
    gtk_label_set_xalign (GTK_LABEL (hero_amt), 0.0);
    ui::add_class (hero_amt, "gnc-online-hero-amount");
    if (c.repayment ())
        ui::add_class (hero_amt, "repayment");
    gtk_box_pack_start (GTK_BOX (hero), hero_amt, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (mw->detail), hero, FALSE, FALSE, 0);

    if (!c.warnings.empty ())
    {
        auto warn = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
        ui::add_class (warn, "gnc-online-warning");
        for (const auto& w : c.warnings)
            gtk_box_pack_start (GTK_BOX (warn), ui::muted (w), FALSE, FALSE, 0);
        if (!c.excluded.empty ())
        {
            auto exp = gtk_expander_new (
                ui::format (ngettext ("%d transaction left out", "%d transactions left out",
                                      static_cast<int> (c.excluded.size ())),
                            static_cast<int> (c.excluded.size ())).c_str ());
            gtk_container_add (GTK_CONTAINER (exp), lines_table (c.excluded));
            gtk_box_pack_start (GTK_BOX (warn), exp, FALSE, FALSE, 0);
        }
        gtk_box_pack_start (GTK_BOX (mw->detail), warn, FALSE, FALSE, 0);
    }

    // The nine boxes.
    auto list = gtk_list_box_new ();
    ui::add_class (list, "gnc-online-boxes");
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
    g_signal_connect (list, "row-activated", G_CALLBACK (toggle_revealer), nullptr);
    auto pence = [&](int b) { return ui::money (c.box[b]); };
    auto pounds = [&](int b) { return ui::money_whole (c.box[b]); };
    gtk_container_add (GTK_CONTAINER (list), box_row (1, _("VAT due on sales and other outputs"),
                                                      nullptr, pence (1), &c.lines[1], false));
    gtk_container_add (GTK_CONTAINER (list), box_row (2, _("VAT due on acquisitions from the EU"),
                                                      _("Northern Ireland goods only"), pence (2), &c.lines[2], false));
    gtk_container_add (GTK_CONTAINER (list), box_row (3, _("Total VAT due"),
                                                      _("Box 1 + Box 2"), pence (3), nullptr, true));
    gtk_container_add (GTK_CONTAINER (list), box_row (4, _("VAT reclaimed on purchases and other inputs"),
                                                      nullptr, pence (4), &c.lines[4], false));
    gtk_container_add (GTK_CONTAINER (list), box_row (5, c.repayment () ? _("Net VAT to be reclaimed")
                                                                        : _("Net VAT to pay HMRC"),
                                                      _("The difference between Box 3 and Box 4"),
                                                      pence (5), nullptr, true));
    gtk_container_add (GTK_CONTAINER (list), box_row (6, _("Total value of sales, excluding VAT"),
                                                      mw->settings.flat_rate.empty () ? _("Whole pounds")
                                                      : _("Flat Rate Scheme: including VAT"),
                                                      pounds (6), &c.lines[6], false));
    gtk_container_add (GTK_CONTAINER (list), box_row (7, _("Total value of purchases, excluding VAT"),
                                                      _("Whole pounds"), pounds (7), &c.lines[7], false));
    gtk_container_add (GTK_CONTAINER (list), box_row (8, _("Goods supplied to the EU, excluding VAT"),
                                                      _("Northern Ireland only"), pounds (8), &c.lines[8], false));
    gtk_container_add (GTK_CONTAINER (list), box_row (9, _("Goods acquired from the EU, excluding VAT"),
                                                      _("Northern Ireland only"), pounds (9), &c.lines[9], false));
    gtk_box_pack_start (GTK_BOX (mw->detail), list, FALSE, FALSE, 0);

    // Declaration and filing.
    if (ob.open ())
    {
        auto decl = ui::card ();
        ui::add_class (decl, "gnc-online-declaration");
        mw->declare = gtk_check_button_new ();
        auto decl_label = gtk_label_new (_(DECLARATION));
        gtk_label_set_line_wrap (GTK_LABEL (decl_label), TRUE);
        gtk_label_set_xalign (GTK_LABEL (decl_label), 0.0);
        gtk_label_set_max_width_chars (GTK_LABEL (decl_label), 70);
        gtk_container_add (GTK_CONTAINER (mw->declare), decl_label);
        g_signal_connect (mw->declare, "toggled", G_CALLBACK (on_declare_toggled), mw);
        gtk_box_pack_start (GTK_BOX (decl), mw->declare, FALSE, FALSE, 0);

        auto settle = mw->settings.accounts.find (vat::Role::Settlement);
        if (settle != mw->settings.accounts.end () && !settle->second.empty () &&
            mw->settings.flat_rate.empty ())
        {
            auto acc = online::account_from_guid_string (mw->book, settle->second.front ());
            if (acc)
            {
                auto name = gnc_account_get_full_name (acc);
                mw->journal = gtk_check_button_new_with_label (
                    (std::string (_("Afterwards, move this period's VAT into ")) + name).c_str ());
                g_free (name);
                gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (mw->journal), TRUE);
                gtk_box_pack_start (GTK_BOX (decl), mw->journal, FALSE, FALSE, 0);
            }
        }

        auto actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
        mw->submit = gtk_button_new_with_mnemonic (_("_File return with HMRC"));
        ui::add_class (mw->submit, "suggested-action");
        g_signal_connect (mw->submit, "clicked", G_CALLBACK (on_submit), mw);
        gtk_box_pack_end (GTK_BOX (actions), mw->submit, FALSE, FALSE, 0);
        std::string why;
        if (!mw->session->signed_in ())
            why = _("Sign in to HMRC to file.");
        else if (ob.end >= today_ymd ())
            why = std::string (_("You can file once the period ends on ")) + ui::human_date (ob.end) + ".";
        if (!why.empty ())
            gtk_box_pack_start (GTK_BOX (actions), ui::muted (why, false), FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (decl), actions, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (mw->detail), decl, FALSE, FALSE, 0);
        update_submit_sensitivity (mw);
    }
    else if (ob.period_key.empty ())
    {
        gtk_box_pack_start (GTK_BOX (mw->detail),
                            ui::muted (_("A preview only. To file, choose an open obligation from HMRC.")),
                            FALSE, FALSE, 0);
    }
    gtk_widget_show_all (mw->detail);
}

GtkWidget*
build_returns_page (MtdWindow* mw)
{
    auto paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);

    auto left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    ui::add_class (left, "gnc-online-sidebar");
    gtk_widget_set_size_request (left, 300, -1);
    auto left_head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    ui::add_class (left_head, "gnc-online-sidebar-head");
    gtk_box_pack_start (GTK_BOX (left_head), ui::heading (_("Obligations"), "gnc-online-subtitle"), TRUE, TRUE, 0);
    auto refresh = gtk_button_new_from_icon_name ("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (refresh, _("Ask HMRC which returns are due"));
    ui::add_class (refresh, "flat");
    g_signal_connect (refresh, "clicked", G_CALLBACK (on_refresh_obligations), mw);
    gtk_box_pack_end (GTK_BOX (left_head), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (left), left_head, FALSE, FALSE, 0);

    mw->obligations_list = gtk_list_box_new ();
    ui::add_class (mw->obligations_list, "gnc-online-list");
    g_signal_connect (mw->obligations_list, "row-selected", G_CALLBACK (on_obligation_selected), mw);
    auto sw = gtk_scrolled_window_new (nullptr, nullptr);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (sw), mw->obligations_list);
    gtk_box_pack_start (GTK_BOX (left), sw, TRUE, TRUE, 0);

    auto preview = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    ui::add_class (preview, "gnc-online-sidebar-foot");
    gtk_box_pack_start (GTK_BOX (preview), ui::heading (_("Preview any period"), "gnc-online-subtitle"), FALSE, FALSE, 0);
    auto dates = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    // Default to the last complete calendar quarter.
    auto today = g_date_time_new_now_local ();
    int q_month = ((g_date_time_get_month (today) - 1) / 3) * 3 + 1;
    int year = g_date_time_get_year (today);
    int start_month = q_month - 3, start_year = year;
    if (start_month < 1)
    {
        start_month += 12;
        --start_year;
    }
    g_date_time_unref (today);
    auto q_start = gnc_dmy2time64 (1, start_month, start_year);
    auto q_end = gnc_dmy2time64 (1, q_month, year) - 1;
    mw->preview_from = gnc_date_edit_new (q_start, FALSE, FALSE);
    mw->preview_to = gnc_date_edit_new (q_end, FALSE, FALSE);
    gtk_box_pack_start (GTK_BOX (dates), mw->preview_from, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (dates), gtk_label_new ("–"), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (dates), mw->preview_to, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (preview), dates, FALSE, FALSE, 0);
    auto go = gtk_button_new_with_mnemonic (_("_Preview"));
    g_signal_connect (go, "clicked", G_CALLBACK (on_preview), mw);
    gtk_box_pack_start (GTK_BOX (preview), go, FALSE, FALSE, 0);
    gtk_box_pack_end (GTK_BOX (left), preview, FALSE, FALSE, 0);

    gtk_paned_pack1 (GTK_PANED (paned), left, FALSE, FALSE);

    mw->detail = page_box ();
    gtk_paned_pack2 (GTK_PANED (paned), scrolled (mw->detail), TRUE, FALSE);

    rebuild_obligations (mw);
    rebuild_detail (mw);
    return paned;
}

/* ---------------------------------------------------------------- */
/* Filed page                                                        */

void
on_fetch_account (GtkButton*, MtdWindow* mw)
{
    auto vrn = mw->settings.vrn;
    auto from = today_ymd (-365), to = today_ymd ();
    using Pair = std::pair<std::vector<hmrc::Liability>, std::vector<hmrc::Payment>>;
    auto result = with_token<Pair> (mw, _("Fetching your VAT account…"),
        [&] (const hmrc::Client& c, const std::string& token) {
            return Pair{c.liabilities (token, vrn, from, to), c.payments (token, vrn, from, to)};
        });
    if (!result)
        return;
    std::string text;
    for (const auto& l : result->first)
        text += "• " + period_title (l.from, l.to) + ": " + l.type + ", £" +
                l.original_amount + _(" (outstanding £") + l.outstanding_amount + ")\n";
    if (result->first.empty ())
        text += _("No liabilities in the last year.\n");
    text += "\n";
    for (const auto& p : result->second)
        text += "• " + ui::human_date (p.received) + ": £" + p.amount + _(" received\n");
    if (result->second.empty ())
        text += _("No payments in the last year.");
    ui::show_info (win (mw), _("Your VAT account with HMRC"), text);
}

void
rebuild_filed (MtdWindow* mw)
{
    clear_container (mw->filed_box);
    auto head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start (GTK_BOX (head), ui::heading (_("Filed returns")), TRUE, TRUE, 0);
    auto account = gtk_button_new_with_mnemonic (_("_Liabilities and payments…"));
    g_signal_connect (account, "clicked", G_CALLBACK (on_fetch_account), mw);
    gtk_box_pack_end (GTK_BOX (head), account, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (mw->filed_box), head, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (mw->filed_box), ui::muted (
        _("Every return filed from this book, with HMRC's receipt. Keep them: they are your proof of filing.")),
        FALSE, FALSE, 0);

    auto filings = vat::load_filings (mw->book);
    if (filings.empty ())
        gtk_box_pack_start (GTK_BOX (mw->filed_box), ui::muted (_("Nothing filed from this book yet.")), FALSE, FALSE, 0);
    for (auto it = filings.rbegin (); it != filings.rend (); ++it)
    {
        const auto& f = *it;
        auto c = ui::card ();
        auto top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start (GTK_BOX (top), ui::heading (period_title (f.start, f.end), "gnc-online-subtitle"), TRUE, TRUE, 0);
        auto net = gtk_label_new (("£" + f.payload.net_vat_due).c_str ());
        ui::add_class (net, "gnc-online-amount");
        gtk_box_pack_end (GTK_BOX (top), net, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (c), top, FALSE, FALSE, 0);
        std::string meta = std::string (_("Filed ")) + ui::human_date (static_cast<time64> (f.submitted_at)) +
            "  ·  " + _("Form bundle ") + f.receipt.form_bundle_number;
        if (!f.receipt.charge_ref_number.empty ())
            meta += "  ·  " + std::string (_("Charge ref ")) + f.receipt.charge_ref_number;
        if (!f.receipt.receipt_id.empty ())
            meta += "\n" + std::string (_("Receipt ID ")) + f.receipt.receipt_id;
        gtk_box_pack_start (GTK_BOX (c), ui::muted (meta), FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (mw->filed_box), c, FALSE, FALSE, 0);
    }
    gtk_widget_show_all (mw->filed_box);
}

GtkWidget*
build_filed_page (MtdWindow* mw)
{
    mw->filed_box = page_box ();
    rebuild_filed (mw);
    return scrolled (mw->filed_box);
}

/* ---------------------------------------------------------------- */
/* Window                                                            */

void
close_handler (gpointer data)
{
    auto mw = static_cast<MtdWindow*> (data);
    gtk_widget_destroy (mw->window);
}

void
on_destroy (GtkWidget*, MtdWindow* mw)
{
    gnc_unregister_gui_component (mw->component_id);
    if (mw->map_store)
        g_object_unref (mw->map_store);
    delete mw;
}

gboolean
find_any (gpointer, gpointer)
{
    return TRUE;
}

} // anonymous namespace

void
gnc_ui_mtd_vat_dialog (GtkWindow* parent)
{
    if (auto existing = static_cast<MtdWindow*> (
            gnc_find_first_gui_component (DIALOG_MTD_VAT_CM_CLASS, find_any, nullptr)))
    {
        gtk_window_present (GTK_WINDOW (existing->window));
        return;
    }

    auto mw = new MtdWindow;
    mw->book = gnc_get_current_book ();
    mw->settings = vat::Settings::load (mw->book);
    mw->http = online::make_default_transport ();

    mw->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name (mw->window, "gnc-id-mtd-vat");
    ui::add_class (mw->window, "gnc-online-window");
    gtk_window_set_title (GTK_WINDOW (mw->window), _("VAT Returns"));
    gtk_window_set_default_size (GTK_WINDOW (mw->window), 1040, 720);
    if (parent)
        gtk_window_set_transient_for (GTK_WINDOW (mw->window), parent);
    make_session (mw);

    auto header = gtk_header_bar_new ();
    gtk_header_bar_set_show_close_button (GTK_HEADER_BAR (header), TRUE);
    gtk_header_bar_set_title (GTK_HEADER_BAR (header), _("VAT Returns"));
    gtk_header_bar_set_subtitle (GTK_HEADER_BAR (header), _("Making Tax Digital"));
    gtk_window_set_titlebar (GTK_WINDOW (mw->window), header);

    mw->stack = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (mw->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    // The connection page is built first: the others read its widgets.
    auto connection = build_connection_page (mw);
    gtk_stack_add_titled (GTK_STACK (mw->stack), build_returns_page (mw), "returns", _("Returns"));
    gtk_stack_add_titled (GTK_STACK (mw->stack), build_mapping_page (mw), "boxes", _("VAT Boxes"));
    gtk_stack_add_titled (GTK_STACK (mw->stack), build_filed_page (mw), "filed", _("Filed"));
    gtk_stack_add_titled (GTK_STACK (mw->stack), connection, "connection", _("Connection"));

    auto switcher = gtk_stack_switcher_new ();
    gtk_stack_switcher_set_stack (GTK_STACK_SWITCHER (switcher), GTK_STACK (mw->stack));
    gtk_header_bar_set_custom_title (GTK_HEADER_BAR (header), switcher);
    auto title = gtk_label_new (_("VAT Returns"));
    ui::add_class (title, "title");
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header), title);
    gtk_container_add (GTK_CONTAINER (mw->window), mw->stack);

    mw->component_id = gnc_register_gui_component (DIALOG_MTD_VAT_CM_CLASS, nullptr,
                                                   close_handler, mw);
    gnc_gui_component_set_session (mw->component_id, gnc_get_current_session ());
    g_signal_connect (mw->window, "destroy", G_CALLBACK (on_destroy), mw);

    gtk_widget_show_all (mw->window);
    update_connection_state (mw);
    gtk_widget_grab_focus (mw->settings.vrn.empty () ? mw->vrn : mw->client_id);
    // New users land where they can do something.
    if (!mw->session->config ().complete ())
        gtk_stack_set_visible_child_name (GTK_STACK (mw->stack), "connection");
}
