/********************************************************************\
 * gnc-online-book.hpp -- book-level settings for online services   *
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
/** @file gnc-online-book.hpp
 *  @brief Non-secret, per-book state (account mappings, sync cursors,
 *  filed receipts) is kept as JSON documents in the book's KVP so it
 *  travels with the file and survives every backend.
 */
#ifndef GNC_ONLINE_BOOK_HPP
#define GNC_ONLINE_BOOK_HPP

#include "gnc-json.hpp"

#include <qofbook.h>
#include <Account.h>
#include <gnc-numeric.h>

#include <optional>
#include <string>

namespace gnc::online
{

/** Read the JSON document stored under the slot @a key ("online/<key>").
 *  Returns an empty object when absent or unreadable. */
json::Value load_book_json (QofBook* book, const std::string& key);
/** Replace the document under @a key and mark the book dirty. */
void save_book_json (QofBook* book, const std::string& key, const json::Value& doc);

/** Parse a plain decimal ("-12.5", "1043.27") exactly. */
std::optional<gnc_numeric> parse_decimal (const std::string& text);

std::string account_guid_string (const Account* acc);
Account* account_from_guid_string (QofBook* book, const std::string& guid);

} // namespace gnc::online

#endif /* GNC_ONLINE_BOOK_HPP */
