if (QT6)
    find_package(Qt6 REQUIRED COMPONENTS Test)
else()
    find_package(Qt5 REQUIRED COMPONENTS Test)
endif()

enable_testing(true)
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure)

include_directories("../src")

set(testFiles
    testalgorithm.cpp
    testbittorrenttrackerentry.cpp
    testglobal.cpp
    testorderedset.cpp
    testpath.cpp
    testhttpconnection.cpp
    testpreferences.cpp
    testsessiontracing.cpp
    testutilscompare.cpp
    testutilsbytearray.cpp
    testutilsgzip.cpp
    testutilsio.cpp
    testutilsstring.cpp
    testutilsversion.cpp
)

foreach(testFile ${testFiles})
    get_filename_component(testFilename "${testFile}" NAME_WLE)

    add_executable("${testFilename}" "${testFile}")
    if (${testFilename} STREQUAL "testsessiontracing")
        target_link_libraries("${testFilename}" PRIVATE Qt::Test qbt_webui)
    else()
        target_link_libraries("${testFilename}" PRIVATE Qt::Test qbt_base)
    endif()
    add_test(NAME "${testFilename}" COMMAND "${testFilename}")

    add_dependencies(check "${testFilename}")
endforeach()
