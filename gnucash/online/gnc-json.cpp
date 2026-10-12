/********************************************************************\
 * gnc-json.cpp -- A small, exact JSON value type for online services *
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

#include "gnc-json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace gnc::json
{

static const Value s_null;

static bool
valid_number_literal (const std::string& s)
{
    size_t i = 0, n = s.size ();
    auto digit = [&](size_t k) { return k < n && s[k] >= '0' && s[k] <= '9'; };
    if (i < n && s[i] == '-') ++i;
    if (!digit (i)) return false;
    if (s[i] == '0') ++i;
    else while (digit (i)) ++i;
    if (i < n && s[i] == '.')
    {
        ++i;
        if (!digit (i)) return false;
        while (digit (i)) ++i;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E'))
    {
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
        if (!digit (i)) return false;
        while (digit (i)) ++i;
    }
    return i == n;
}

Value::Value (Array a)
    : m_type{Type::Array}, m_array{std::make_shared<Array> (std::move (a))} {}

Value::Value (Object o)
    : m_type{Type::Object}, m_object{std::make_shared<Object> (std::move (o))} {}

Value
Value::number (const std::string& literal)
{
    if (!valid_number_literal (literal))
        throw std::invalid_argument ("not a JSON number: " + literal);
    Value v;
    v.m_type = Type::Number;
    v.m_text = literal;
    return v;
}

bool
Value::as_bool () const
{
    if (m_type != Type::Bool)
        throw std::logic_error ("JSON value is not a boolean");
    return m_bool;
}

const std::string&
Value::as_string () const
{
    if (m_type != Type::String && m_type != Type::Number)
        throw std::logic_error ("JSON value is not a string");
    return m_text;
}

int64_t
Value::as_int () const
{
    if (m_type == Type::Number || m_type == Type::String)
    {
        try
        {
            size_t used = 0;
            auto v = std::stoll (m_text, &used);
            if (used == m_text.size ())
                return v;
        }
        catch (const std::exception&) {}
    }
    throw std::logic_error ("JSON value is not an integer");
}

const Array&
Value::as_array () const
{
    if (m_type != Type::Array)
        throw std::logic_error ("JSON value is not an array");
    return *m_array;
}

Array&
Value::as_array ()
{
    if (m_type != Type::Array)
        throw std::logic_error ("JSON value is not an array");
    if (m_array.use_count () > 1)
        m_array = std::make_shared<Array> (*m_array);
    return *m_array;
}

const Object&
Value::as_object () const
{
    if (m_type != Type::Object)
        throw std::logic_error ("JSON value is not an object");
    return *m_object;
}

Object&
Value::as_object ()
{
    if (m_type != Type::Object)
        throw std::logic_error ("JSON value is not an object");
    if (m_object.use_count () > 1)
        m_object = std::make_shared<Object> (*m_object);
    return *m_object;
}

const Value*
Value::find (const std::string& key) const
{
    if (m_type != Type::Object)
        return nullptr;
    for (const auto& [k, v] : *m_object)
        if (k == key)
            return &v;
    return nullptr;
}

const Value&
Value::operator[] (const std::string& key) const
{
    auto v = find (key);
    return v ? *v : s_null;
}

std::string
Value::get_string (const std::string& key, const std::string& fallback) const
{
    auto v = find (key);
    if (v && (v->is_string () || v->is_number ()))
        return v->m_text;
    return fallback;
}

std::optional<std::string>
Value::get_optional_string (const std::string& key) const
{
    auto v = find (key);
    if (v && (v->is_string () || v->is_number ()))
        return v->m_text;
    return std::nullopt;
}

bool
Value::get_bool (const std::string& key, bool fallback) const
{
    auto v = find (key);
    return (v && v->is_bool ()) ? v->m_bool : fallback;
}

Value&
Value::set (const std::string& key, Value v)
{
    auto& obj = as_object ();
    for (auto& [k, existing] : obj)
        if (k == key)
        {
            existing = std::move (v);
            return *this;
        }
    obj.emplace_back (key, std::move (v));
    return *this;
}

Value&
Value::push (Value v)
{
    as_array ().push_back (std::move (v));
    return *this;
}

void
Value::erase (const std::string& key)
{
    auto& obj = as_object ();
    obj.erase (std::remove_if (obj.begin (), obj.end (),
                               [&](const Member& m) { return m.first == key; }),
               obj.end ());
}

std::string
quote (const std::string& s)
{
    std::string out;
    out.reserve (s.size () + 2);
    out += '"';
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                snprintf (buf, sizeof buf, "\\u%04x", c);
                out += buf;
            }
            else
                out += static_cast<char> (c);
        }
    }
    out += '"';
    return out;
}

static void
newline (std::string& out, bool pretty, int indent)
{
    if (!pretty) return;
    out += '\n';
    out.append (indent * 2, ' ');
}

void
Value::dump_to (std::string& out, bool pretty, int indent) const
{
    switch (m_type)
    {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += m_bool ? "true" : "false"; break;
    case Type::Number: out += m_text; break;
    case Type::String: out += quote (m_text); break;
    case Type::Array:
        out += '[';
        for (size_t i = 0; i < m_array->size (); ++i)
        {
            if (i) out += ',';
            newline (out, pretty, indent + 1);
            (*m_array)[i].dump_to (out, pretty, indent + 1);
        }
        if (!m_array->empty ()) newline (out, pretty, indent);
        out += ']';
        break;
    case Type::Object:
        out += '{';
        for (size_t i = 0; i < m_object->size (); ++i)
        {
            if (i) out += ',';
            newline (out, pretty, indent + 1);
            out += quote ((*m_object)[i].first);
            out += pretty ? ": " : ":";
            (*m_object)[i].second.dump_to (out, pretty, indent + 1);
        }
        if (!m_object->empty ()) newline (out, pretty, indent);
        out += '}';
        break;
    }
}

std::string
Value::dump (bool pretty) const
{
    std::string out;
    dump_to (out, pretty, 0);
    return out;
}

namespace
{

class Parser
{
public:
    explicit Parser (const std::string& text) : m_s{text} {}

    Value parse_document ()
    {
        auto v = parse_value (0);
        skip_ws ();
        if (m_pos != m_s.size ())
            fail ("unexpected trailing characters");
        return v;
    }

private:
    static constexpr int max_depth = 256;

    [[noreturn]] void fail (const std::string& what)
    {
        throw ParseError ("JSON parse error at offset " +
                          std::to_string (m_pos) + ": " + what);
    }

    void skip_ws ()
    {
        while (m_pos < m_s.size () &&
               (m_s[m_pos] == ' ' || m_s[m_pos] == '\t' ||
                m_s[m_pos] == '\n' || m_s[m_pos] == '\r'))
            ++m_pos;
    }

    bool consume (const char* lit)
    {
        size_t len = strlen (lit);
        if (m_s.compare (m_pos, len, lit) == 0)
        {
            m_pos += len;
            return true;
        }
        return false;
    }

    Value parse_value (int depth)
    {
        if (depth > max_depth)
            fail ("nesting too deep");
        skip_ws ();
        if (m_pos >= m_s.size ())
            fail ("unexpected end of input");
        char c = m_s[m_pos];
        if (c == '{') return parse_object (depth);
        if (c == '[') return parse_array (depth);
        if (c == '"') return Value (parse_string ());
        if (consume ("true")) return Value (true);
        if (consume ("false")) return Value (false);
        if (consume ("null")) return Value ();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number ();
        fail (std::string ("unexpected character '") + c + "'");
    }

    Value parse_number ()
    {
        size_t start = m_pos;
        while (m_pos < m_s.size () &&
               (isdigit (static_cast<unsigned char> (m_s[m_pos])) ||
                m_s[m_pos] == '-' || m_s[m_pos] == '+' || m_s[m_pos] == '.' ||
                m_s[m_pos] == 'e' || m_s[m_pos] == 'E'))
            ++m_pos;
        auto lit = m_s.substr (start, m_pos - start);
        if (!valid_number_literal (lit))
            fail ("malformed number");
        return Value::number (lit);
    }

    unsigned parse_hex4 ()
    {
        if (m_pos + 4 > m_s.size ())
            fail ("truncated \\u escape");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i)
        {
            char h = m_s[m_pos++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= h - '0';
            else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
            else fail ("bad hex digit in \\u escape");
        }
        return v;
    }

    static void append_utf8 (std::string& out, unsigned cp)
    {
        if (cp < 0x80)
            out += static_cast<char> (cp);
        else if (cp < 0x800)
        {
            out += static_cast<char> (0xC0 | (cp >> 6));
            out += static_cast<char> (0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char> (0xE0 | (cp >> 12));
            out += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char> (0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char> (0xF0 | (cp >> 18));
            out += static_cast<char> (0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char> (0x80 | (cp & 0x3F));
        }
    }

    std::string parse_string ()
    {
        ++m_pos; // opening quote
        std::string out;
        while (true)
        {
            if (m_pos >= m_s.size ())
                fail ("unterminated string");
            unsigned char c = m_s[m_pos++];
            if (c == '"')
                return out;
            if (c < 0x20)
                fail ("control character in string");
            if (c != '\\')
            {
                out += static_cast<char> (c);
                continue;
            }
            if (m_pos >= m_s.size ())
                fail ("unterminated escape");
            char e = m_s[m_pos++];
            switch (e)
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                unsigned cp = parse_hex4 ();
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    if (!consume ("\\u"))
                        fail ("unpaired surrogate");
                    unsigned lo = parse_hex4 ();
                    if (lo < 0xDC00 || lo > 0xDFFF)
                        fail ("invalid low surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                else if (cp >= 0xDC00 && cp <= 0xDFFF)
                    fail ("unpaired surrogate");
                append_utf8 (out, cp);
                break;
            }
            default:
                fail ("invalid escape");
            }
        }
    }

    Value parse_array (int depth)
    {
        ++m_pos;
        Array arr;
        skip_ws ();
        if (m_pos < m_s.size () && m_s[m_pos] == ']')
        {
            ++m_pos;
            return Value (std::move (arr));
        }
        while (true)
        {
            arr.push_back (parse_value (depth + 1));
            skip_ws ();
            if (m_pos >= m_s.size ())
                fail ("unterminated array");
            if (m_s[m_pos] == ',') { ++m_pos; continue; }
            if (m_s[m_pos] == ']') { ++m_pos; break; }
            fail ("expected ',' or ']'");
        }
        return Value (std::move (arr));
    }

    Value parse_object (int depth)
    {
        ++m_pos;
        Object obj;
        skip_ws ();
        if (m_pos < m_s.size () && m_s[m_pos] == '}')
        {
            ++m_pos;
            return Value (std::move (obj));
        }
        while (true)
        {
            skip_ws ();
            if (m_pos >= m_s.size () || m_s[m_pos] != '"')
                fail ("expected member name");
            auto key = parse_string ();
            skip_ws ();
            if (m_pos >= m_s.size () || m_s[m_pos] != ':')
                fail ("expected ':'");
            ++m_pos;
            obj.emplace_back (std::move (key), parse_value (depth + 1));
            skip_ws ();
            if (m_pos >= m_s.size ())
                fail ("unterminated object");
            if (m_s[m_pos] == ',') { ++m_pos; continue; }
            if (m_s[m_pos] == '}') { ++m_pos; break; }
            fail ("expected ',' or '}'");
        }
        return Value (std::move (obj));
    }

    const std::string& m_s;
    size_t m_pos = 0;
};

} // anonymous namespace

Value
parse (const std::string& text)
{
    return Parser (text).parse_document ();
}

} // namespace gnc::json
