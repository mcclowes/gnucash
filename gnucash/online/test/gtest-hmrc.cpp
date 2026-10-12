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
\********************************************************************/
#include <config.h>

#include "gnc-hmrc.hpp"
#include "fake-transport.hpp"

#include <gtest/gtest.h>

using namespace gnc;
using namespace gnc::hmrc;

static ClientEnvironment
sample_env ()
{
    ClientEnvironment e;
    e.device_id = "beec798b-b366-47fa-b1f8-92cede14a1ce";
    e.os_user = "alice smith";
    e.os_family = "Linux";
    e.os_version = "6.8.0";
    e.device_manufacturer = "Framework";
    e.device_model = "Laptop 13";
    e.utc_offset = "+01:00";
    e.local_ips = {"192.168.1.20", "fe80::1"};
    e.local_ips_timestamp = "2026-10-12T09:15:00.123Z";
    e.mac_addresses = {"ea:43:1a:5d:21:45"};
    e.screens = {{1920, 1080, 1.0, 24}, {2560, 1440, 1.25, 30}};
    e.window_width = 1256;
    e.window_height = 803;
    e.product_version = "5.13";
    return e;
}

static std::string
header (const Headers& h, const std::string& name)
{
    for (const auto& [k, v] : h)
        if (k == name)
            return v;
    return "<absent>";
}

TEST (HmrcFraudHeaders, desktop_app_direct_set_is_well_formed)
{
    auto h = fraud_prevention_headers (sample_env ());
    EXPECT_EQ ("DESKTOP_APP_DIRECT", header (h, "Gov-Client-Connection-Method"));
    EXPECT_EQ ("beec798b-b366-47fa-b1f8-92cede14a1ce", header (h, "Gov-Client-Device-ID"));
    EXPECT_EQ ("os=alice%20smith", header (h, "Gov-Client-User-IDs"));
    EXPECT_EQ ("UTC+01:00", header (h, "Gov-Client-Timezone"));
    EXPECT_EQ ("192.168.1.20,fe80%3A%3A1", header (h, "Gov-Client-Local-IPs"));
    EXPECT_EQ ("2026-10-12T09:15:00.123Z", header (h, "Gov-Client-Local-IPs-Timestamp"));
    EXPECT_EQ ("ea%3A43%3A1a%3A5d%3A21%3A45", header (h, "Gov-Client-MAC-Addresses"));
    EXPECT_EQ ("width=1920&height=1080&scaling-factor=1&colour-depth=24,"
               "width=2560&height=1440&scaling-factor=1.25&colour-depth=30",
               header (h, "Gov-Client-Screens"));
    EXPECT_EQ ("width=1256&height=803", header (h, "Gov-Client-Window-Size"));
    EXPECT_EQ ("os-family=Linux&os-version=6.8.0&device-manufacturer=Framework"
               "&device-model=Laptop%2013", header (h, "Gov-Client-User-Agent"));
    EXPECT_EQ ("GnuCash=5.13", header (h, "Gov-Vendor-Version"));
    EXPECT_EQ ("GnuCash", header (h, "Gov-Vendor-Product-Name"));
    // We have no multi-factor or licence data: those are omitted, not blank.
    EXPECT_EQ ("<absent>", header (h, "Gov-Client-Multi-Factor"));
    EXPECT_EQ ("<absent>", header (h, "Gov-Vendor-License-IDs"));
}

TEST (HmrcFraudHeaders, unknown_values_are_omitted)
{
    ClientEnvironment e;
    e.device_id = "x";
    auto h = fraud_prevention_headers (e);
    EXPECT_EQ ("<absent>", header (h, "Gov-Client-MAC-Addresses"));
    EXPECT_EQ ("<absent>", header (h, "Gov-Client-Local-IPs-Timestamp"));
    EXPECT_EQ ("<absent>", header (h, "Gov-Client-Window-Size"));
}

static ReturnPayload
sample_payload ()
{
    ReturnPayload p;
    p.period_key = "24A1";
    p.vat_due_sales = "2000.00";
    p.vat_due_acquisitions = "0.00";
    p.total_vat_due = "2000.00";
    p.vat_reclaimed_curr_period = "349.75";
    p.net_vat_due = "1650.25";
    p.total_value_sales_ex_vat = "10000";
    p.total_value_purchases_ex_vat = "1748";
    p.total_value_goods_supplied_ex_vat = "0";
    p.total_acquisitions_ex_vat = "0";
    p.finalised = true;
    return p;
}

TEST (HmrcReturn, payload_serialises_numbers_exactly)
{
    EXPECT_EQ (R"({"periodKey":"24A1","vatDueSales":2000.00,"vatDueAcquisitions":0.00,)"
               R"("totalVatDue":2000.00,"vatReclaimedCurrPeriod":349.75,"netVatDue":1650.25,)"
               R"("totalValueSalesExVAT":10000,"totalValuePurchasesExVAT":1748,)"
               R"("totalValueGoodsSuppliedExVAT":0,"totalAcquisitionsExVAT":0,"finalised":true})",
               sample_payload ().to_json ().dump ());
}

