/********************************************************************\
 * gnc-hmrc.cpp -- HMRC Making Tax Digital (VAT) API client          *
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

#include "gnc-hmrc.hpp"

#include <glib.h>
#include <gnc-filepath-utils.h>

#include <cinttypes>
#include <cstdio>
#include <regex>
#include <sstream>

#if defined(__unix__) || defined(__APPLE__)
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#endif
#ifdef __linux__
#include <linux/if_packet.h>
#endif

namespace gnc::hmrc
{

using online::OnlineError;
using online::url_encode;

static const char* VAT_ACCEPT = "application/vnd.hmrc.1.0+json";

/* ---------------------------------------------------------------- */
/* Config and tokens                                                 */

std::string
Config::base_url () const
{
    return sandbox ? "https://test-api.service.hmrc.gov.uk"
                   : "https://api.service.hmrc.gov.uk";
}

json::Value
Config::to_json () const
{
    auto v = json::Value::object ();
    v.set ("client_id", client_id);
    v.set ("redirect_uri", redirect_uri);
    v.set ("sandbox", sandbox);
    return v;
}

Config
Config::from_json (const json::Value& v)
{
    Config c;
    c.client_id = v.get_string ("client_id");
    c.client_secret = v.get_string ("client_secret");
    c.redirect_uri = v.get_string ("redirect_uri", c.redirect_uri);
    c.sandbox = v.get_bool ("sandbox", true);
    return c;
}

json::Value
Tokens::to_json () const
{
    auto v = json::Value::object ();
    v.set ("access_token", access_token);
    v.set ("refresh_token", refresh_token);
    v.set ("expires_at", expires_at);
    v.set ("scope", scope);
    return v;
}

Tokens
Tokens::from_json (const json::Value& v)
{
    Tokens t;
    t.access_token = v.get_string ("access_token");
    t.refresh_token = v.get_string ("refresh_token");
    if (auto e = v.find ("expires_at"); e && e->is_number ())
        t.expires_at = e->as_int ();
    t.scope = v.get_string ("scope");
    return t;
}

/* ---------------------------------------------------------------- */
/* Return payload                                                    */

/** Parse "123.45" or "123" into pence. Throws on malformed input. */
static int64_t
to_pence (const std::string& s)
{
    static const std::regex re (R"(^(-?)(\d+)(?:\.(\d{1,2}))?$)");
    std::smatch m;
    if (!std::regex_match (s, m, re))
        throw std::invalid_argument ("bad amount: " + s);
    int64_t pounds = std::stoll (m[2].str ());
    int64_t pence = 0;
    if (m[3].matched)
    {
        auto frac = m[3].str ();
        if (frac.size () == 1) frac += '0';
        pence = std::stoll (frac);
    }
    int64_t total = pounds * 100 + pence;
    return m[1].str () == "-" ? -total : total;
}

json::Value
ReturnPayload::to_json () const
{
    auto v = json::Value::object ();
    v.set ("periodKey", period_key);
    v.set ("vatDueSales", json::Value::number (vat_due_sales));
    v.set ("vatDueAcquisitions", json::Value::number (vat_due_acquisitions));
    v.set ("totalVatDue", json::Value::number (total_vat_due));
    v.set ("vatReclaimedCurrPeriod", json::Value::number (vat_reclaimed_curr_period));
    v.set ("netVatDue", json::Value::number (net_vat_due));
    v.set ("totalValueSalesExVAT", json::Value::number (total_value_sales_ex_vat));
    v.set ("totalValuePurchasesExVAT", json::Value::number (total_value_purchases_ex_vat));
    v.set ("totalValueGoodsSuppliedExVAT", json::Value::number (total_value_goods_supplied_ex_vat));
    v.set ("totalAcquisitionsExVAT", json::Value::number (total_acquisitions_ex_vat));
    v.set ("finalised", finalised);
    return v;
}

