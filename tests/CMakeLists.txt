find_package(Qt6 REQUIRED COMPONENTS Test)

# Each pure module gets its own test executable, so a failure names the module
# it came from without reading the log.
foreach(suite rangeparser pathresolver)
    qt_add_executable(tst_${suite} tst_${suite}.cpp)

    target_link_libraries(tst_${suite} PRIVATE
        nestdrive_http
        Qt6::Test
    )

    # Console application: the test runner needs stdout, unlike the GUI target.
    set_target_properties(tst_${suite} PROPERTIES WIN32_EXECUTABLE FALSE)

    add_test(NAME ${suite} COMMAND tst_${suite})
endforeach()
