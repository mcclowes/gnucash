/********************************************************************\
 * gnc-json.hpp -- A small, exact JSON value type for online services *
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
/** @file gnc-json.hpp
 *  @brief A minimal JSON document model for the online services.
 *
 *  Money must never pass through a double. Numbers are therefore kept
 *  as their literal text, both when parsed and when written, so that
 *  "1234.56" leaves GnuCash exactly as it was computed. Object member
 *  order is preserved, which keeps request bodies readable and stable
 *  for tests.
 */
#ifndef GNC_JSON_HPP
#define GNC_JSON_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gnc::json
{

class ParseError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class Value;
using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;
using Object = std::vector<Member>;

class Value
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value () : m_type{Type::Null} {}
    Value (std::nullptr_t) : Value () {}
    Value (bool b) : m_type{Type::Bool}, m_bool{b} {}
    Value (int i) : m_type{Type::Number}, m_text{std::to_string (i)} {}
    Value (int64_t i) : m_type{Type::Number}, m_text{std::to_string (i)} {}
    Value (const char* s) : m_type{Type::String}, m_text{s ? s : ""} {}
    Value (std::string s) : m_type{Type::String}, m_text{std::move (s)} {}
    Value (Array a);
    Value (Object o);

    /** A number given by its literal text, e.g. "1234.56". The text is
     *  validated against the JSON number grammar. */
    static Value number (const std::string& literal);
    static Value array () { return Value (Array{}); }
    static Value object () { return Value (Object{}); }

    Type type () const { return m_type; }
    bool is_null () const { return m_type == Type::Null; }
    bool is_bool () const { return m_type == Type::Bool; }
    bool is_number () const { return m_type == Type::Number; }
    bool is_string () const { return m_type == Type::String; }
    bool is_array () const { return m_type == Type::Array; }
    bool is_object () const { return m_type == Type::Object; }

    bool as_bool () const;
    /** The literal text of a number, or the contents of a string. */
    const std::string& as_string () const;
    int64_t as_int () const;
    const Array& as_array () const;
    Array& as_array ();
    const Object& as_object () const;
    Object& as_object ();

    /** Object lookup; returns nullptr when absent or not an object. */
    const Value* find (const std::string& key) const;
    /** Object lookup that yields a shared null value when absent. */
    const Value& operator[] (const std::string& key) const;
    /** Convenience: the string member @a key, or @a fallback. */
    std::string get_string (const std::string& key,
                            const std::string& fallback = "") const;
    std::optional<std::string> get_optional_string (const std::string& key) const;
    bool get_bool (const std::string& key, bool fallback = false) const;

    /** Set (or replace) a member of an object. */
    Value& set (const std::string& key, Value v);
    /** Append to an array. */
    Value& push (Value v);
    /** Remove a member of an object, if present. */
    void erase (const std::string& key);

    std::string dump (bool pretty = false) const;

private:
    void dump_to (std::string& out, bool pretty, int indent) const;

    Type m_type;
    bool m_bool = false;
    std::string m_text;
    std::shared_ptr<Array> m_array;
    std::shared_ptr<Object> m_object;
};

/** Parse a complete JSON document. Throws ParseError. */
Value parse (const std::string& text);

/** Escape a string as a JSON string literal, including the quotes. */
std::string quote (const std::string& s);

} // namespace gnc::json

#endif /* GNC_JSON_HPP */
