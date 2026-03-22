set(TEST_COMMON
    ${CMAKE_SOURCE_DIR}/src/sbv-error.c
    ${CMAKE_SOURCE_DIR}/src/model/sbv-user.c
    ${CMAKE_SOURCE_DIR}/src/model/sbv-group.c
)

foreach(T users groups)
    add_executable(test-${T} test-${T}.c ${TEST_COMMON})
    target_include_directories(test-${T} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test-${T} PRIVATE
        PkgConfig::GTK4
        PkgConfig::ADW
        ${LDAP_LIB}
        ${LBER_LIB}
    )
    add_test(NAME ${T} COMMAND test-${T})
endforeach()