ReturnPayload
ReturnPayload::from_json (const json::Value& v)
{
    ReturnPayload p;
    p.period_key = v.get_string ("periodKey");
    p.vat_due_sales = v.get_string ("vatDueSales", "0.00");
    p.vat_due_acquisitions = v.get_string ("vatDueAcquisitions", "0.00");
    p.total_vat_due = v.get_string ("totalVatDue", "0.00");
    p.vat_reclaimed_curr_period = v.get_string ("vatReclaimedCurrPeriod", "0.00");
    p.net_vat_due = v.get_string ("netVatDue", "0.00");
    p.total_value_sales_ex_vat = v.get_string ("totalValueSalesExVAT", "0");
    p.total_value_purchases_ex_vat = v.get_string ("totalValuePurchasesExVAT", "0");
    p.total_value_goods_supplied_ex_vat = v.get_string ("totalValueGoodsSuppliedExVAT", "0");
    p.total_acquisitions_ex_vat = v.get_string ("totalAcquisitionsExVAT", "0");
    p.finalised = v.get_bool ("finalised", false);
    return p;
}

std::string
ReturnPayload::validate () const
{
    static const std::regex pence_re (R"(^-?\d{1,11}\.\d{2}$)");
    static const std::regex pounds_re (R"(^-?\d{1,13}$)");
    static const std::regex key_re (R"(^[A-Za-z0-9#]{4}$)");

    if (!std::regex_match (period_key, key_re))
        return "The period key must be the four characters HMRC issued for the obligation.";

    const std::pair<const char*, const std::string*> pence_boxes[] = {
        {"Box 1", &vat_due_sales}, {"Box 2", &vat_due_acquisitions},
        {"Box 3", &total_vat_due}, {"Box 4", &vat_reclaimed_curr_period},
        {"Box 5", &net_vat_due}};
    for (auto [name, val] : pence_boxes)
        if (!std::regex_match (*val, pence_re))
            return std::string (name) + " must be an amount in pounds and pence.";

    const std::pair<const char*, const std::string*> pound_boxes[] = {
        {"Box 6", &total_value_sales_ex_vat}, {"Box 7", &total_value_purchases_ex_vat},
        {"Box 8", &total_value_goods_supplied_ex_vat}, {"Box 9", &total_acquisitions_ex_vat}};
    for (auto [name, val] : pound_boxes)
        if (!std::regex_match (*val, pounds_re))
            return std::string (name) + " must be a whole number of pounds.";

    auto b1 = to_pence (vat_due_sales), b2 = to_pence (vat_due_acquisitions);
    auto b3 = to_pence (total_vat_due), b4 = to_pence (vat_reclaimed_curr_period);
    auto b5 = to_pence (net_vat_due);
    if (b3 != b1 + b2)
        return "Box 3 must equal Box 1 plus Box 2.";
    if (b5 < 0)
        return "Box 5 cannot be negative.";
    if (b5 != (b3 >= b4 ? b3 - b4 : b4 - b3))
        return "Box 5 must be the difference between Box 3 and Box 4.";
    return "";
}

json::Value
Receipt::to_json () const
{
    auto v = json::Value::object ();
    v.set ("processingDate", processing_date);
    v.set ("formBundleNumber", form_bundle_number);
    v.set ("paymentIndicator", payment_indicator);
    v.set ("chargeRefNumber", charge_ref_number);
    v.set ("receiptId", receipt_id);
    v.set ("correlationId", correlation_id);
    v.set ("receiptTimestamp", receipt_timestamp);
    v.set ("receiptSignature", receipt_signature);
    return v;
}

Receipt
Receipt::from_json (const json::Value& v)
{
    Receipt r;
    r.processing_date = v.get_string ("processingDate");
    r.form_bundle_number = v.get_string ("formBundleNumber");
    r.payment_indicator = v.get_string ("paymentIndicator");
    r.charge_ref_number = v.get_string ("chargeRefNumber");
    r.receipt_id = v.get_string ("receiptId");
    r.correlation_id = v.get_string ("correlationId");
    r.receipt_timestamp = v.get_string ("receiptTimestamp");
    r.receipt_signature = v.get_string ("receiptSignature");
    return r;
}

/* ---------------------------------------------------------------- */
/* Fraud prevention                                                  */

static std::string
join (const std::vector<std::string>& parts, const char* sep)
{
    std::string out;
    for (const auto& p : parts)
    {
        if (!out.empty ()) out += sep;
        out += p;
    }
    return out;
}

