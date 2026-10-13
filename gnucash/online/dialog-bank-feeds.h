/********************************************************************\
 * dialog-bank-feeds.h -- the Bank Feeds window                     *
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
#ifndef DIALOG_BANK_FEEDS_H
#define DIALOG_BANK_FEEDS_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Open (or raise) the Bank Feeds window for the current book. */
void gnc_ui_bank_feeds_dialog (GtkWindow* parent);

/** Sync every ready connection straight away, then show the matcher. */
void gnc_ui_bank_feeds_sync_all (GtkWindow* parent);

#ifdef __cplusplus
}
#endif

#endif /* DIALOG_BANK_FEEDS_H */
