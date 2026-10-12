/********************************************************************\
 * gnc-online-book.cpp -- book-level settings for online services   *
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

#include "gnc-online-book.hpp"

#include <glib.h>
#include <cstdint>
#include <qofinstance-p.h>
#include <qoflog.h>

G_GNUC_UNUSED static const char* log_module = "gnc.online";

namespace gnc::online
{

json::Value
load_book_json (QofBook* book, const std::string& key)
{
    if (!book)
        return json::Value::object ();
    auto text = qof_instance_get_path_kvp<const char*> (QOF_INSTANCE (book),
                                                        {"online", key});
    if (!text || !*text || !**text)
        return json::Value::object ();
    try
    {
        auto v = json::parse (*text);
        if (v.is_object ())
            return v;
    }
    catch (const json::ParseError& e)
    {
        PWARN ("Discarding unreadable online/%s: %s", key.c_str (), e.what ());
    }
    return json::Value::object ();
}

void
save_book_json (QofBook* book, const std::string& key, const json::Value& doc)
{
    g_return_if_fail (book);
    std::optional<const char*> val{g_strdup (doc.dump ().c_str ())};
    qof_book_begin_edit (book);
    qof_instance_set_path_kvp<const char*> (QOF_INSTANCE (book), val, {"online", key});
    qof_instance_set_dirty (QOF_INSTANCE (book));
    qof_book_commit_edit (book);
}

std::optional<gnc_numeric>
parse_decimal (const std::string& text)
{
    size_t i = 0, n = text.size ();
    bool neg = false;
    if (i < n && (text[i] == '-' || text[i] == '+'))
        neg = text[i++] == '-';
    int64_t num = 0, denom = 1;
    bool digits = false, point = false;
    for (; i < n; ++i)
    {
        char c = text[i];
        if (c == '.' && !point)
        {
            point = true;
            continue;
        }
        if (c < '0' || c > '9')
            return std::nullopt;
        if (num > (INT64_MAX - 9) / 10 || (point && denom > INT64_MAX / 10))
            return std::nullopt;
        num = num * 10 + (c - '0');
        if (point)
            denom *= 10;
        digits = true;
    }
    if (!digits)
        return std::nullopt;
    return gnc_numeric_create (neg ? -num : num, denom);
}

std::string
account_guid_string (const Account* acc)
{
    if (!acc)
        return "";
    char buf[GUID_ENCODING_LENGTH + 1];
    guid_to_string_buff (qof_entity_get_guid (QOF_INSTANCE (acc)), buf);
    return buf;
}

Account*
account_from_guid_string (QofBook* book, const std::string& guid)
{
    GncGUID g;
    if (guid.empty () || !string_to_guid (guid.c_str (), &g))
        return nullptr;
    return xaccAccountLookup (&g, book);
}

} // namespace gnc::online
