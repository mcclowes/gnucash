/********************************************************************\
 * gnc-online-http.cpp -- HTTP transport for online services        *
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

#include "gnc-online-http.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>

#include <curl/curl.h>

namespace gnc::online
{

static std::string
lower (std::string s)
{
    std::transform (s.begin (), s.end (), s.begin (),
                    [](unsigned char c) { return std::tolower (c); });
    return s;
}

const std::string*
HttpRequest::header (const std::string& name) const
{
    auto want = lower (name);
    for (const auto& [k, v] : headers)
        if (lower (k) == want)
            return &v;
    return nullptr;
}

std::string
HttpResponse::header (const std::string& name) const
{
    auto it = headers.find (lower (name));
    return it == headers.end () ? std::string{} : it->second;
}

std::string
url_encode (const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (std::isalnum (c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char> (c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::string
url_decode (const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size (); ++i)
    {
        if (s[i] == '+')
            out += ' ';
        else if (s[i] == '%' && i + 2 < s.size () &&
                 std::isxdigit (static_cast<unsigned char> (s[i + 1])) &&
                 std::isxdigit (static_cast<unsigned char> (s[i + 2])))
        {
            out += static_cast<char> (std::stoi (s.substr (i + 1, 2), nullptr, 16));
            i += 2;
        }
        else
            out += s[i];
    }
    return out;
}

std::string
form_encode (const std::vector<std::pair<std::string, std::string>>& kv)
{
    std::string out;
    for (const auto& [k, v] : kv)
    {
        if (!out.empty ())
            out += '&';
        out += url_encode (k) + '=' + url_encode (v);
    }
    return out;
}

std::map<std::string, std::string>
parse_query (const std::string& uri)
{
    std::map<std::string, std::string> out;
    auto start = uri.find ('?');
    auto q = start == std::string::npos ? uri : uri.substr (start + 1);
    auto hash = q.find ('#');
    if (hash != std::string::npos)
        q = q.substr (0, hash);
    size_t pos = 0;
    while (pos <= q.size ())
    {
        auto amp = q.find ('&', pos);
        auto part = q.substr (pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (!part.empty ())
        {
            auto eq = part.find ('=');
            if (eq == std::string::npos)
                out[url_decode (part)] = "";
            else
                out[url_decode (part.substr (0, eq))] = url_decode (part.substr (eq + 1));
        }
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    return out;
}

namespace
{

size_t
write_body (char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto body = static_cast<std::string*> (userdata);
    body->append (ptr, size * nmemb);
    return size * nmemb;
}

size_t
write_header (char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto resp = static_cast<HttpResponse*> (userdata);
    std::string line (ptr, size * nmemb);
    if (line.rfind ("HTTP/", 0) == 0)
        resp->headers.clear (); // a new response (e.g. after a redirect)
    auto colon = line.find (':');
    if (colon != std::string::npos)
    {
        auto name = lower (line.substr (0, colon));
        auto value = line.substr (colon + 1);
        auto b = value.find_first_not_of (" \t");
        auto e = value.find_last_not_of (" \t\r\n");
        resp->headers[name] = (b == std::string::npos) ? "" : value.substr (b, e - b + 1);
    }
    return size * nmemb;
}

class CurlTransport : public HttpTransport
{
public:
    CurlTransport ()
    {
        static std::once_flag once;
        std::call_once (once, [] { curl_global_init (CURL_GLOBAL_DEFAULT); });
    }

    HttpResponse send (const HttpRequest& req) override
    {
        HttpResponse resp;
        std::unique_ptr<CURL, decltype (&curl_easy_cleanup)> curl
            {curl_easy_init (), curl_easy_cleanup};
        if (!curl)
            throw OnlineError ("Unable to initialise the network library.");

        struct curl_slist* hdrs = nullptr;
        for (const auto& [k, v] : req.headers)
            hdrs = curl_slist_append (hdrs, (k + ": " + v).c_str ());
        // Suppress curl's automatic "Expect: 100-continue".
        hdrs = curl_slist_append (hdrs, "Expect:");

        char errbuf[CURL_ERROR_SIZE] = "";
        auto h = curl.get ();
        curl_easy_setopt (h, CURLOPT_URL, req.url.c_str ());
        curl_easy_setopt (h, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt (h, CURLOPT_WRITEFUNCTION, write_body);
        curl_easy_setopt (h, CURLOPT_WRITEDATA, &resp.body);
        curl_easy_setopt (h, CURLOPT_HEADERFUNCTION, write_header);
        curl_easy_setopt (h, CURLOPT_HEADERDATA, &resp);
        curl_easy_setopt (h, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt (h, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt (h, CURLOPT_TIMEOUT, 90L);
        curl_easy_setopt (h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt (h, CURLOPT_USERAGENT, "GnuCash/" PROJECT_VERSION);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt (h, CURLOPT_PROTOCOLS_STR, "https,http");
#endif

        if (req.method == "GET")
            curl_easy_setopt (h, CURLOPT_HTTPGET, 1L);
        else
        {
            curl_easy_setopt (h, CURLOPT_CUSTOMREQUEST, req.method.c_str ());
            curl_easy_setopt (h, CURLOPT_POSTFIELDS, req.body.c_str ());
            curl_easy_setopt (h, CURLOPT_POSTFIELDSIZE_LARGE,
                              static_cast<curl_off_t> (req.body.size ()));
        }

        auto rc = curl_easy_perform (h);
        curl_slist_free_all (hdrs);
        if (rc != CURLE_OK)
            throw OnlineError (std::string ("Network error: ") +
                               (*errbuf ? errbuf : curl_easy_strerror (rc)));
        long status = 0;
        curl_easy_getinfo (h, CURLINFO_RESPONSE_CODE, &status);
        resp.status = static_cast<int> (status);
        return resp;
    }
};

} // anonymous namespace

std::unique_ptr<HttpTransport>
make_default_transport ()
{
    return std::make_unique<CurlTransport> ();
}

} // namespace gnc::online
