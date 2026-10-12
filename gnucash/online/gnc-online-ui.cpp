/********************************************************************\
 * gnc-online-ui.cpp -- shared GTK helpers for the online dialogs    *
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

#include "gnc-online-ui.hpp"

#include <glib/gi18n.h>
#include <webkit2/webkit2.h>

#include <gnc-commodity.h>
#include <gnc-date.h>
#include <gnc-locale-utils.h>
#include <gnc-ui-util.h>

#include <atomic>
#include <cstdarg>
#include <exception>
#include <thread>

namespace gnc::ui
{

/* ---------------------------------------------------------------- */
/* Background work                                                   */

namespace
{
struct BackgroundState
{
    std::atomic<bool> done{false};
    GMainLoop* loop = nullptr;
};

gboolean
background_finished (gpointer data)
{
    auto state = static_cast<BackgroundState*> (data);
    if (g_main_loop_is_running (state->loop))
        g_main_loop_quit (state->loop);
    return G_SOURCE_REMOVE;
}
} // anonymous namespace

void
run_in_background (GtkWindow* parent, const std::string& message,
                   std::function<void ()> work)
{
    auto popup = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated (GTK_WINDOW (popup), FALSE);
    gtk_window_set_modal (GTK_WINDOW (popup), TRUE);
    gtk_window_set_resizable (GTK_WINDOW (popup), FALSE);
    gtk_window_set_type_hint (GTK_WINDOW (popup), GDK_WINDOW_TYPE_HINT_DIALOG);
    if (parent)
    {
        gtk_window_set_transient_for (GTK_WINDOW (popup), parent);
        gtk_window_set_position (GTK_WINDOW (popup), GTK_WIN_POS_CENTER_ON_PARENT);
    }
    add_class (popup, "gnc-online-busy");
    auto box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_container_set_border_width (GTK_CONTAINER (box), 20);
    auto spinner = gtk_spinner_new ();
    gtk_spinner_start (GTK_SPINNER (spinner));
    gtk_box_pack_start (GTK_BOX (box), spinner, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), gtk_label_new (message.c_str ()), FALSE, FALSE, 0);
    gtk_container_add (GTK_CONTAINER (popup), box);

    BackgroundState state;
    state.loop = g_main_loop_new (nullptr, FALSE);
    std::exception_ptr error;

    // Defer showing the popup so quick calls don't flash a window.
    // Grab input at once, so nothing can close the caller's window or
    // start a second request; only the popup's appearance is deferred.
    gtk_widget_realize (popup);
    gtk_grab_add (popup);
    struct Deferred { GtkWidget* popup; guint id; } deferred{popup, 0};
    deferred.id = g_timeout_add (250, [](gpointer d) -> gboolean {
        auto deferred = static_cast<Deferred*> (d);
        deferred->id = 0;
        gtk_widget_show_all (deferred->popup);
        return G_SOURCE_REMOVE;
    }, &deferred);

    if (parent)
    {
        auto gdk = gtk_widget_get_window (GTK_WIDGET (parent));
        if (gdk)
        {
            auto cursor = gdk_cursor_new_from_name (gdk_window_get_display (gdk), "wait");
            gdk_window_set_cursor (gdk, cursor);
            g_object_unref (cursor);
        }
    }

    std::thread worker ([&] {
        try { work (); }
        catch (...) { error = std::current_exception (); }
        state.done = true;
        g_idle_add (background_finished, &state);
    });

    // Always run: only this thread dispatches the idle that ends the
    // loop, so it must be consumed here even if the work was instant.
    g_main_loop_run (state.loop);
    worker.join ();

    if (deferred.id)
        g_source_remove (deferred.id);
    gtk_grab_remove (popup);
    gtk_widget_destroy (popup);
    g_main_loop_unref (state.loop);
    if (parent && gtk_widget_get_window (GTK_WIDGET (parent)))
        gdk_window_set_cursor (gtk_widget_get_window (GTK_WIDGET (parent)), nullptr);

    if (error)
        std::rethrow_exception (error);
}

