/********************************************************************\
 * gnc-hmrc.hpp -- HMRC Making Tax Digital (VAT) API client          *
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
/** @file gnc-hmrc.hpp
 *  @brief A direct bridge from GnuCash to HMRC's VAT (MTD) API.
 *
 *  GnuCash connects as a desktop application
 *  (Gov-Client-Connection-Method: DESKTOP_APP_DIRECT). The user
 *  registers their own application on the HMRC Developer Hub and
 *  supplies its client id and secret; nothing passes through a third
 *  party.
 *
 *  Reference: https://developer.service.hmrc.gov.uk/api-documentation/docs/api/service/vat-api/1.0
 *  and the fraud prevention specification at
 *  https://developer.service.hmrc.gov.uk/guides/fraud-prevention/
 */
#ifndef GNC_HMRC_HPP
#define GNC_HMRC_HPP

#include "gnc-json.hpp"
#include "gnc-online-http.hpp"
#include "gnc-online-secrets.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnc::hmrc
{

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Config
{
    std::string client_id;
    std::string client_secret;
    bool sandbox = true;
    /** Must match a redirect URI registered for the application. The
     *  sign-in window intercepts navigation to it, so nothing needs to
     *  listen on this address. */
    std::string redirect_uri = "http://localhost:8765/gnucash/hmrc-callback";

    std::string base_url () const;
    std::string environment_name () const { return sandbox ? "sandbox" : "production"; }
    bool complete () const { return !client_id.empty () && !client_secret.empty (); }

    json::Value to_json () const; // excludes the secret
    static Config from_json (const json::Value&);
};

struct Tokens
{
    std::string access_token;
    std::string refresh_token;
    int64_t expires_at = 0; ///< Unix time.
    std::string scope;

    bool valid_at (int64_t now) const { return !access_token.empty () && now + 60 < expires_at; }
    json::Value to_json () const;
    static Tokens from_json (const json::Value&);
};

struct Obligation
{
    std::string period_key;
    std::string start;  ///< YYYY-MM-DD
    std::string end;
    std::string due;
    std::string status; ///< "O" open, "F" fulfilled
    std::string received;

    bool open () const { return status == "O"; }
};

/** The nine boxes as HMRC wants them: boxes 1-5 with two decimal
 *  places, boxes 6-9 as whole pounds. Values are decimal literals. */
struct ReturnPayload
{
    std::string period_key;
    std::string vat_due_sales;                ///< Box 1
    std::string vat_due_acquisitions;         ///< Box 2
    std::string total_vat_due;                ///< Box 3
    std::string vat_reclaimed_curr_period;    ///< Box 4
    std::string net_vat_due;                  ///< Box 5
    std::string total_value_sales_ex_vat;     ///< Box 6
    std::string total_value_purchases_ex_vat; ///< Box 7
    std::string total_value_goods_supplied_ex_vat; ///< Box 8
    std::string total_acquisitions_ex_vat;    ///< Box 9
    bool finalised = false;

    json::Value to_json () const;
    static ReturnPayload from_json (const json::Value&);
    /** Empty when well formed, else the first problem found. */
    std::string validate () const;
};

struct Receipt
{
    std::string processing_date;
    std::string form_bundle_number;
    std::string payment_indicator;
    std::string charge_ref_number;
    std::string receipt_id;        ///< Receipt-ID header
    std::string correlation_id;    ///< X-CorrelationId header
    std::string receipt_timestamp; ///< Receipt-Timestamp header
    std::string receipt_signature; ///< Receipt-Signature header

    json::Value to_json () const;
    static Receipt from_json (const json::Value&);
};

struct Liability
{
    std::string from, to, type, due;
    std::string original_amount, outstanding_amount;
};

struct Payment
{
    std::string amount, received;
};

struct TestOrganisation
{
    std::string user_id, password, vrn, name;
};

/** Facts about the machine, gathered once per request batch, that the
 *  fraud prevention headers report. */
struct ClientEnvironment
{
    struct Screen { int width = 0, height = 0; double scale = 1.0; int depth = 24; };

    std::string device_id;
    std::string os_user;
    std::string os_family;
    std::string os_version;
    std::string device_manufacturer;
    std::string device_model;
    std::string utc_offset;            ///< "+01:00"
    std::vector<std::string> local_ips;
    std::string local_ips_timestamp;   ///< yyyy-MM-ddThh:mm:ss.sssZ
    std::vector<std::string> mac_addresses;
    std::vector<Screen> screens;
    int window_width = 0, window_height = 0;
    std::string product_name = "GnuCash";
    std::string product_version;
};

/** Build the DESKTOP_APP_DIRECT header set from @a env. */
Headers fraud_prevention_headers (const ClientEnvironment& env);

/** Fill everything the operating system can tell us. Screen and window
 *  sizes are left for the GUI to supply. */
ClientEnvironment collect_system_environment ();

/** A stable random identifier for this installation, created on first
 *  use and kept in the user data directory. */
std::string device_id ();

using EnvironmentProvider = std::function<ClientEnvironment ()>;

class Client
{
public:
    Client (online::HttpTransport& http, Config cfg, EnvironmentProvider env);

    const Config& config () const { return m_cfg; }

    /** The Government Gateway sign-in URL. */
    std::string authorize_url (const std::string& state) const;
    Tokens exchange_code (const std::string& code, int64_t now) const;
    Tokens refresh (const std::string& refresh_token, int64_t now) const;
    /** An application-restricted token (client credentials grant). */
    std::string application_token () const;

    std::vector<Obligation> obligations (const std::string& token,
                                         const std::string& vrn,
                                         const std::string& from,
                                         const std::string& to,
                                         const std::string& status = "") const;
    Receipt submit_return (const std::string& token, const std::string& vrn,
                           const ReturnPayload& payload) const;
    ReturnPayload view_return (const std::string& token, const std::string& vrn,
                               const std::string& period_key) const;
    std::vector<Liability> liabilities (const std::string& token,
                                        const std::string& vrn,
                                        const std::string& from,
                                        const std::string& to) const;
    std::vector<Payment> payments (const std::string& token,
                                   const std::string& vrn,
                                   const std::string& from,
                                   const std::string& to) const;

    /** Sandbox only: ask HMRC to check our fraud prevention headers.
     *  Returns the raw feedback document. */
    json::Value validate_fraud_headers (const std::string& app_token) const;
    /** Sandbox only: create a test organisation registered for VAT. */
    TestOrganisation create_test_organisation (const std::string& app_token) const;

private:
    online::HttpResponse call (const std::string& method, const std::string& path,
                               const std::string& token,
                               const std::string& body = "",
                               const Headers& extra = {}) const;
    Tokens token_request (const std::vector<std::pair<std::string, std::string>>& form,
                          int64_t now) const;
    [[noreturn]] void fail (const online::HttpResponse& resp,
                            const std::string& context) const;

    online::HttpTransport& m_http;
    Config m_cfg;
    EnvironmentProvider m_env;
};

/** Pulls together config, the secret store and token refresh. The
 *  credentials live in the secret store under the service name
 *  "hmrc-mtd-<environment>". */
class Session
{
public:
    Session (online::HttpTransport& http, online::SecretStore& secrets,
             EnvironmentProvider env, bool sandbox);

    Config& config () { return m_cfg; }
    const Config& config () const { return m_cfg; }
    void save_config ();

    bool signed_in () const { return !m_tokens.refresh_token.empty (); }
    void sign_out ();
    /** Store tokens obtained from the authorization code flow. */
    void complete_sign_in (const std::string& code, int64_t now);
    /** A usable access token, refreshing it if need be. Throws
     *  OnlineError when the user must sign in again. */
    std::string access_token (int64_t now);

    Client client () const;

private:
    std::string service () const;

    online::HttpTransport& m_http;
    online::SecretStore& m_secrets;
    EnvironmentProvider m_env;
    Config m_cfg;
    Tokens m_tokens;
};

/** Human wording for the error codes HMRC documents for this API. */
std::string describe_error (const std::string& code, const std::string& fallback);

} // namespace gnc::hmrc

#endif /* GNC_HMRC_HPP */
