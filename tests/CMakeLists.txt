function(add_usa_test name source label)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE Qt6::Test Qt6::Core Qt6::Widgets Qt6::Sql)
    # Domain + worker sources reused without pulling the full GUI app:
    target_sources(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/domain/FileTree.cpp
        ${CMAKE_SOURCE_DIR}/src/services/LocalScannerWorker.cpp
    )
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS ${label})
endfunction()

add_usa_test(test_filetree test_filetree.cpp unit)
add_usa_test(test_scanner test_scanner.cpp integration)
