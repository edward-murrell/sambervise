# ── GSettings schema ─────────────────────────────────────────────────────────
# Copy schema to build tree and compile it for development runs:
#   GSETTINGS_SCHEMA_DIR=<build>/data ./sambervise

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/data)

add_custom_target(compile-schemas ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/au.com.codefoundation.Sambervise.gschema.xml
            ${CMAKE_BINARY_DIR}/data/
    COMMAND ${GLIB_COMPILE_SCHEMAS} ${CMAKE_BINARY_DIR}/data
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/au.com.codefoundation.Sambervise.gschema.xml
    COMMENT "Compiling GSettings schemas"
)

# ── Install ───────────────────────────────────────────────────────────────────

install(FILES au.com.codefoundation.Sambervise.gschema.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas
)

install(FILES au.com.codefoundation.Sambervise.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

# Recompile the system schema cache after a direct install.
# Skip when DESTDIR is set (e.g. CPack staging) — the postinst script handles
# that case on the target machine.
install(CODE "
    if(\"\$ENV{DESTDIR}\" STREQUAL \"\")
        message(STATUS \"Compiling GSettings schemas\")
        execute_process(
            COMMAND ${GLIB_COMPILE_SCHEMAS}
                    \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas\"
        )
    endif()
")
