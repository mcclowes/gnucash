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
/* A scripted HttpTransport: responses are queued per URL fragment and
 * every request is kept for inspection. */
#ifndef GNC_ONLINE_FAKE_TRANSPORT_HPP
#define GNC_ONLINE_FAKE_TRANSPORT_HPP

#include "gnc-online-http.hpp"

#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

class FakeTransport : public gnc::online::HttpTransport
{
public:
    /** Queue a response for the next request whose URL contains @a fragment. */
    void reply (const std::string& fragment, int status, const std::string& body,
                std::map<std::string, std::string> headers = {})
    {
        gnc::online::HttpResponse r;
        r.status = status;
        r.body = body;
        r.headers = std::move (headers);
        m_queue.emplace_back (fragment, r);
    }

    gnc::online::HttpResponse send (const gnc::online::HttpRequest& req) override
    {
        requests.push_back (req);
        for (auto it = m_queue.begin (); it != m_queue.end (); ++it)
            if (req.url.find (it->first) != std::string::npos)
            {
                auto r = it->second;
                m_queue.erase (it);
                return r;
            }
        throw gnc::online::OnlineError ("unexpected request: " + req.url);
    }

    bool drained () const { return m_queue.empty (); }

    std::vector<gnc::online::HttpRequest> requests;

private:
    std::deque<std::pair<std::string, gnc::online::HttpResponse>> m_queue;
};

#endif
