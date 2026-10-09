add_executable(usa
    app/main.cpp
    services/IScannerService.cpp
    services/DiskTelemetry.cpp
    services/OwnerResolver.cpp    services/LocalScannerWorker.cpp
    services/ScanLogger.cpp
    services/ScannerService.cpp
    services/DatabaseManager.cpp
    services/scanner/DirectoryFinder.cpp
    services/scanner/MftFinder.cpp
    services/scanner/MftRecord.cpp
    services/scanner/ScanEngine.cpp
    models/FileSystemModel.cpp
    models/DetailsTableModel.cpp
    models/StatsTableModel.cpp
    models/TreeFilterProxy.cpp
    ui/ThemeManager.cpp
    ui/TreemapWidget.cpp
    ui/BarWidget.cpp
    ui/PieWidget.cpp
    ui/OptionsDialog.cpp
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
if(WIN32)
    target_link_libraries(usa PRIVATE pdh advapi32)
endif()
if(APPLE)
    target_link_libraries(usa PRIVATE "-framework IOKit" "-framework CoreFoundation")
endif()

set_target_properties(usa PROPERTIES
    OUTPUT_NAME "UniversalStorageAnalyzer"
    WIN32_EXECUTABLE TRUE
)

# Deterministic smoke/headless entry points are built into the binary:
#   UniversalStorageAnalyzer --smoke-test
#   UniversalStorageAnalyzer --headless-check