static std::string
format_scale (double scale)
{
    char buf[32];
    snprintf (buf, sizeof buf, "%.2f", scale);
    std::string s (buf);
    while (!s.empty () && s.back () == '0') s.pop_back ();
    if (!s.empty () && s.back () == '.') s.pop_back ();
    return s;
}

Headers
fraud_prevention_headers (const ClientEnvironment& env)
{
    Headers h;
    auto add = [&](const char* name, const std::string& value) {
        if (!value.empty ())
            h.emplace_back (name, value);
    };
    auto kv = [](const std::vector<std::pair<std::string, std::string>>& pairs) {
        std::vector<std::string> parts;
        for (const auto& [k, v] : pairs)
            if (!v.empty ())
                parts.push_back (url_encode (k) + "=" + url_encode (v));
        return join (parts, "&");
    };
    auto encoded_list = [](const std::vector<std::string>& items) {
        std::vector<std::string> parts;
        for (const auto& i : items)
            parts.push_back (url_encode (i));
        return join (parts, ",");
    };

    add ("Gov-Client-Connection-Method", "DESKTOP_APP_DIRECT");
    add ("Gov-Client-Device-ID", env.device_id);
    if (!env.os_user.empty ())
        add ("Gov-Client-User-IDs", "os=" + url_encode (env.os_user));
    if (!env.utc_offset.empty ())
        add ("Gov-Client-Timezone", "UTC" + env.utc_offset);
    add ("Gov-Client-Local-IPs", encoded_list (env.local_ips));
    if (!env.local_ips.empty ())
        add ("Gov-Client-Local-IPs-Timestamp", env.local_ips_timestamp);
    add ("Gov-Client-MAC-Addresses", encoded_list (env.mac_addresses));

    std::vector<std::string> screens;
    for (const auto& s : env.screens)
        screens.push_back (kv ({{"width", std::to_string (s.width)},
                                {"height", std::to_string (s.height)},
                                {"scaling-factor", format_scale (s.scale)},
                                {"colour-depth", std::to_string (s.depth)}}));
    add ("Gov-Client-Screens", join (screens, ","));
    if (env.window_width > 0 && env.window_height > 0)
        add ("Gov-Client-Window-Size",
             kv ({{"width", std::to_string (env.window_width)},
                  {"height", std::to_string (env.window_height)}}));
    add ("Gov-Client-User-Agent",
         kv ({{"os-family", env.os_family}, {"os-version", env.os_version},
              {"device-manufacturer", env.device_manufacturer},
              {"device-model", env.device_model}}));
    if (!env.product_version.empty ())
        add ("Gov-Vendor-Version",
             url_encode (env.product_name) + "=" + url_encode (env.product_version));
    add ("Gov-Vendor-Product-Name", url_encode (env.product_name));
    return h;
}

static std::string
read_trimmed (const char* path)
{
    gchar* contents = nullptr;
    if (!g_file_get_contents (path, &contents, nullptr, nullptr))
        return "";
    std::string s (g_strstrip (contents));
    g_free (contents);
    return s;
}

std::string
device_id ()
{
    auto path = gnc_build_userdata_path ("hmrc-device-id");
    auto id = read_trimmed (path);
    if (id.empty ())
    {
        auto uuid = g_uuid_string_random ();
        id = uuid;
        g_free (uuid);
        g_file_set_contents (path, id.c_str (), -1, nullptr);
    }
    g_free (path);
    return id;
}

