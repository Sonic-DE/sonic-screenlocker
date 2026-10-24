remove_definitions(-DTRANSLATION_DOMAIN=\"kscreenlocker\")
add_definitions(-DTRANSLATION_DOMAIN=\"kscreenlocker_greet\")

include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/../
    ${CMAKE_CURRENT_SOURCE_DIR}/../
)

add_subdirectory(worker)

set(kscreenlocker_authenticator_SRCS
    pamauthenticators.cpp
    pamauthenticators.h
    pamauthenticator.cpp
    pamauthenticator.h
    pamauthenticatormodel.cpp
    pamauthenticatordescriptor.cpp
)

ecm_qt_declare_logging_category(kscreenlocker_authenticator_SRCS
    HEADER kscreenlocker_greet_logging.h
    IDENTIFIER KSCREENLOCKER_GREET
    CATEGORY_NAME kscreenlocker_greet
    DESCRIPTION "KScreenLocker (greeter)"
    EXPORT KSCREENLOCKER
)

set(kscreenlocker_greet_SRCS
    greeterapp.cpp
    greeterappearance.cpp
   virtualkeyboard.cpp
   virtualkeyboard.h
   main.cpp
   noaccessnetworkaccessmanagerfactory.cpp
   greeteripcclient.cpp
   powermanagement.cpp
   ../logind.cpp
    greeterapp.h
    greeterappearance.h
   noaccessnetworkaccessmanagerfactory.h
   greeteripcclient.h
   powermanagement.h
   ../logind.h
)

ecm_qt_declare_logging_category(kscreenlocker_greet_SRCS
    HEADER kscreenlocker_logging.h
    IDENTIFIER KSCREENLOCKER
    CATEGORY_NAME kscreenlocker
    DESCRIPTION "KScreenLocker"
    EXPORT KSCREENLOCKER
)

set_source_files_properties(worker/org.kde.plasma.screenlocker.worker.xml PROPERTIES INCLUDE "result.h")
qt_add_dbus_interface(kscreenlocker_authenticator_SRCS worker/org.kde.plasma.screenlocker.worker.xml org.kde.plasma.screenlocker.worker)
qt_add_dbus_adaptor(kscreenlocker_authenticator_SRCS worker/org.kde.plasma.screenlocker.xml pamauthenticator.h PamAuthenticator)

add_library(kscreenlocker_authenticator OBJECT ${kscreenlocker_authenticator_SRCS})
target_link_libraries(kscreenlocker_authenticator
    WorkerInterface
    Qt::Core
    Qt::Qml
    Qt::DBus
    ${PAM_LIBRARIES}
    KF6::CoreAddons
    KF6::ConfigCore
    KF6::I18n
)

qt_add_resources(kscreenlocker_greet_SRCS fallbacktheme.qrc)
qt_add_resources(kscreenlocker_greet_SRCS virtualkeyboard.qrc)

add_executable(kscreenlocker_greet ${kscreenlocker_greet_SRCS})
qt_add_resources(kscreenlocker_greet controls_config PREFIX "/" FILES "qtquickcontrols2.conf")

ecm_add_qml_module(kscreenlocker_greet URI org.kde.kscreenlocker VERSION 1.0)

target_link_libraries(kscreenlocker_greet PRIVATE
    settings
    kscreenlocker_authenticator
    KF6::Package
    KF6::Crash
    KF6::ColorScheme
    Plasma::Plasma
    Qt::QuickControls2
    KF6::I18n
    KF6::ConfigGui
    KF6::KIOCore
    KF6::WindowSystem
    KF6::I18nQml
    Plasma::PlasmaQuick
    Qt::DBus
    Qt::Quick
    Qt::Qml
    Qt::Network
    X11::X11
    XCB::XCB
    KF6::ScreenDpms
)

add_dependencies(kscreenlocker_greet kscreenlocker_worker)

qt_generate_foreign_qml_types(kscreenlocker_authenticator kscreenlocker_greet)

# manually install type info since it's not installed by default for executables but we want it
# for the kscreenlocker UI in shellpackage
qt6_query_qml_module(kscreenlocker_greet
        TARGET_PATH qml_module_target_path
        QMLDIR qml_module_qmldir
        TYPEINFO qml_module_typeinfo
)

install(FILES "${qml_module_qmldir}"   DESTINATION ${KDE_INSTALL_QMLDIR}/${qml_module_target_path})
install(FILES "${qml_module_typeinfo}" DESTINATION ${KDE_INSTALL_QMLDIR}/${qml_module_target_path})

install(TARGETS kscreenlocker_greet DESTINATION ${KDE_INSTALL_LIBEXECDIR})
