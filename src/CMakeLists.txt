# ── GResource bundle ─────────────────────────────────────────────────────────

set(GRESOURCE_XML ${CMAKE_SOURCE_DIR}/data/sambervise.gresource.xml)
set(GRESOURCE_C   ${CMAKE_CURRENT_BINARY_DIR}/sambervise-resources.c)

add_custom_command(
    OUTPUT  ${GRESOURCE_C}
    COMMAND ${GLIB_COMPILE_RESOURCES}
            --target=${GRESOURCE_C}
            --sourcedir=${CMAKE_SOURCE_DIR}/data
            --c-name sambervise
            --generate-source
            ${GRESOURCE_XML}
    DEPENDS ${GRESOURCE_XML}
            ${CMAKE_SOURCE_DIR}/data/ui/window.ui
            ${CMAKE_SOURCE_DIR}/data/ui/connect-dialog.ui
    COMMENT "Compiling GResources"
)


# ── Main executable ───────────────────────────────────────────────────────────

add_executable(sambervise
    main.c
    sbv-app.c
    sbv-error.c
    model/sbv-user.c
    model/sbv-group.c
    model/sbv-computer.c
    model/sbv-profile.c
    backend/sbv-connection.c
    backend/sbv-dns.c
    backend/sbv-profiles.c
    backend/sbv-users-backend.c
    backend/sbv-groups-backend.c
    backend/sbv-computers-backend.c
    ui/sbv-window.c
    ui/sbv-connect-dialog.c
    ui/sbv-password-dialog.c
    ui/sbv-users-panel.c
    ui/sbv-groups-panel.c
    ui/sbv-computers-panel.c
    ${GRESOURCE_C}
)

target_include_directories(sambervise PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(sambervise PRIVATE
    PkgConfig::GTK4
    PkgConfig::ADW
    ${LDAP_LIB}
    ${LBER_LIB}
)

install(TARGETS sambervise RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
