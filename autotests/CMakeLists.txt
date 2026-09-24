include(ECMMarkAsTest)

#######################################
# LogindTest
#######################################
set(logindTest_SRCS
    logindtest.cpp
    fakelogind.cpp
    ../logind.cpp
    ../kscreenlocker_logging.cpp
)

add_executable(logindTest ${logindTest_SRCS})
target_link_libraries(logindTest Qt::DBus Qt::Test KF6::I18n)
add_test(NAME ksmserver-logindTest COMMAND logindTest)
ecm_mark_as_test(logindTest)

#######################################
# PamTest
#######################################

pkg_search_module(PamWrapper pam_wrapper)
option(REQUIRE_PAM_REGRESSION_TESTS "Fail configuration when isolated PAM regression tests cannot be built" OFF)
set(SCREENLOCKER_DESKTOP_PACKAGE_DIR "" CACHE PATH "Path to the paired desktoppackage source tree")
option(REQUIRE_LOCKSCREEN_CONTRACT_TEST "Fail configuration when the paired production lockscreen theme is unavailable" OFF)
# We're turning it off on the CI until pam_wrapper 1.15 is available on our image (which adds sanitizer support.)
# see https://invent.kde.org/plasma/kscreenlocker/-/issues/11 for more details
if (PamWrapper_FOUND AND NOT "$ENV{KDECI_BUILD}" STREQUAL "TRUE")
    pkg_get_variable(PamWrapperModuleDir pam_wrapper "modules")
    set(PamTestServiceDir "${CMAKE_CURRENT_BINARY_DIR}/pam-services")
    file(MAKE_DIRECTORY "${PamTestServiceDir}")
    configure_file(data/test_service.in "${PamTestServiceDir}/test_service")
    find_library(PamWrapperLibrary NAMES pam_wrapper REQUIRED)
    add_library(pam_failure_delay_test_module MODULE pam_failure_delay_test_module.cpp)
    target_include_directories(pam_failure_delay_test_module PRIVATE ${PAM_INCLUDE_DIR})
    target_link_libraries(pam_failure_delay_test_module PRIVATE ${PAM_LIBRARIES})
    set_target_properties(pam_failure_delay_test_module PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        CXX_VISIBILITY_PRESET default
    )
    set(PamFailureDelayTestModule "${CMAKE_CURRENT_BINARY_DIR}/pam_failure_delay_test_module${CMAKE_SHARED_MODULE_SUFFIX}")
    configure_file(data/failure_delay_test.in "${PamTestServiceDir}/failure_delay_test")
    add_executable(pamTest pamtest.cpp)
    target_link_libraries(pamTest Qt::Test kscreenlocker_authenticator)
    target_compile_definitions(pamTest PRIVATE
        PAM_TEST_SERVICE_DIR="${PamTestServiceDir}"
        PAM_TEST_DB="${CMAKE_CURRENT_SOURCE_DIR}/data/test_db"
    )
    add_test(NAME ksmserver-pamTest COMMAND pamTest)
    # they made a mistake in the env name, so set both old and new
    ecm_mark_as_test(pamTest)
    set_tests_properties(ksmserver-pamTest PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "LD_PRELOAD=${PamWrapperLibrary};PAM_WRAPPER=1;PAM_WRAPPER_SERVICE_DIR=${PamTestServiceDir};PAM_MATRIX_PASSWD=${CMAKE_CURRENT_SOURCE_DIR}/data/test_db;UID_WRAPPER_DISABLE_DEEPBIND=1;PAM_WRAPPER_DISABLE_DEEPBIND=1;ASAN_OPTIONS=verify_asan_link_order=0"
    )

    add_executable(pamDelayTest pamdelaytest.cpp)
    target_link_libraries(pamDelayTest Qt::Test kscreenlocker_authenticator)
    target_compile_definitions(pamDelayTest PRIVATE PAM_TEST_SERVICE_DIR="${PamTestServiceDir}")
    add_dependencies(pamDelayTest pam_failure_delay_test_module)
    add_test(NAME ksmserver-pamDelayTest COMMAND pamDelayTest)
    ecm_mark_as_test(pamDelayTest)
    set_tests_properties(ksmserver-pamDelayTest PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "LD_PRELOAD=${PamWrapperLibrary};PAM_WRAPPER=1;PAM_WRAPPER_SERVICE_DIR=${PamTestServiceDir};UID_WRAPPER_DISABLE_DEEPBIND=1;PAM_WRAPPER_DISABLE_DEEPBIND=1;ASAN_OPTIONS=verify_asan_link_order=0"
    )

    add_executable(pamAuthenticatorsTest pamauthenticatorstest.cpp)
    target_link_libraries(pamAuthenticatorsTest Qt::Test kscreenlocker_authenticator)
    add_dependencies(pamAuthenticatorsTest pam_failure_delay_test_module)
    add_test(NAME ksmserver-pamAuthenticatorsTest COMMAND pamAuthenticatorsTest)
    ecm_mark_as_test(pamAuthenticatorsTest)
    set_tests_properties(ksmserver-pamAuthenticatorsTest PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "LD_PRELOAD=${PamWrapperLibrary};PAM_WRAPPER=1;PAM_WRAPPER_SERVICE_DIR=${PamTestServiceDir};UID_WRAPPER_DISABLE_DEEPBIND=1;PAM_WRAPPER_DISABLE_DEEPBIND=1;ASAN_OPTIONS=verify_asan_link_order=0"
    )

    if(NOT KSCREENLOCKER_HAVE_PAM_FAIL_DELAY)
        add_executable(pamNativeDelayTest pamnativedelaytest.cpp)
        target_link_libraries(pamNativeDelayTest Qt::Test kscreenlocker_authenticator)
        add_dependencies(pamNativeDelayTest pam_failure_delay_test_module)
        add_test(NAME ksmserver-pamNativeDelayTest COMMAND pamNativeDelayTest)
        ecm_mark_as_test(pamNativeDelayTest)
        set_tests_properties(ksmserver-pamNativeDelayTest PROPERTIES
            TIMEOUT 30
            ENVIRONMENT "LD_PRELOAD=${PamWrapperLibrary};PAM_WRAPPER=1;PAM_WRAPPER_SERVICE_DIR=${PamTestServiceDir};UID_WRAPPER_DISABLE_DEEPBIND=1;PAM_WRAPPER_DISABLE_DEEPBIND=1;ASAN_OPTIONS=verify_asan_link_order=0"
        )
    endif()