ClientEnvironment
collect_system_environment ()
{
    ClientEnvironment env;
    env.device_id = device_id ();
    env.os_user = g_get_user_name ();
    env.product_version = PROJECT_VERSION;

    auto now = g_date_time_new_now_local ();
    auto offset_us = g_date_time_get_utc_offset (now);
    auto minutes = offset_us / G_TIME_SPAN_MINUTE;
    char buf[16];
    snprintf (buf, sizeof buf, "%c%02d:%02d", minutes < 0 ? '-' : '+',
              static_cast<int> (std::abs (minutes) / 60),
              static_cast<int> (std::abs (minutes) % 60));
    env.utc_offset = buf;
    g_date_time_unref (now);

    auto utc = g_date_time_new_now_utc ();
    auto stamp = g_date_time_format (utc, "%Y-%m-%dT%H:%M:%S");
    snprintf (buf, sizeof buf, ".%03dZ", g_date_time_get_microsecond (utc) / 1000);
    env.local_ips_timestamp = std::string (stamp) + buf;
    g_free (stamp);
    g_date_time_unref (utc);

#if defined(_WIN32)
    env.os_family = "Windows";
#elif defined(__APPLE__)
    env.os_family = "macOS";
#endif

#if defined(__unix__) || defined(__APPLE__)
    struct utsname u;
    if (uname (&u) == 0)
    {
#ifndef __APPLE__
        env.os_family = u.sysname;
#endif
        env.os_version = u.release;
    }

    struct ifaddrs* ifs = nullptr;
    if (getifaddrs (&ifs) == 0)
    {
        for (auto i = ifs; i; i = i->ifa_next)
        {
            if (!i->ifa_addr || (i->ifa_flags & IFF_LOOPBACK))
                continue;
            auto family = i->ifa_addr->sa_family;
            if (family == AF_INET || family == AF_INET6)
            {
                char host[NI_MAXHOST];
                socklen_t len = family == AF_INET ? sizeof (struct sockaddr_in)
                                                  : sizeof (struct sockaddr_in6);
                if (getnameinfo (i->ifa_addr, len, host, sizeof host,
                                 nullptr, 0, NI_NUMERICHOST) == 0)
                {
                    std::string ip (host);
                    auto pct = ip.find ('%'); // drop IPv6 zone index
                    if (pct != std::string::npos)
                        ip.resize (pct);
                    env.local_ips.push_back (ip);
                }
            }
#ifdef __linux__
            else if (family == AF_PACKET)
            {
                auto ll = reinterpret_cast<struct sockaddr_ll*> (i->ifa_addr);
                if (ll->sll_halen != 6)
                    continue;
                bool zero = true;
                char mac[18];
                for (int b = 0; b < 6; ++b)
                    zero = zero && ll->sll_addr[b] == 0;
                if (zero)
                    continue;
                snprintf (mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x",
                          ll->sll_addr[0], ll->sll_addr[1], ll->sll_addr[2],
                          ll->sll_addr[3], ll->sll_addr[4], ll->sll_addr[5]);
                env.mac_addresses.push_back (mac);
            }
#endif
        }
        freeifaddrs (ifs);
    }
#endif

#ifdef __linux__
    env.device_manufacturer = read_trimmed ("/sys/class/dmi/id/sys_vendor");
    env.device_model = read_trimmed ("/sys/class/dmi/id/product_name");
#endif
    return env;
}

/* ---------------------------------------------------------------- */
/* Errors                                                            */

std::string
describe_error (const std::string& code, const std::string& fallback)
{
    static const std::map<std::string, std::string> messages = {
        {"VRN_INVALID", "HMRC did not recognise that VAT registration number."},
        {"INVALID_VRN", "HMRC did not recognise that VAT registration number."},
        {"DUPLICATE_SUBMISSION", "A return for this period has already been submitted."},
        {"TAX_PERIOD_NOT_ENDED", "This VAT period has not ended yet. Submit it after the end date."},
        {"NOT_FINALISED", "The return must be declared final before it can be submitted."},
        {"INVALID_PERIODKEY", "HMRC did not recognise the period key for this obligation."},
        {"PERIOD_KEY_INVALID", "HMRC did not recognise the period key for this obligation."},
        {"CLIENT_OR_AGENT_NOT_AUTHORISED", "The signed-in Government Gateway account is not authorised for this VAT number."},
        {"INVALID_CREDENTIALS", "Your HMRC sign-in has expired. Please sign in again."},
        {"MISSING_CREDENTIALS", "You are not signed in to HMRC."},
        {"INSOLVENT_TRADER", "HMRC reports this business as insolvent; returns cannot be filed online."},
        {"RULE_INSOLVENT_TRADER", "HMRC reports this business as insolvent; returns cannot be filed online."},
        {"FORBIDDEN", "HMRC refused the request for this VAT number."},
        {"MESSAGE_THROTTLED_OUT", "HMRC is rate limiting requests. Wait a minute and try again."},
        {"INVALID_SCOPE", "The application is not permitted to use the VAT API. Check its subscriptions on the HMRC Developer Hub."},
        {"invalid_grant", "HMRC rejected the sign-in. Please sign in again."},
        {"invalid_client", "HMRC rejected the application's client id or secret."},
    };
    auto it = messages.find (code);
    if (it != messages.end ())
        return it->second;
    return fallback.empty () ? code : fallback;
}