TEST (HmrcReturn, validation_catches_inconsistent_boxes)
{
    EXPECT_EQ ("", sample_payload ().validate ());

    auto p = sample_payload ();
    p.total_vat_due = "1999.99";
    EXPECT_NE ("", p.validate ());

    p = sample_payload ();
    p.net_vat_due = "1650.24";
    EXPECT_NE ("", p.validate ());

    p = sample_payload ();
    p.total_value_sales_ex_vat = "10000.50";
    EXPECT_NE ("", p.validate ());

    p = sample_payload ();
    p.period_key = "";
    EXPECT_NE ("", p.validate ());

    // A repayment: Box 4 exceeds Box 3, Box 5 is the positive difference.
    p = sample_payload ();
    p.vat_reclaimed_curr_period = "2500.00";
    p.net_vat_due = "500.00";
    EXPECT_EQ ("", p.validate ());
}

static Client
make_client (FakeTransport& t)
{
    Config cfg;
    cfg.client_id = "cid";
    cfg.client_secret = "csecret";
    cfg.sandbox = true;
    return Client (t, cfg, sample_env);
}

TEST (HmrcClient, authorize_url_requests_vat_scopes)
{
    FakeTransport t;
    auto url = make_client (t).authorize_url ("st8");
    EXPECT_EQ (0u, url.find ("https://test-api.service.hmrc.gov.uk/oauth/authorize?"));
    EXPECT_NE (std::string::npos, url.find ("scope=read%3Avat%20write%3Avat"));
    EXPECT_NE (std::string::npos, url.find ("client_id=cid"));
    EXPECT_NE (std::string::npos, url.find ("state=st8"));
    EXPECT_NE (std::string::npos,
               url.find ("redirect_uri=http%3A%2F%2Flocalhost%3A8765%2Fgnucash%2Fhmrc-callback"));
}

TEST (HmrcClient, exchanges_code_for_tokens)
{
    FakeTransport t;
    t.reply ("/oauth/token", 200,
             R"({"access_token":"AT","refresh_token":"RT","expires_in":14400,"scope":"read:vat write:vat","token_type":"bearer"})");
    auto tokens = make_client (t).exchange_code ("the-code", 1000);
    EXPECT_EQ ("AT", tokens.access_token);
    EXPECT_EQ ("RT", tokens.refresh_token);
    EXPECT_EQ (15400, tokens.expires_at);
    ASSERT_EQ (1u, t.requests.size ());
    const auto& req = t.requests[0];
    EXPECT_EQ ("POST", req.method);
    EXPECT_NE (std::string::npos, req.body.find ("grant_type=authorization_code"));
    EXPECT_NE (std::string::npos, req.body.find ("code=the-code"));
    EXPECT_NE (std::string::npos, req.body.find ("client_secret=csecret"));
}

TEST (HmrcClient, obligations_send_fraud_headers_and_parse)
{
    FakeTransport t;
    t.reply ("/organisations/vat/123456789/obligations", 200, R"({"obligations":[
        {"periodKey":"24A1","start":"2024-01-01","end":"2024-03-31","due":"2024-05-07","status":"F","received":"2024-05-06"},
        {"periodKey":"24A2","start":"2024-04-01","end":"2024-06-30","due":"2024-08-07","status":"O"}]})");
    auto obs = make_client (t).obligations ("AT", "123456789", "2024-01-01", "2024-12-31");
    ASSERT_EQ (2u, obs.size ());
    EXPECT_EQ ("24A1", obs[0].period_key);
    EXPECT_FALSE (obs[0].open ());
    EXPECT_TRUE (obs[1].open ());
    EXPECT_EQ ("2024-06-30", obs[1].end);

    const auto& req = t.requests[0];
    EXPECT_NE (std::string::npos, req.url.find ("from=2024-01-01&to=2024-12-31"));
    ASSERT_TRUE (req.header ("Accept"));
    EXPECT_EQ ("application/vnd.hmrc.1.0+json", *req.header ("Accept"));
    EXPECT_EQ ("Bearer AT", *req.header ("Authorization"));
    ASSERT_TRUE (req.header ("Gov-Client-Connection-Method"));
    EXPECT_EQ ("DESKTOP_APP_DIRECT", *req.header ("Gov-Client-Connection-Method"));
}

TEST (HmrcClient, no_obligations_is_an_empty_list)
{
    FakeTransport t;
    t.reply ("/obligations", 404, R"({"code":"NOT_FOUND","message":"The remote endpoint has indicated that no data can be found"})");
    EXPECT_TRUE (make_client (t).obligations ("AT", "123456789", "", "", "O").empty ());
}

