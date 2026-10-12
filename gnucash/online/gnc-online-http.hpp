/********************************************************************\
 * gnc-online-http.hpp -- HTTP transport for online services        *
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
/** @file gnc-online-http.hpp
 *  @brief The thin HTTP seam that the HMRC and Plaid clients speak through.
 *
 *  Clients take an HttpTransport by reference so that the protocol
 *  logic can be exercised in tests without a network.
 */
#ifndef GNC_ONLINE_HTTP_HPP
#define GNC_ONLINE_HTTP_HPP

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gnc::online
{

/** Raised for transport failures and for service errors that the
 *  clients translate into human-readable messages. */
class OnlineError : public std::runtime_error
{
public:
    explicit OnlineError (const std::string& msg, int status = 0,
                          std::string code = "")
        : std::runtime_error (msg), m_status{status}, m_code{std::move (code)} {}
    int status () const { return m_status; }
    /** The service's machine-readable error code, when it gave one. */
    const std::string& code () const { return m_code; }
private:
    int m_status;
    std::string m_code;
};

struct HttpRequest
{
    std::string method = "GET";
    std::string url;
    /** Ordered; header names are sent as given. */
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    const std::string* header (const std::string& name) const;
};

struct HttpResponse
{
    int status = 0;
    /** Header names are folded to lower case. */
    std::map<std::string, std::string> headers;
    std::string body;

    bool ok () const { return status >= 200 && status < 300; }
    std::string header (const std::string& name) const;
};

class HttpTransport
{
public:
    virtual ~HttpTransport () = default;
    /** Perform the request. Throws OnlineError only when no HTTP
     *  response was obtained at all; HTTP error statuses are returned. */
    virtual HttpResponse send (const HttpRequest& req) = 0;
};

/** The production transport, backed by libcurl. Thread-compatible:
 *  one instance may be used from one thread at a time. */
std::unique_ptr<HttpTransport> make_default_transport ();

/** RFC 3986 percent-encoding of everything but unreserved characters. */
std::string url_encode (const std::string& s);
std::string url_decode (const std::string& s);
/** Encode key/value pairs as application/x-www-form-urlencoded. */
std::string form_encode (const std::vector<std::pair<std::string, std::string>>& kv);
/** Parse the query component of a URI ("a=1&b=2", leading '?' optional,
 *  anything up to and including the first '?' is ignored). */
std::map<std::string, std::string> parse_query (const std::string& uri);

} // namespace gnc::online

#endif /* GNC_ONLINE_HTTP_HPP */
