/********************************************************************\
 * gnc-online-ui.hpp -- shared GTK helpers for the online dialogs    *
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
/** @file gnc-online-ui.hpp
 *  @brief Pieces shared by the Bank Feeds and VAT windows: running
 *  network calls off the main thread, the embedded sign-in browser,
 *  and a few widget builders that carry the shared style classes.
 */
#ifndef GNC_ONLINE_UI_HPP
#define GNC_ONLINE_UI_HPP

#include "gnc-hmrc.hpp"

#include <gtk/gtk.h>
#include <gnc-date.h>
#include <gnc-numeric.h>

#include <functional>
#include <optional>
#include <string>

namespace gnc::ui
{

/** Run @a work on a worker thread while a small progress window keeps
 *  the interface alive. @a work must not touch GTK or the engine.
 *  Exceptions thrown by @a work are rethrown here. */
void run_in_background (GtkWindow* parent, const std::string& message,
                        std::function<void ()> work);

template <typename T> T
fetch (GtkWindow* parent, const std::string& message, std::function<T ()> work)
{
    std::optional<T> out;
    run_in_background (parent, message, [&] { out = work (); });
    return std::move (*out);
}

/** Show an error with a short headline and the detail beneath. */
void show_error (GtkWindow* parent, const std::string& headline,
                 const std::string& detail);
void show_info (GtkWindow* parent, const std::string& headline,
                const std::string& detail);

struct WebFlowResult
{
    enum class Kind { Intercepted, FinishedElsewhere, Cancelled } kind;
    std::string uri;
};

/** Open @a url in an embedded, cookie-less browser and wait until it
 *  navigates to an address starting with @a intercept_prefix. When
 *  @a allow_external is set the user may continue in their own browser
 *  and then press "Done", yielding FinishedElsewhere. */
WebFlowResult run_web_flow (GtkWindow* parent, const std::string& title,
                            const std::string& subtitle, const std::string& url,
                            const std::string& intercept_prefix,
                            bool allow_external);

/** The fraud prevention facts, including the screens and the size of
 *  @a window. */
hmrc::ClientEnvironment client_environment (GtkWindow* window);

/** printf into a std::string, for translatable format strings. */
std::string format (const char* fmt, ...) G_GNUC_PRINTF (1, 2);

/* Widget builders. */
GtkWidget* heading (const std::string& text, const char* css_class = "gnc-online-title");
GtkWidget* muted (const std::string& text, bool wrap = true);
GtkWidget* pill (const std::string& text, const char* variant);
GtkWidget* card ();
void add_class (GtkWidget* w, const char* css_class);
/** "£1,234.56" for a GBP amount, using the user's number format. */
std::string money (gnc_numeric n, const char* currency = "GBP");
/** "£23,012" — for the boxes HMRC wants in whole pounds. */
std::string money_whole (gnc_numeric n, const char* currency = "GBP");
/** "12 Oct 2026" */
std::string human_date (const std::string& ymd);
std::string human_date (time64 t);
/** "3 hours ago" */
std::string relative_time (int64_t then, int64_t now);

} // namespace gnc::ui

#endif /* GNC_ONLINE_UI_HPP */