/* ---------------------------------------------------------------- */
/* Messages                                                          */

static void
message (GtkWindow* parent, GtkMessageType type, const std::string& headline,
         const std::string& detail)
{
    auto dialog = gtk_message_dialog_new (parent, GTK_DIALOG_MODAL, type,
                                          GTK_BUTTONS_CLOSE, "%s", headline.c_str ());
    if (!detail.empty ())
        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s",
                                                  detail.c_str ());
    gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);
}

void
show_error (GtkWindow* parent, const std::string& headline, const std::string& detail)
{
    message (parent, GTK_MESSAGE_ERROR, headline, detail);
}

void
show_info (GtkWindow* parent, const std::string& headline, const std::string& detail)
{
    message (parent, GTK_MESSAGE_INFO, headline, detail);
}

/* ---------------------------------------------------------------- */
/* Embedded browser                                                  */

namespace
{
struct WebFlow
{
    GtkWidget* dialog = nullptr;
    std::string prefix;
    std::string current_uri;
    WebFlowResult result{WebFlowResult::Kind::Cancelled, ""};
    GtkWidget* address = nullptr;
};

bool
intercept (WebFlow* flow, const char* uri)
{
    if (!uri || flow->prefix.empty () ||
        g_ascii_strncasecmp (uri, flow->prefix.c_str (), flow->prefix.size ()) != 0)
        return false;
    flow->result = {WebFlowResult::Kind::Intercepted, uri};
    gtk_dialog_response (GTK_DIALOG (flow->dialog), GTK_RESPONSE_OK);
    return true;
}

gboolean
on_decide_policy (WebKitWebView* view, WebKitPolicyDecision* decision,
                  WebKitPolicyDecisionType type, gpointer data)
{
    auto flow = static_cast<WebFlow*> (data);
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
        type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
        return FALSE;
    auto nav = webkit_navigation_policy_decision_get_navigation_action (
        WEBKIT_NAVIGATION_POLICY_DECISION (decision));
    auto uri = webkit_uri_request_get_uri (webkit_navigation_action_get_request (nav));
    if (intercept (flow, uri))
    {
        webkit_policy_decision_ignore (decision);
        return TRUE;
    }
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
    {
        // Banks' own sign-in pages often open in a new window; keep
        // them in this one so the flow can finish here.
        webkit_web_view_load_uri (view, uri);
        webkit_policy_decision_ignore (decision);
        return TRUE;
    }
    return FALSE;
}

void
on_uri_changed (WebKitWebView* view, GParamSpec*, gpointer data)
{
    auto flow = static_cast<WebFlow*> (data);
    auto uri = webkit_web_view_get_uri (view);
    flow->current_uri = uri ? uri : "";
    if (flow->address)
    {
        // Show the host only: it is what the user needs to trust.
        auto parsed = g_uri_parse (flow->current_uri.c_str (), G_URI_FLAGS_NONE, nullptr);
        std::string shown = flow->current_uri;
        if (parsed)
        {
            auto host = g_uri_get_host (parsed);
            bool secure = g_strcmp0 (g_uri_get_scheme (parsed), "https") == 0;
            shown = std::string (secure ? "🔒 " : "") + (host ? host : "");
            g_uri_unref (parsed);
        }
        gtk_label_set_text (GTK_LABEL (flow->address), shown.c_str ());
    }
    intercept (flow, uri);
}

void
on_load_failed (WebKitWebView*, WebKitLoadEvent, gchar* failing_uri, GError*,
                gpointer data)
{
    // A redirect to an address nothing listens on still reveals the code.
    intercept (static_cast<WebFlow*> (data), failing_uri);
}
} // anonymous namespace

