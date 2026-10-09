function(add_usa_test name source label)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE Qt6::Test Qt6::Core Qt6::Widgets Qt6::Sql)
    # Domain + worker + scanner sources reused without pulling the GUI app.
    # Extra per-test sources may follow the label.
    target_sources(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/services/LocalScannerWorker.cpp
        ${CMAKE_SOURCE_DIR}/src/services/ScanLogger.cpp
        ${CMAKE_SOURCE_DIR}/src/services/scanner/DirectoryFinder.cpp
        ${CMAKE_SOURCE_DIR}/src/services/scanner/MftFinder.cpp
        ${CMAKE_SOURCE_DIR}/src/services/scanner/MftRecord.cpp
        ${CMAKE_SOURCE_DIR}/src/services/scanner/ScanEngine.cpp
        ${ARGN}
    )
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS ${label})
endfunction()

add_usa_test(test_scanner test_scanner.cpp integration)
add_usa_test(test_blockingqueue test_blockingqueue.cpp unit)
add_usa_test(test_analytics test_analytics.cpp unit)
add_usa_test(test_treemap test_treemap.cpp unit)
add_usa_test(test_directoryfinder test_directoryfinder.cpp integration)
add_usa_test(test_mftfinder test_mftfinder.cpp integration)
add_usa_test(test_scanengine test_scanengine.cpp integration)
add_usa_test(test_filesystemmodel test_filesystemmodel.cpp unit
    ${CMAKE_SOURCE_DIR}/src/models/FileSystemModel.cpp
    ${CMAKE_SOURCE_DIR}/src/models/DetailsTableModel.cpp
    ${CMAKE_SOURCE_DIR}/src/models/StatsTableModel.cpp
    ${CMAKE_SOURCE_DIR}/src/models/TreeFilterProxy.cpp)
add_usa_test(test_utf8 test_utf8.cpp unit)
add_usa_test(test_pipeline test_pipeline.cpp integration
    ${CMAKE_SOURCE_DIR}/src/models/FileSystemModel.cpp
    ${CMAKE_SOURCE_DIR}/src/services/IScannerService.cpp
    ${CMAKE_SOURCE_DIR}/src/services/ScannerService.cpp)
add_usa_test(test_ui test_ui.cpp ui
    ${CMAKE_SOURCE_DIR}/src/models/FileSystemModel.cpp
    ${CMAKE_SOURCE_DIR}/src/models/DetailsTableModel.cpp
    ${CMAKE_SOURCE_DIR}/src/models/TreeFilterProxy.cpp
    ${CMAKE_SOURCE_DIR}/src/services/IScannerService.cpp
    ${CMAKE_SOURCE_DIR}/src/services/ScannerService.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/ThemeManager.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/TreemapWidget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/BarWidget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/PieWidget.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/MainWindow.cpp)
# Widget tests run headless on every OS (no display server needed).
set_tests_properties(test_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