/* ---------------------------------------------------------------- */
/* Client                                                            */

Client::Client (online::HttpTransport& http, Config cfg, EnvironmentProvider env)
    : m_http{http}, m_cfg{std::move (cfg)}, m_env{std::move (env)} {}

std::string
Client::authorize_url (const std::string& state) const
{
    return m_cfg.base_url () + "/oauth/authorize?" +
        online::form_encode ({{"response_type", "code"},
                              {"client_id", m_cfg.client_id},
                              {"scope", "read:vat write:vat"},
                              {"state", state},
                              {"redirect_uri", m_cfg.redirect_uri}});
}

void
Client::fail (const online::HttpResponse& resp, const std::string& context) const
{
    std::string code, message;
    try
    {
        auto v = json::parse (resp.body);
        code = v.get_string ("code", v.get_string ("error"));
        message = v.get_string ("message", v.get_string ("error_description"));
        if (auto errs = v.find ("errors"); errs && errs->is_array () &&
            !errs->as_array ().empty ())
        {
            const auto& first = errs->as_array ().front ();
            code = first.get_string ("code", code);
            message = first.get_string ("message", message);
        }
    }
    catch (const std::exception&) {}

    if (code.empty () && resp.status == 401)
        code = "INVALID_CREDENTIALS";
    if (code.empty () && resp.status == 429)
        code = "MESSAGE_THROTTLED_OUT";
    auto text = describe_error (code, message);
    if (text.empty ())
        text = "HMRC returned HTTP " + std::to_string (resp.status) + ".";
    throw OnlineError (context + ": " + text, resp.status, code);
}

Tokens
Client::token_request (const std::vector<std::pair<std::string, std::string>>& form,
                       int64_t now) const
{
    online::HttpRequest req;
    req.method = "POST";
    req.url = m_cfg.base_url () + "/oauth/token";
    req.headers = {{"Content-Type", "application/x-www-form-urlencoded"},
                   {"Accept", "application/json"}};
    req.body = online::form_encode (form);
    auto resp = m_http.send (req);
    if (!resp.ok ())
        fail (resp, "Signing in to HMRC");
    auto v = json::parse (resp.body);
    Tokens t;
    t.access_token = v.get_string ("access_token");
    t.refresh_token = v.get_string ("refresh_token");
    t.scope = v.get_string ("scope");
    int64_t expires_in = 14400;
    if (auto e = v.find ("expires_in"); e && e->is_number ())
        expires_in = e->as_int ();
    t.expires_at = now + expires_in;
    if (t.access_token.empty ())
        throw OnlineError ("HMRC did not return an access token.");
    return t;
}

Tokens
Client::exchange_code (const std::string& code, int64_t now) const
{
    return token_request ({{"grant_type", "authorization_code"},
                           {"client_id", m_cfg.client_id},
                           {"client_secret", m_cfg.client_secret},
                           {"redirect_uri", m_cfg.redirect_uri},
                           {"code", code}}, now);
}

Tokens
Client::refresh (const std::string& refresh_token, int64_t now) const
{
    return token_request ({{"grant_type", "refresh_token"},
                           {"client_id", m_cfg.client_id},
                           {"client_secret", m_cfg.client_secret},
                           {"refresh_token", refresh_token}}, now);
}

std::string
Client::application_token () const
{
    return token_request ({{"grant_type", "client_credentials"},
                           {"client_id", m_cfg.client_id},
                           {"client_secret", m_cfg.client_secret},
                           {"scope", "write:vat read:vat"}}, 0).access_token;
}