elseif(REQUIRE_PAM_REGRESSION_TESTS)
    message(FATAL_ERROR "Isolated PAM regression tests were required, but pam_wrapper is unavailable or disabled for this CI environment")
endif()

if(EXISTS "${SCREENLOCKER_DESKTOP_PACKAGE_DIR}/contents/lockscreen/LockScreenUi.qml"
   AND EXISTS "${SCREENLOCKER_DESKTOP_PACKAGE_DIR}/contents/lockscreen/MainBlock.qml")
    add_executable(lockScreenContractTest lockscreencontracttest.cpp)
    target_link_libraries(lockScreenContractTest Qt::Test kscreenlocker_authenticator)
    target_compile_definitions(lockScreenContractTest PRIVATE
        SCREENLOCKER_LOCKSCREEN_UI="${SCREENLOCKER_DESKTOP_PACKAGE_DIR}/contents/lockscreen/LockScreenUi.qml"
        SCREENLOCKER_MAIN_BLOCK="${SCREENLOCKER_DESKTOP_PACKAGE_DIR}/contents/lockscreen/MainBlock.qml"
    )
    add_test(NAME ksmserver-lockScreenContractTest COMMAND lockScreenContractTest)
    ecm_mark_as_test(lockScreenContractTest)
    find_program(QMLLINT_EXECUTABLE NAMES qmllint6 qmllint)
    if(QMLLINT_EXECUTABLE)
        add_test(NAME ksmserver-lockScreenQmlLint
            COMMAND ${QMLLINT_EXECUTABLE}
                "${SCREENLOCKER_DESKTOP_PACKAGE_DIR}/contents/lockscreen/LockScreenUi.qml"
                "${SCREENLOCKER_DESKTOP_PACKAGE_DIR}/contents/lockscreen/MainBlock.qml"
        )
    elseif(REQUIRE_LOCKSCREEN_CONTRACT_TEST)
        message(FATAL_ERROR "The paired lockscreen contract test requires qmllint")
    endif()
elseif(REQUIRE_LOCKSCREEN_CONTRACT_TEST)
    message(FATAL_ERROR "The paired production lockscreen theme was required but SCREENLOCKER_DESKTOP_PACKAGE_DIR is invalid")
endif()


#######################################
# KeyboardGrabber
#######################################
add_executable(keyboardGrabber keyboardgrabber.cpp)
target_link_libraries(keyboardGrabber Qt::Core XCB::XCB)
ecm_mark_as_test(keyboardGrabber)

#######################################
# PointerGrabber
#######################################
add_executable(pointerGrabber pointergrabber.cpp)
target_link_libraries(pointerGrabber Qt::Core XCB::XCB)
ecm_mark_as_test(pointerGrabber)

#######################################
# LockWindowTest
#######################################
set(x11LockerTest_SRCS
    x11lockertest.cpp
    ../x11locker.cpp
    ../globalaccel.cpp
    ../abstractlocker.cpp
    ../kscreenlocker_logging.cpp
)

add_executable(x11LockerTest ${x11LockerTest_SRCS})
target_link_libraries(x11LockerTest
    KF6::GlobalAccel
    KF6::I18n
    KF6::WindowSystem
    Qt::Test
    Qt::Widgets
    X11::X11
    XCB::XCB
    XCB::KEYSYMS
)

target_link_libraries(x11LockerTest Qt::GuiPrivate)

add_test(NAME ksmserver-x11LockerTest COMMAND x11LockerTest)
ecm_mark_as_test(x11LockerTest)
