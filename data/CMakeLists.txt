# ── GSettings schema ─────────────────────────────────────────────────────────
# Copy schema to build tree and compile it for development runs:
#   GSETTINGS_SCHEMA_DIR=<build>/data ./sambervise

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/data)

add_custom_target(compile-schemas ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/org.ekm.sambervise.gschema.xml
            ${CMAKE_BINARY_DIR}/data/
    COMMAND ${GLIB_COMPILE_SCHEMAS} ${CMAKE_BINARY_DIR}/data
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/org.ekm.sambervise.gschema.xml
    COMMENT "Compiling GSettings schemas"
)

# ── Install ───────────────────────────────────────────────────────────────────

install(FILES org.ekm.sambervise.gschema.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas
)

install(FILES org.ekm.sambervise.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

# Recompile the system schema cache after installing
install(CODE "
    message(STATUS \"Compiling GSettings schemas\")
    execute_process(
        COMMAND ${GLIB_COMPILE_SCHEMAS}
                \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas\"
    )
")
