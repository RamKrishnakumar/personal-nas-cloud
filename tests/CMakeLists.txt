find_package(Qt6 REQUIRED COMPONENTS Test)

# One executable per module, so a failure names the module it came from without
# reading the log. The library each suite links is the only difference, which also
# keeps the pure-logic suites free of Gui and Sql plugin dependencies.
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

# Pure logic: parsing, paths, admission bookkeeping, hardware detection.
foreach(suite rangeparser pathresolver requestparser admissioncontrol hardwaretier compression)
    nestdrive_add_test(${suite} LIBRARIES nestdrive_http)
endforeach()

nestdrive_add_test(cookies LIBRARIES nestdrive_http)

nestdrive_add_test(database LIBRARIES nestdrive_db)
nestdrive_add_test(auditlog LIBRARIES nestdrive_db)

# Authentication: a temporary database, no server.
foreach(suite passwordhash loginratelimiter sessionstore userstore permissions)
    nestdrive_add_test(${suite} LIBRARIES nestdrive_auth)
endforeach()
nestdrive_add_test(thumbnailcache LIBRARIES nestdrive_media)
nestdrive_add_test(thumbnailscanner LIBRARIES nestdrive_media)

# A stand-in for the ffmpeg CLI. Built rather than scripted because QProcess has to
# launch it the same way it launches the real thing, and because a .bat is not
# consistently seen as executable across the two toolchains.
qt_add_executable(fake_ffmpeg fixtures/fake_ffmpeg.cpp)
target_link_libraries(fake_ffmpeg PRIVATE Qt6::Core Qt6::Gui)
set_target_properties(fake_ffmpeg PROPERTIES WIN32_EXECUTABLE FALSE)

# The same binary again under the other name. VideoTranscoder finds ffprobe by
# substituting the filename beside ffmpeg, which is how every distribution ships the
# pair — so the fixture has to exist under both names, and the source switches on
# which one it was invoked as. Two targets from one source rather than two sources
# that would drift apart.
qt_add_executable(fake_ffprobe fixtures/fake_ffmpeg.cpp)
target_link_libraries(fake_ffprobe PRIVATE Qt6::Core Qt6::Gui)
set_target_properties(fake_ffprobe PROPERTIES WIN32_EXECUTABLE FALSE)

nestdrive_add_test(frameextractor LIBRARIES nestdrive_media)
nestdrive_add_test(videotranscoder LIBRARIES nestdrive_media)

# Integration: real sockets against the worker pool.
nestdrive_add_test(httpserver LIBRARIES nestdrive_handlers)
nestdrive_add_test(admissionserver LIBRARIES nestdrive_handlers)
nestdrive_add_test(thumbnailserver LIBRARIES nestdrive_handlers)
nestdrive_add_test(listapi LIBRARIES nestdrive_handlers)
nestdrive_add_test(authserver LIBRARIES nestdrive_handlers)
nestdrive_add_test(authzserver LIBRARIES nestdrive_handlers)
nestdrive_add_test(admissionauth LIBRARIES nestdrive_handlers)
nestdrive_add_test(auditserver LIBRARIES nestdrive_handlers)
nestdrive_add_test(videoserver LIBRARIES nestdrive_handlers)

# These suites run the fixtures, so they have to exist first. Listed here, after every
# target is created, because add_dependencies() needs both sides to exist already.
#
# Passed as defines rather than discovered at runtime: guessing at a sibling path is
# exactly the kind of thing that works in one build layout and silently skips the
# tests in another.
foreach(suite frameextractor videotranscoder videoserver thumbnailcache thumbnailscanner)
    add_dependencies(tst_${suite} fake_ffmpeg fake_ffprobe)
    target_compile_definitions(tst_${suite} PRIVATE
        FAKE_FFMPEG_PATH="$<TARGET_FILE:fake_ffmpeg>"
        FAKE_FFPROBE_PATH="$<TARGET_FILE:fake_ffprobe>"
    )
endforeach()
