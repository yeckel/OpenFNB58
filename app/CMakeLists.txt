cmake_minimum_required(VERSION 3.28)
project(fnb58app VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ── Qt location: prefer /opt/Qt local install if present ──────────────────
if(EXISTS "/opt/Qt/6.9.2/gcc_64" AND NOT DEFINED CMAKE_PREFIX_PATH)
    list(APPEND CMAKE_PREFIX_PATH "/opt/Qt/6.9.2/gcc_64")
endif()

find_package(Qt6 6.7 REQUIRED COMPONENTS
    Core Gui Quick QuickControls2 Qml QuickDialogs2 Widgets)

qt_standard_project_setup(REQUIRES 6.7)

# Put the executable in bin/ to avoid conflict with the QML module directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ── Sources ────────────────────────────────────────────────────────────────
qt_add_executable(fnb58app
    main.cpp
    DeviceBackend.h  DeviceBackend.cpp
    BaseTransport.h
    UsbTransport.h   UsbTransport.cpp
    BleTransport.h   BleTransport.cpp
    ZipWriter.h      ZipWriter.cpp
    XlsxWriter.h     XlsxWriter.cpp
    DataRecord.h
)

qt_add_qml_module(fnb58app
    URI "fnb58app"
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/LiveChart.qml
)

target_link_libraries(fnb58app PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Qml
    Qt6::QuickDialogs2
    Qt6::Widgets
)

# ── DBus (BlueZ BLE) — Linux only ─────────────────────────────────────────
if(UNIX AND NOT APPLE)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    target_link_libraries(fnb58app PRIVATE Qt6::DBus)
    target_compile_definitions(fnb58app PRIVATE FNB58_HAVE_DBUS)
endif()

# ── macOS bundle ──────────────────────────────────────────────────────────
if(APPLE)
    set_target_properties(fnb58app PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME        "OpenFNB58"
        MACOSX_BUNDLE_BUNDLE_VERSION     "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_GUI_IDENTIFIER     "cz.yeckel.openfnb58"
    )
endif()

# ── RPATH (local dev only) ─────────────────────────────────────────────────
if(EXISTS "/opt/Qt/6.9.2/gcc_64")
    set_target_properties(fnb58app PROPERTIES
        BUILD_RPATH   "/opt/Qt/6.9.2/gcc_64/lib"
        INSTALL_RPATH "/opt/Qt/6.9.2/gcc_64/lib"
    )
endif()