online::HttpResponse
Client::call (const std::string& method, const std::string& path,
              const std::string& token, const std::string& body,
              const Headers& extra) const
{
    online::HttpRequest req;
    req.method = method;
    req.url = m_cfg.base_url () + path;
    req.headers.emplace_back ("Accept", VAT_ACCEPT);
    req.headers.emplace_back ("Authorization", "Bearer " + token);
    if (!body.empty ())
        req.headers.emplace_back ("Content-Type", "application/json");
    if (m_env)
        for (auto& h : fraud_prevention_headers (m_env ()))
            req.headers.push_back (std::move (h));
    for (const auto& h : extra)
        req.headers.push_back (h);
    req.body = body;
    return m_http.send (req);
}

static bool
is_not_found (const online::HttpResponse& resp)
{
    return resp.status == 404;
}

std::vector<Obligation>
Client::obligations (const std::string& token, const std::string& vrn,
                     const std::string& from, const std::string& to,
                     const std::string& status) const
{
    std::vector<std::pair<std::string, std::string>> q;
    if (!from.empty ()) q.emplace_back ("from", from);
    if (!to.empty ()) q.emplace_back ("to", to);
    if (!status.empty ()) q.emplace_back ("status", status);
    auto path = "/organisations/vat/" + url_encode (vrn) + "/obligations";
    if (!q.empty ())
        path += "?" + online::form_encode (q);

    auto resp = call ("GET", path, token);
    if (is_not_found (resp))
        return {};
    if (!resp.ok ())
        fail (resp, "Fetching VAT obligations");

    std::vector<Obligation> out;
    auto v = json::parse (resp.body);
    if (auto arr = v.find ("obligations"); arr && arr->is_array ())
        for (const auto& o : arr->as_array ())
            out.push_back ({o.get_string ("periodKey"), o.get_string ("start"),
                            o.get_string ("end"), o.get_string ("due"),
                            o.get_string ("status"), o.get_string ("received")});
    return out;
}

Receipt
Client::submit_return (const std::string& token, const std::string& vrn,
                       const ReturnPayload& payload) const
{
    if (!payload.finalised)
        throw OnlineError (describe_error ("NOT_FINALISED", ""));
    if (auto problem = payload.validate (); !problem.empty ())
        throw OnlineError (problem);

    auto resp = call ("POST", "/organisations/vat/" + url_encode (vrn) + "/returns",
                      token, payload.to_json ().dump ());
    if (!resp.ok ())
        fail (resp, "Submitting the VAT return");

    auto v = json::parse (resp.body);
    Receipt r;
    r.processing_date = v.get_string ("processingDate");
    r.form_bundle_number = v.get_string ("formBundleNumber");
    r.payment_indicator = v.get_string ("paymentIndicator");
    r.charge_ref_number = v.get_string ("chargeRefNumber");
    r.receipt_id = resp.header ("Receipt-ID");
    r.correlation_id = resp.header ("X-CorrelationId");
    r.receipt_timestamp = resp.header ("Receipt-Timestamp");
    r.receipt_signature = resp.header ("Receipt-Signature");
    return r;
}

ReturnPayload
Client::view_return (const std::string& token, const std::string& vrn,
                     const std::string& period_key) const
{
    auto resp = call ("GET", "/organisations/vat/" + url_encode (vrn) +
                      "/returns/" + url_encode (period_key), token);
    if (!resp.ok ())
        fail (resp, "Retrieving the submitted return");
    return ReturnPayload::from_json (json::parse (resp.body));
}

std::vector<Liability>
Client::liabilities (const std::string& token, const std::string& vrn,
                     const std::string& from, const std::string& to) const
{
    auto resp = call ("GET", "/organisations/vat/" + url_encode (vrn) +
                      "/liabilities?" + online::form_encode ({{"from", from}, {"to", to}}),
                      token);
    if (is_not_found (resp))
        return {};
    if (!resp.ok ())
        fail (resp, "Fetching VAT liabilities");
    std::vector<Liability> out;
    auto v = json::parse (resp.body);
    if (auto arr = v.find ("liabilities"); arr && arr->is_array ())
        for (const auto& l : arr->as_array ())
        {
            const auto& period = l["taxPeriod"];
            out.push_back ({period.get_string ("from"), period.get_string ("to"),
                            l.get_string ("type"), l.get_string ("due"),
                            l.get_string ("originalAmount"),
                            l.get_string ("outstandingAmount", "0")});
        }
    return out;
}

