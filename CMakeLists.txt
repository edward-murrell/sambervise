cmake_minimum_required(VERSION 3.18)
project(sambervise VERSION 0.1.12 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADW  REQUIRED IMPORTED_TARGET libadwaita-1)

find_library(LDAP_LIB ldap REQUIRED)
find_library(LBER_LIB lber REQUIRED)
pkg_check_modules(KRB5 REQUIRED IMPORTED_TARGET krb5)

find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
find_program(GLIB_COMPILE_SCHEMAS   glib-compile-schemas   REQUIRED)

include(GNUInstallDirs)
include(CTest)

add_subdirectory(data)
add_subdirectory(src)
add_subdirectory(tests)

# ── Debian package (cpack -G DEB) ─────────────────────────────────────────────

set(CPACK_GENERATOR                       "DEB")
set(CPACK_PACKAGE_NAME                    "sambervise")
set(CPACK_PACKAGE_VERSION                 "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT                 "Edward Murrell <edward@murrell.co.nz>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "GTK 4 GUI for administering Samba 4 Active Directory Domain Controllers")
set(CPACK_DEBIAN_FILE_NAME                "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_SECTION          "admin")
set(CPACK_DEBIAN_PACKAGE_PRIORITY         "optional")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libgtk-4-1 (>= 4.0), libadwaita-1-0 (>= 1.0), libldap-2.5-0, libkrb5-3, libsasl2-modules-gssapi-mit")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_SOURCE_DIR}/packaging/postinst"
    "${CMAKE_SOURCE_DIR}/packaging/postrm")

# Release notes / changelog bundled into the .deb
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_SOURCE_DIR}/CHANGELOG.md")
install(FILES "${CMAKE_SOURCE_DIR}/CHANGELOG.md"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}")

# Exclude gschemas.compiled — it's a system-wide database rebuilt by postinst
set(CPACK_IGNORE_FILES "/gschemas[.]compiled$")

include(CPack)
