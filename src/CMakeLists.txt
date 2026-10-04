add_executable(usa
    app/main.cpp
    domain/FileTree.cpp
    services/IScannerService.cpp
    services/LocalScannerWorker.cpp
    services/ScannerService.cpp
    services/DatabaseManager.cpp
    models/FileSystemModel.cpp
    models/DetailsTableModel.cpp
    ui/ThemeManager.cpp
    ui/MainWindow.cpp
    resources/app.qrc
)

target_include_directories(usa PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(usa PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Sql
)

set_target_properties(usa PROPERTIES
    OUTPUT_NAME "UniversalStorageAnalyzer"
    WIN32_EXECUTABLE TRUE
)

# Deterministic smoke/headless entry points are built into the binary:
#   UniversalStorageAnalyzer --smoke-test
#   UniversalStorageAnalyzer --headless-check