WebFlowResult
run_web_flow (GtkWindow* parent, const std::string& title, const std::string& subtitle,
              const std::string& url, const std::string& intercept_prefix,
              bool allow_external)
{
    WebFlow flow;
    flow.prefix = intercept_prefix;

    flow.dialog = gtk_dialog_new_with_buttons (title.c_str (), parent,
                                               GTK_DIALOG_MODAL,
                                               _("_Cancel"), GTK_RESPONSE_CANCEL,
                                               nullptr);
    add_class (flow.dialog, "gnc-online-webflow");
    gtk_window_set_default_size (GTK_WINDOW (flow.dialog), 520, 720);

    auto content = gtk_dialog_get_content_area (GTK_DIALOG (flow.dialog));
    gtk_box_set_spacing (GTK_BOX (content), 0);

    auto bar = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    add_class (bar, "gnc-online-webflow-bar");
    auto sub = muted (subtitle);
    gtk_box_pack_start (GTK_BOX (bar), sub, FALSE, FALSE, 0);
    flow.address = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (flow.address), 0.0);
    add_class (flow.address, "gnc-online-address");
    gtk_box_pack_start (GTK_BOX (bar), flow.address, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (content), bar, FALSE, FALSE, 0);

    // An ephemeral context: no bank or HMRC cookies outlive the window.
    auto context = webkit_web_context_new_ephemeral ();
    auto view = webkit_web_view_new_with_context (context);
    g_object_unref (context);
    g_signal_connect (view, "decide-policy", G_CALLBACK (on_decide_policy), &flow);
    g_signal_connect (view, "notify::uri", G_CALLBACK (on_uri_changed), &flow);
    g_signal_connect (view, "load-failed", G_CALLBACK (on_load_failed), &flow);
    gtk_box_pack_start (GTK_BOX (content), view, TRUE, TRUE, 0);

    if (allow_external)
    {
        gtk_dialog_add_button (GTK_DIALOG (flow.dialog), _("Open in _Browser"), 1);
        gtk_dialog_add_button (GTK_DIALOG (flow.dialog), _("I've _Finished"), 2);
    }

    webkit_web_view_load_uri (WEBKIT_WEB_VIEW (view), url.c_str ());
    gtk_widget_show_all (flow.dialog);

    while (true)
    {
        auto response = gtk_dialog_run (GTK_DIALOG (flow.dialog));
        if (response == 1)
        {
            gtk_show_uri_on_window (GTK_WINDOW (flow.dialog), url.c_str (),
                                    GDK_CURRENT_TIME, nullptr);
            continue;
        }
        if (response == 2)
            flow.result = {WebFlowResult::Kind::FinishedElsewhere, flow.current_uri};
        break;
    }
    // Stop callbacks into the stack-allocated state before it goes.
    g_signal_handlers_disconnect_by_data (view, &flow);
    gtk_widget_destroy (flow.dialog);
    return flow.result;
}

/* ---------------------------------------------------------------- */
/* Fraud prevention environment                                      */

hmrc::ClientEnvironment
client_environment (GtkWindow* window)
{
    auto env = hmrc::collect_system_environment ();
    auto display = gdk_display_get_default ();
    if (display)
    {
        int depth = 24;
        if (auto screen = gdk_display_get_default_screen (display))
            if (auto visual = gdk_screen_get_system_visual (screen))
                depth = gdk_visual_get_depth (visual);
        for (int i = 0; i < gdk_display_get_n_monitors (display); ++i)
        {
            auto mon = gdk_display_get_monitor (display, i);
            GdkRectangle geo;
            gdk_monitor_get_geometry (mon, &geo);
            int scale = gdk_monitor_get_scale_factor (mon);
            // GDK reports logical pixels; HMRC wants physical ones.
            env.screens.push_back ({geo.width * scale, geo.height * scale,
                                    static_cast<double> (scale), depth});
        }
    }
    if (window)
        gtk_window_get_size (window, &env.window_width, &env.window_height);
    return env;
}

/* ---------------------------------------------------------------- */
/* Widgets                                                           */

void
add_class (GtkWidget* w, const char* css_class)
{
    gtk_style_context_add_class (gtk_widget_get_style_context (w), css_class);
}

GtkWidget*
heading (const std::string& text, const char* css_class)
{
    auto label = gtk_label_new (text.c_str ());
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
    add_class (label, css_class);
    return label;
}

