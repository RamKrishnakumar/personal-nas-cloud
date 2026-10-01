find_package(Qt6 REQUIRED COMPONENTS Test)

# One executable per module, so a failure names the module it came from without
# reading the log. The library each suite links is the only difference.
function(nestdrive_add_test suite)
    cmake_parse_arguments(ARG "" "" "LIBRARIES" ${ARGN})

    qt_add_executable(tst_${suite} tst_${suite}.cpp)

    target_link_libraries(tst_${suite} PRIVATE
        ${ARG_LIBRARIES}
        Qt6::Test
    )

    # Console application: the test runner needs stdout, unlike the GUI target.
    set_target_properties(tst_${suite} PROPERTIES WIN32_EXECUTABLE FALSE)

    add_test(NAME ${suite} COMMAND tst_${suite})
endfunction()

# Pure parsing and path logic — no Qt6::Sql, so no driver plugin to deploy.
foreach(suite rangeparser pathresolver requestparser admissioncontrol hardwaretier)
    nestdrive_add_test(${suite} LIBRARIES nestdrive_http)
endforeach()

nestdrive_add_test(database LIBRARIES nestdrive_db)

# Integration: real sockets against the worker pool.
nestdrive_add_test(httpserver LIBRARIES nestdrive_http)
nestdrive_add_test(admissionserver LIBRARIES nestdrive_http)
