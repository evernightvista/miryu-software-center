# miryu-software-check — standalone update checker binary.
#
# Kept OUTSIDE the src/ tree on purpose: the root CMakeLists.txt globs
# src/*.cpp into the miryu-software-center GUI target, and this standalone
# program must be a separate binary with its own main().
add_executable(miryu-software-check
    main.cpp
    UpdateCheckNotifier.cpp
    UpdateCheckNotifier.h
    NotifierItem.cpp
    NotifierItem.h
)

target_include_directories(miryu-software-check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(miryu-software-check PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    KF6::CoreAddons
    KF6::I18n
    KF6::ConfigCore
    KF6::Notifications
    KF6::JobWidgets
    KF6::StatusNotifierItem
    KF6::Service
    KF6::KIOGui
    KF6::DBusAddons
    KF6::Crash
)

install(TARGETS miryu-software-check ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