GtkWidget*
muted (const std::string& text, bool wrap)
{
    auto label = gtk_label_new (text.c_str ());
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (label), wrap);
    if (wrap)
        gtk_label_set_max_width_chars (GTK_LABEL (label), 72);
    add_class (label, "gnc-online-muted");
    return label;
}

GtkWidget*
pill (const std::string& text, const char* variant)
{
    auto label = gtk_label_new (text.c_str ());
    add_class (label, "gnc-online-pill");
    if (variant)
        add_class (label, variant);
    gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (label, GTK_ALIGN_START);
    return label;
}

GtkWidget*
card ()
{
    auto box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    add_class (box, "gnc-online-card");
    return box;
}

std::string
money (gnc_numeric n, const char* currency)
{
    auto table = gnc_commodity_table_get_table (gnc_get_current_book ());
    auto comm = gnc_commodity_table_lookup (table, GNC_COMMODITY_NS_CURRENCY, currency);
    auto info = comm ? gnc_commodity_print_info (comm, TRUE)
                     : gnc_default_print_info (TRUE);
    return xaccPrintAmount (n, info);
}

std::string
money_whole (gnc_numeric n, const char* currency)
{
    auto table = gnc_commodity_table_get_table (gnc_get_current_book ());
    auto comm = gnc_commodity_table_lookup (table, GNC_COMMODITY_NS_CURRENCY, currency);
    auto info = comm ? gnc_commodity_print_info (comm, TRUE)
                     : gnc_default_print_info (TRUE);
    info.max_decimal_places = 0;
    info.min_decimal_places = 0;
    info.force_fit = TRUE;
    info.round = TRUE;
    std::string out = xaccPrintAmount (gnc_numeric_convert (n, 1, GNC_HOW_RND_TRUNC), info);
    // The printer leaves a bare decimal point when asked for none.
    std::string dp = gnc_localeconv ()->mon_decimal_point;
    if (!dp.empty ())
    {
        auto pos = out.rfind (dp);
        if (pos != std::string::npos &&
            (pos + dp.size () == out.size () ||
             !g_ascii_isdigit (out[pos + dp.size ()])))
            out.erase (pos, dp.size ());
    }
    return out;
}

std::string
human_date (time64 t)
{
    auto dt = g_date_time_new_from_unix_local (t);
    if (!dt)
        return "";
    auto s = g_date_time_format (dt, "%-d %b %Y");
    std::string out = s ? s : "";
    g_free (s);
    g_date_time_unref (dt);
    return out;
}

std::string
human_date (const std::string& ymd)
{
    int y = 0, m = 0, d = 0;
    if (sscanf (ymd.c_str (), "%d-%d-%d", &y, &m, &d) != 3)
        return ymd;
    auto dt = g_date_time_new_local (y, m, d, 12, 0, 0);
    if (!dt)
        return ymd;
    auto s = g_date_time_format (dt, "%-d %b %Y");
    std::string out = s ? s : ymd;
    g_free (s);
    g_date_time_unref (dt);
    return out;
}

std::string
format (const char* fmt, ...)
{
    va_list args;
    va_start (args, fmt);
    auto text = g_strdup_vprintf (fmt, args);
    va_end (args);
    std::string out (text);
    g_free (text);
    return out;
}

std::string
relative_time (int64_t then, int64_t now)
{
    if (then <= 0)
        return _("never");
    auto secs = now - then;
    if (secs < 90)
        return _("just now");
    if (secs < 3600)
    {
        auto m = static_cast<int> (secs / 60);
        return format (ngettext ("%d minute ago", "%d minutes ago", m), m);
    }
    if (secs < 2 * 86400)
    {
        auto h = static_cast<int> (secs / 3600);
        return format (ngettext ("%d hour ago", "%d hours ago", h), h);
    }
    if (secs < 30 * 86400)
    {
        auto d = static_cast<int> (secs / 86400);
        return format (ngettext ("%d day ago", "%d days ago", d), d);
    }
    return human_date (static_cast<time64> (then));
}

} // namespace gnc::ui
