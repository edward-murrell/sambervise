cmake_minimum_required(VERSION 3.18)
project(sambervise VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(ADW  REQUIRED IMPORTED_TARGET libadwaita-1)

find_library(LDAP_LIB ldap REQUIRED)
find_library(LBER_LIB lber REQUIRED)

find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
find_program(GLIB_COMPILE_SCHEMAS   glib-compile-schemas   REQUIRED)

include(GNUInstallDirs)
include(CTest)

add_subdirectory(data)
add_subdirectory(src)
add_subdirectory(tests)
