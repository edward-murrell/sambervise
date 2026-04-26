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
            ${CMAKE_SOURCE_DIR}/data/icons/scalable/actions/document-edit-symbolic.svg
            ${CMAKE_SOURCE_DIR}/data/icons/scalable/actions/view-list-bullet-symbolic.svg
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
    model/sbv-ldap-node.c
    backend/sbv-connection.c
    backend/sbv-dns.c
    backend/sbv-profiles.c
    backend/sbv-users-backend.c
    backend/sbv-groups-backend.c
    backend/sbv-computers-backend.c
    backend/sbv-idmap-hints.c
    backend/sbv-collisions.c
    backend/sbv-ldap-browse.c
    ui/sbv-window.c
    ui/sbv-connect-dialog.c
    ui/sbv-password-dialog.c
    ui/sbv-users-panel.c
    ui/sbv-groups-panel.c
    ui/sbv-computers-panel.c
    ui/sbv-create-user-dialog.c
    ui/sbv-create-group-dialog.c
    ui/sbv-create-computer-dialog.c
    ui/sbv-ldap-panel.c
    ${GRESOURCE_C}
)

target_include_directories(sambervise PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(sambervise PRIVATE
    PACKAGE_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(sambervise PRIVATE
    PkgConfig::GTK4
    PkgConfig::ADW
    PkgConfig::KRB5
    ${LDAP_LIB}
    ${LBER_LIB}
)

install(TARGETS sambervise RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
