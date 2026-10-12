# Bank feeds (Plaid) and the HMRC Making Tax Digital VAT bridge.

add_subdirectory(test)

set(online_core_SOURCES
  gnc-bank-feeds.cpp
  gnc-hmrc.cpp
  gnc-json.cpp
  gnc-online-book.cpp
  gnc-online-http.cpp
  gnc-online-secrets.cpp
  gnc-plaid.cpp
  gnc-vat-return.cpp
)

set(online_core_HEADERS
  gnc-bank-feeds.hpp
  gnc-hmrc.hpp
  gnc-json.hpp
  gnc-online-book.hpp
  gnc-online-http.hpp
  gnc-online-secrets.hpp
  gnc-plaid.hpp
  gnc-vat-return.hpp
)

set(online_gui_SOURCES
  dialog-bank-feeds.cpp
  dialog-mtd-vat.cpp
  gnc-online-ui.cpp
  gnc-plugin-online.cpp
)

set(online_gui_HEADERS
  dialog-bank-feeds.h
  dialog-mtd-vat.h
  gnc-online-ui.hpp
  gnc-plugin-online.h
)

set(online_UI gnc-plugin-online.ui)

if (HAVE_ONLINE_SERVICES)
  set_source_files_properties (${online_core_SOURCES} ${online_gui_SOURCES}
    PROPERTIES OBJECT_DEPENDS ${CONFIG_H})

  add_library(gnc-online-core ${online_core_SOURCES} ${online_core_HEADERS})
  target_link_libraries(gnc-online-core
    gnc-engine gnc-core-utils PkgConfig::CURL PkgConfig::GLIB2 ${LIBSECRET_LDFLAGS})
  target_include_directories(gnc-online-core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_BINARY_DIR}/common ${LIBSECRET_INCLUDE_DIRS})
  target_compile_definitions(gnc-online-core PRIVATE -DG_LOG_DOMAIN=\"gnc.online\")

  gnc_generate_gresources(BASE online-gresources RESOURCE_FILES ${online_UI})

  add_library(gnc-online
    ${online_gui_SOURCES} ${online_gui_HEADERS}
    ${CMAKE_CURRENT_BINARY_DIR}/online-gresources.c)
  target_link_libraries(gnc-online
    gnc-online-core gnc-generic-import gnc-gnome-utils gnc-gnome gnc-engine
    gnc-app-utils gnc-core-utils PkgConfig::GTK3 PkgConfig::WEBKIT)
  target_include_directories(gnc-online
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/gnucash/import-export)
  target_compile_definitions(gnc-online PRIVATE -DG_LOG_DOMAIN=\"gnc.online\")

  if (APPLE)
    set_target_properties (gnc-online-core gnc-online PROPERTIES
      INSTALL_NAME_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/gnucash")
  endif()

  install(TARGETS gnc-online-core gnc-online
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gnucash
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/gnucash
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

set_local_dist(online_DIST_local CMakeLists.txt README.md
  ${online_core_SOURCES} ${online_core_HEADERS}
  ${online_gui_SOURCES} ${online_gui_HEADERS} ${online_UI})
set(online_DIST ${online_DIST_local} ${test_online_DIST} PARENT_SCOPE)
