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
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652       *
 * Boston, MA  02110-1301,  USA       gnu@gnu.org                   *
\********************************************************************/

#include <config.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "dialog-bank-feeds.h"
#include "dialog-mtd-vat.h"
#include "gnc-plugin-manager.h"
#include "gnc-plugin-online.h"
#include "gnc-main-window.h"

#define PLUGIN_ACTIONS_NAME "gnc-plugin-online-actions"
#define PLUGIN_UI_FILENAME  "gnc-plugin-online.ui"

static void cmd_bank_feeds (GSimpleAction*, GVariant*, gpointer user_data);
static void cmd_sync_feeds (GSimpleAction*, GVariant*, gpointer user_data);
static void cmd_vat_returns (GSimpleAction*, GVariant*, gpointer user_data);

static GActionEntry gnc_plugin_actions [] =
{
    { "BankFeedsAction", cmd_bank_feeds, nullptr, nullptr, nullptr, { 0 } },
    { "SyncBankFeedsAction", cmd_sync_feeds, nullptr, nullptr, nullptr, { 0 } },
    { "VatReturnsAction", cmd_vat_returns, nullptr, nullptr, nullptr, { 0 } },
};
static guint gnc_plugin_n_actions = G_N_ELEMENTS (gnc_plugin_actions);

static const gchar* gnc_plugin_load_ui_items [] =
{
    "ActionsPlaceholder1",
    "BusinessPlaceholder1",
    nullptr,
};

struct _GncPluginOnline
{
    GncPlugin gnc_plugin;
};

G_DEFINE_TYPE (GncPluginOnline, gnc_plugin_online, GNC_TYPE_PLUGIN)

GncPlugin*
gnc_plugin_online_new (void)
{
    return GNC_PLUGIN (g_object_new (GNC_TYPE_PLUGIN_ONLINE, nullptr));
}

static void
gnc_plugin_online_class_init (GncPluginOnlineClass* klass)
{
    auto plugin_class = GNC_PLUGIN_CLASS (klass);
    plugin_class->plugin_name = GNC_PLUGIN_ONLINE_NAME;
    plugin_class->actions_name = PLUGIN_ACTIONS_NAME;
    plugin_class->actions = gnc_plugin_actions;
    plugin_class->n_actions = gnc_plugin_n_actions;
    plugin_class->ui_filename = PLUGIN_UI_FILENAME;
    plugin_class->ui_updates = gnc_plugin_load_ui_items;
}

static void
gnc_plugin_online_init (GncPluginOnline*)
{
}

static GtkWindow*
window_of (gpointer user_data)
{
    auto data = static_cast<GncMainWindowActionData*> (user_data);
    return GTK_WINDOW (data->window);
}

static void
cmd_bank_feeds (GSimpleAction*, GVariant*, gpointer user_data)
{
    gnc_ui_bank_feeds_dialog (window_of (user_data));
}

static void
cmd_sync_feeds (GSimpleAction*, GVariant*, gpointer user_data)
{
    gnc_ui_bank_feeds_sync_all (window_of (user_data));
}

static void
cmd_vat_returns (GSimpleAction*, GVariant*, gpointer user_data)
{
    gnc_ui_mtd_vat_dialog (window_of (user_data));
}

void
gnc_plugin_online_create_plugin (void)
{
    gnc_plugin_manager_add_plugin (gnc_plugin_manager_get (), gnc_plugin_online_new ());
}