std::vector<Payment>
Client::payments (const std::string& token, const std::string& vrn,
                  const std::string& from, const std::string& to) const
{
    auto resp = call ("GET", "/organisations/vat/" + url_encode (vrn) +
                      "/payments?" + online::form_encode ({{"from", from}, {"to", to}}),
                      token);
    if (is_not_found (resp))
        return {};
    if (!resp.ok ())
        fail (resp, "Fetching VAT payments");
    std::vector<Payment> out;
    auto v = json::parse (resp.body);
    if (auto arr = v.find ("payments"); arr && arr->is_array ())
        for (const auto& p : arr->as_array ())
            out.push_back ({p.get_string ("amount"), p.get_string ("received")});
    return out;
}

json::Value
Client::validate_fraud_headers (const std::string& app_token) const
{
    auto resp = call ("GET", "/test/fraud-prevention-headers/validate", app_token);
    if (!resp.ok ())
        fail (resp, "Validating fraud prevention headers");
    return json::parse (resp.body);
}

TestOrganisation
Client::create_test_organisation (const std::string& app_token) const
{
    auto body = json::Value::object ();
    body.set ("serviceNames", json::Array{json::Value ("mtd-vat")});
    auto resp = call ("POST", "/create-test-user/organisations", app_token, body.dump ());
    if (!resp.ok ())
        fail (resp, "Creating a sandbox test organisation");
    auto v = json::parse (resp.body);
    return {v.get_string ("userId"), v.get_string ("password"),
            v.get_string ("vrn"), v["organisationDetails"].get_string ("name")};
}

/* ---------------------------------------------------------------- */
/* Session                                                           */

Session::Session (online::HttpTransport& http, online::SecretStore& secrets,
                  EnvironmentProvider env, bool sandbox)
    : m_http{http}, m_secrets{secrets}, m_env{std::move (env)}
{
    m_cfg.sandbox = sandbox;
    if (auto stored = m_secrets.load (service (), "config"))
    {
        try
        {
            m_cfg = Config::from_json (json::parse (*stored));
            m_cfg.sandbox = sandbox;
        }
        catch (const std::exception&) {}
    }
    if (auto stored = m_secrets.load (service (), "tokens"))
    {
        try { m_tokens = Tokens::from_json (json::parse (*stored)); }
        catch (const std::exception&) {}
    }
}

std::string
Session::service () const
{
    return "hmrc-mtd-" + m_cfg.environment_name ();
}

void
Session::save_config ()
{
    auto v = m_cfg.to_json ();
    v.set ("client_secret", m_cfg.client_secret);
    m_secrets.store (service (), "config",
                     "GnuCash HMRC application (" + m_cfg.environment_name () + ")",
                     v.dump ());
}

void
Session::sign_out ()
{
    m_tokens = {};
    m_secrets.erase (service (), "tokens");
}

void
Session::complete_sign_in (const std::string& code, int64_t now)
{
    m_tokens = client ().exchange_code (code, now);
    m_secrets.store (service (), "tokens",
                     "GnuCash HMRC sign-in (" + m_cfg.environment_name () + ")",
                     m_tokens.to_json ().dump ());
}

std::string
Session::access_token (int64_t now)
{
    if (m_tokens.valid_at (now))
        return m_tokens.access_token;
    if (m_tokens.refresh_token.empty ())
        throw OnlineError (describe_error ("MISSING_CREDENTIALS", ""), 401,
                           "MISSING_CREDENTIALS");
    try
    {
        auto fresh = client ().refresh (m_tokens.refresh_token, now);
        if (fresh.refresh_token.empty ())
            fresh.refresh_token = m_tokens.refresh_token;
        m_tokens = fresh;
        m_secrets.store (service (), "tokens",
                         "GnuCash HMRC sign-in (" + m_cfg.environment_name () + ")",
                         m_tokens.to_json ().dump ());
    }
    catch (const OnlineError& e)
    {
        if (e.status () == 400 || e.status () == 401)
            sign_out ();
        throw;
    }
    return m_tokens.access_token;
}

Client
Session::client () const
{
    return Client (m_http, m_cfg, m_env);
}

} // namespace gnc::hmrc
