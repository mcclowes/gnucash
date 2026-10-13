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
/** @file gnc-plugin-online.h
 *  @brief Menu entries for bank feeds and HMRC VAT returns.
 */
#ifndef GNC_PLUGIN_ONLINE_H
#define GNC_PLUGIN_ONLINE_H

#include <gtk/gtk.h>
#include "gnc-plugin.h"

G_BEGIN_DECLS

#define GNC_TYPE_PLUGIN_ONLINE (gnc_plugin_online_get_type ())
G_DECLARE_FINAL_TYPE (GncPluginOnline, gnc_plugin_online, GNC, PLUGIN_ONLINE, GncPlugin)

#define GNC_PLUGIN_ONLINE_NAME "gnc-plugin-online"

GncPlugin* gnc_plugin_online_new (void);
void gnc_plugin_online_create_plugin (void);

G_END_DECLS

#endif /* GNC_PLUGIN_ONLINE_H */