TEST (HmrcClient, submits_return_and_keeps_receipt)
{
    FakeTransport t;
    t.reply ("/organisations/vat/123456789/returns", 201,
             R"({"processingDate":"2024-07-15T09:45:17.000Z","paymentIndicator":"DD","formBundleNumber":"256660290587","chargeRefNumber":"aCxFaNx0FZsCvyWF"})",
             {{"receipt-id", "2f0d9c1b"}, {"x-correlationid", "corr-1"},
              {"receipt-timestamp", "2024-07-15T09:45:17.000Z"}, {"receipt-signature", "sig"}});
    auto r = make_client (t).submit_return ("AT", "123456789", sample_payload ());
    EXPECT_EQ ("256660290587", r.form_bundle_number);
    EXPECT_EQ ("aCxFaNx0FZsCvyWF", r.charge_ref_number);
    EXPECT_EQ ("2f0d9c1b", r.receipt_id);
    EXPECT_EQ ("corr-1", r.correlation_id);
    const auto& req = t.requests[0];
    EXPECT_EQ ("POST", req.method);
    EXPECT_EQ ("application/json", *req.header ("Content-Type"));
    EXPECT_NE (std::string::npos, req.body.find (R"("netVatDue":1650.25)"));
}

TEST (HmrcClient, refuses_unfinalised_or_invalid_returns_without_calling)
{
    FakeTransport t;
    auto p = sample_payload ();
    p.finalised = false;
    EXPECT_THROW (make_client (t).submit_return ("AT", "123456789", p), online::OnlineError);
    p = sample_payload ();
    p.net_vat_due = "1.00";
    EXPECT_THROW (make_client (t).submit_return ("AT", "123456789", p), online::OnlineError);
    EXPECT_TRUE (t.requests.empty ());
}

TEST (HmrcClient, errors_carry_hmrc_codes_and_plain_messages)
{
    FakeTransport t;
    t.reply ("/returns", 403, R"({"code":"BUSINESS_ERROR","message":"Business validation error","errors":[{"code":"DUPLICATE_SUBMISSION","message":"The VAT return was already submitted for the given period."}]})");
    try
    {
        make_client (t).submit_return ("AT", "123456789", sample_payload ());
        FAIL () << "expected an error";
    }
    catch (const online::OnlineError& e)
    {
        EXPECT_EQ ("DUPLICATE_SUBMISSION", e.code ());
        EXPECT_EQ (403, e.status ());
        EXPECT_NE (std::string::npos, std::string (e.what ()).find ("already been submitted"));
    }
}

TEST (HmrcSession, refreshes_expired_tokens_and_persists_them)
{
    FakeTransport t;
    online::MemorySecretStore secrets;
    {
        Session s (t, secrets, sample_env, true);
        s.config ().client_id = "cid";
        s.config ().client_secret = "csecret";
        s.save_config ();
        t.reply ("/oauth/token", 200, R"({"access_token":"AT1","refresh_token":"RT1","expires_in":100})");
        s.complete_sign_in ("code", 0);
        EXPECT_EQ ("AT1", s.access_token (10));
    }
    Session again (t, secrets, sample_env, true);
    EXPECT_EQ ("cid", again.config ().client_id);
    EXPECT_EQ ("csecret", again.config ().client_secret);
    EXPECT_TRUE (again.signed_in ());
    t.reply ("/oauth/token", 200, R"({"access_token":"AT2","refresh_token":"RT2","expires_in":14400})");
    EXPECT_EQ ("AT2", again.access_token (1000)); // AT1 expired at 100
    EXPECT_NE (std::string::npos, t.requests.back ().body.find ("refresh_token=RT1"));
    EXPECT_TRUE (t.drained ());
}

TEST (HmrcSession, rejected_refresh_signs_out)
{
    FakeTransport t;
    online::MemorySecretStore secrets;
    secrets.store ("hmrc-mtd-sandbox", "tokens", "",
                   R"({"access_token":"old","refresh_token":"dead","expires_at":5})");
    Session s (t, secrets, sample_env, true);
    t.reply ("/oauth/token", 400, R"({"error":"invalid_grant","error_description":"expired"})");
    EXPECT_THROW (s.access_token (1000), online::OnlineError);
    EXPECT_FALSE (s.signed_in ());
    EXPECT_FALSE (secrets.load ("hmrc-mtd-sandbox", "tokens"));
}

TEST (HmrcSession, production_and_sandbox_are_kept_apart)
{
    FakeTransport t;
    online::MemorySecretStore secrets;
    Session sandbox (t, secrets, sample_env, true);
    sandbox.config ().client_id = "sandbox-id";
    sandbox.save_config ();
    Session live (t, secrets, sample_env, false);
    EXPECT_EQ ("", live.config ().client_id);
    EXPECT_EQ ("https://api.service.hmrc.gov.uk", live.config ().base_url ());
}
