set(ONLINE_TEST_INCLUDE_DIRS
  ${CMAKE_BINARY_DIR}/common
  ${CMAKE_SOURCE_DIR}/gnucash/online
  ${CMAKE_SOURCE_DIR}/libgnucash/engine
  ${CMAKE_SOURCE_DIR}/libgnucash/core-utils
  ${GTEST_INCLUDE_DIR}
)
set(ONLINE_TEST_LIBS gnc-online-core gnc-engine gnc-core-utils gtest)

if (HAVE_ONLINE_SERVICES)
  gnc_add_test(test-online-json gtest-json.cpp
    ONLINE_TEST_INCLUDE_DIRS ONLINE_TEST_LIBS)
  gnc_add_test(test-online-hmrc gtest-hmrc.cpp
    ONLINE_TEST_INCLUDE_DIRS ONLINE_TEST_LIBS)
  gnc_add_test(test-online-plaid gtest-plaid.cpp
    ONLINE_TEST_INCLUDE_DIRS ONLINE_TEST_LIBS)
  gnc_add_test(test-online-vat-return gtest-vat-return.cpp
    ONLINE_TEST_INCLUDE_DIRS ONLINE_TEST_LIBS)
  gnc_add_test(test-online-bank-feeds gtest-bank-feeds.cpp
    ONLINE_TEST_INCLUDE_DIRS ONLINE_TEST_LIBS)
endif()

set_dist_list(test_online_DIST CMakeLists.txt fake-transport.hpp uk-book.hpp
  gtest-json.cpp gtest-hmrc.cpp gtest-plaid.cpp gtest-vat-return.cpp
  gtest-bank-feeds.cpp)
