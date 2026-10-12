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
#include "gnc-json.hpp"

#include <gtest/gtest.h>

using namespace gnc::json;

TEST (OnlineJson, numbers_keep_their_literal_text)
{
    auto v = parse (R"({"a": 1234.56, "b": -0.1, "c": 1e3, "d": 10})");
    EXPECT_EQ ("1234.56", v["a"].as_string ());
    EXPECT_EQ ("-0.1", v["b"].as_string ());
    EXPECT_EQ ("1e3", v["c"].as_string ());
    EXPECT_EQ (10, v["d"].as_int ());
    EXPECT_EQ (R"({"a":1234.56,"b":-0.1,"c":1e3,"d":10})", v.dump ());
}

TEST (OnlineJson, member_order_is_preserved_and_set_replaces)
{
    auto v = Value::object ();
    v.set ("z", 1).set ("a", "x").set ("z", 2);
    EXPECT_EQ (R"({"z":2,"a":"x"})", v.dump ());
    v.erase ("z");
    EXPECT_EQ (R"({"a":"x"})", v.dump ());
}

TEST (OnlineJson, strings_escape_and_unescape)
{
    auto v = parse (R"(["a\"b\\c\n", "\u00e9\u20ac", "\ud83d\ude00"])");
    const auto& arr = v.as_array ();
    EXPECT_EQ ("a\"b\\c\n", arr[0].as_string ());
    EXPECT_EQ ("\xc3\xa9\xe2\x82\xac", arr[1].as_string ());
    EXPECT_EQ ("\xf0\x9f\x98\x80", arr[2].as_string ());
    EXPECT_EQ ("\"a\\\"b\\\\c\\n\"", quote (arr[0].as_string ()));
    EXPECT_EQ (R"("\u0001")", quote (std::string (1, '\x01')));
}

TEST (OnlineJson, round_trips_nested_documents)
{
    const std::string text = R"({"x":[true,false,null,{"y":[]}],"s":"t"})";
    EXPECT_EQ (text, parse (text).dump ());
    auto pretty = parse (text).dump (true);
    EXPECT_EQ (text, parse (pretty).dump ());
}

TEST (OnlineJson, rejects_malformed_input)
{
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "01", "1.", "-", "\"\\x\"",
                            "tru", "[1] 2", "\"\\ud800\"", "{\"a\":1,}"})
        EXPECT_THROW (parse (bad), ParseError) << bad;
    EXPECT_THROW (Value::number ("1.2.3"), std::invalid_argument);
}

TEST (OnlineJson, missing_members_read_as_null)
{
    auto v = parse (R"({"a":{"b":"c"}})");
    EXPECT_TRUE (v["nope"].is_null ());
    EXPECT_TRUE (v["nope"]["deeper"].is_null ());
    EXPECT_EQ ("c", v["a"].get_string ("b"));
    EXPECT_EQ ("dflt", v.get_string ("nope", "dflt"));
    EXPECT_FALSE (v.get_optional_string ("nope"));
}
