cmake_minimum_required(VERSION 3.20)

project(miryu VERSION 45.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(ECM 6.0 NO_MODULE REQUIRED)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs6)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMInstallIcons)
include(ECMAddAppIcon)
include(ECMDeprecationSettings)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    DBus
    Concurrent
    Network
)

find_package(KF6 REQUIRED COMPONENTS
    CoreAddons
    I18n
    XmlGui
    Config
    WidgetsAddons
    ItemViews
    IconThemes
    KIO
    Crash
    DBusAddons
    WindowSystem
)

ecm_set_disabled_deprecation_versions(
    KF 6.0
    QT 6.5
)

file(GLOB_RECURSE MIRYU_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

file(GLOB_RECURSE MIRYU_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

add_executable(miryu-software-center
    ${MIRYU_SOURCES}
    ${MIRYU_HEADERS}
)

target_include_directories(miryu-software-center PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/models
)

target_link_libraries(miryu-software-center PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::DBus
    Qt6::Concurrent
    Qt6::Network
    KF6::CoreAddons
    KF6::I18n
    KF6::XmlGui
    KF6::ConfigCore
    KF6::ConfigGui
    KF6::WidgetsAddons
    KF6::ItemViews
    KF6::IconThemes
    KF6::KIOCore
    KF6::KIOWidgets
    KF6::Crash
    KF6::DBusAddons
    KF6::WindowSystem
)

install(TARGETS miryu-software-center ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})

install(FILES
    src/miryuui.rc
    DESTINATION ${KDE_INSTALL_KXMLGUIDIR}/miryu
)

install(FILES
    data/org.miryugaming.PackageManager.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)

install(FILES
    data/org.miryugaming.PackageManager.metainfo.xml
    DESTINATION ${KDE_INSTALL_METAINFODIR}
)

# Polkit policy for privileged package operations
install(FILES
    data/org.miryugaming.PackageManager.polkit
    DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions
)

# Polkit rule granting passwordless restart of dnf5daemon-server.service
# (used after every DNF5 transaction to restart the daemon without prompting).
install(FILES
    data/50-miryu-dnf5daemon.rules
    DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/rules.d
)

ki18n_install(po)
