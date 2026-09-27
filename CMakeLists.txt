cmake_minimum_required(VERSION 3.24)

project(qtmonitor
    VERSION 0.1.0
    DESCRIPTION "A Qt6/KF6 resource and task monitor for Linux"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# DBus and Network both ship inside qt6-base alongside Widgets, so neither
# adds a package dependency. DBus reads login sessions from systemd-logind,
# whose runtime files are explicitly documented as unparseable; Network is
# used only for QNetworkInterface, to read interface addresses without
# hand-rolling a netlink client.
# LinguistTools is build-time only: it supplies lupdate/lrelease so the
# .ts catalogs can be refreshed and compiled. Nothing links against it.
find_package(Qt6 REQUIRED COMPONENTS Widgets DBus Network LinguistTools)

# Tests are opt-in so an ordinary build needs neither Qt6::Test nor ctest.
# The CI workflow turns them on; packaging does not.
option(QTMONITOR_BUILD_TESTS "Build the parser tests" OFF)
if(QTMONITOR_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
endif()
find_package(KF6Auth REQUIRED)
find_package(KF6Config REQUIRED)

qt_standard_project_setup()

qt_add_executable(qtmonitor
    src/main.cpp
    src/formatting.h
    src/mainwindow.cpp
    src/mainwindow.h
    src/processmodel.cpp
    src/processmodel.h
    src/sampler.cpp
    src/sampler.h
    src/settings.cpp
    src/settings.h
    src/settingsdialog.cpp
    src/settingsdialog.h
    src/providers/cpuprovider.cpp
    src/providers/cpuprovider.h
    src/providers/diskprovider.cpp
    src/providers/diskprovider.h
    src/providers/drmgpuprovider.cpp
    src/providers/drmgpuprovider.h
    src/providers/gpuprovider.cpp
    src/providers/gpuprovider.h
    src/providers/gpusensors.cpp
    src/providers/gpusensors.h
    src/providers/hardwareinfo.cpp
    src/providers/hardwareinfo.h
    src/providers/hwmonprovider.cpp
    src/providers/hwmonprovider.h
    src/providers/memoryprovider.cpp
    src/providers/memoryprovider.h
    src/providers/networkprovider.cpp
    src/providers/networkprovider.h
    src/providers/nvidiagpuprovider.cpp
    src/providers/nvidiagpuprovider.h
    src/providers/optionaltools.cpp
    src/providers/optionaltools.h
    src/providers/processcontrol.cpp
    src/providers/processcontrol.h
    src/providers/processprovider.cpp
    src/providers/processprovider.h
    src/providers/sessionprovider.cpp
    src/providers/sessionprovider.h
    src/providers/startupprovider.cpp
    src/providers/startupprovider.h
    src/tabs/cpupage.cpp
    src/tabs/cpupage.h
    src/tabs/diskpage.cpp
    src/tabs/diskpage.h
    src/tabs/gpupage.cpp
    src/tabs/gpupage.h
    src/tabs/historygraph.cpp
    src/tabs/historygraph.h
    src/tabs/memorypage.cpp
    src/tabs/memorypage.h
    src/tabs/minigraph.cpp
    src/tabs/minigraph.h
    src/tabs/networkpage.cpp
    src/tabs/networkpage.h
    src/tabs/performancepage.cpp
    src/tabs/performancepage.h
    src/tabs/sensorspage.cpp
    src/tabs/sensorspage.h
    src/tabs/placeholderpage.cpp
    src/tabs/placeholderpage.h
    src/tabs/processespage.cpp
    src/tabs/processespage.h
    src/tabs/startuppage.cpp
    src/tabs/startuppage.h
    src/tabs/theming.h
    src/tabs/userspage.cpp
    src/tabs/userspage.h
    src/resources.qrc
)

target_link_libraries(qtmonitor PRIVATE
    Qt6::Widgets
    Qt6::DBus
    Qt6::Network
    KF6::AuthCore
    KF6::ConfigCore
    KF6::ConfigGui
)

# Translations. The .qm files are compiled from the catalogs below and
# embedded in the binary under :/i18n, so a translated build is one file with
# no runtime lookup path to get wrong. `cmake --build build --target
# update_translations` re-runs lupdate over the sources to refresh them.
#
# The helper is deliberately excluded: it runs as root under D-Bus activation,
# where the invoking user's locale is not in the environment, so its few
# refusal strings stay in English rather than being translated unreliably.
qt_add_translations(qtmonitor
    TS_FILES translations/qtmonitor_en.ts
)

# ---------------------------------------------------------------------------
# Privileged helper (KAuth + polkit)
#
# Signalling another user's process needs root. The helper is a separate,
# minimal binary run by the KAuth D-Bus backend after polkit authorizes the
# action; the GUI itself never escalates.
#
# The kauth_install_helper_files() macro is deliberately NOT used: it depends
# on ECM's KDE_INSTALL_DBUSDIR variables, and this project does not pull in
# extra-cmake-modules. The two D-Bus files are configured from KAuth's own
# stub templates instead, which is exactly what that macro does internally.
# ---------------------------------------------------------------------------

set(QTMONITOR_HELPER_ID "org.qtmonitor")
set(QTMONITOR_HELPER_TARGET "qtmonitor_helper")

# polkit and the D-Bus system bus only read /usr and /etc, so KAuth hardcodes
# /usr for the policy directory. Installing the rest under a different prefix
# would scatter the pieces and silently break escalation.
if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
    message(WARNING
        "CMAKE_INSTALL_PREFIX is '${CMAKE_INSTALL_PREFIX}', but polkit and the "
        "D-Bus system bus only read from /usr and /etc. The privileged helper "
        "will be installed under that prefix while its policy goes to "
        "${KAUTH_POLICY_FILES_INSTALL_DIR}, so signalling other users' "
        "processes will not work. Configure with -DCMAKE_INSTALL_PREFIX=/usr.")
endif()

qt_add_executable(${QTMONITOR_HELPER_TARGET}
    src/helper/main.cpp
    src/helper/qtmonitorhelper.cpp
    src/helper/qtmonitorhelper.h
)

# Core only: the privileged binary must not drag in a GUI stack.
target_link_libraries(${QTMONITOR_HELPER_TARGET} PRIVATE
    Qt6::Core
    KF6::AuthCore
)

install(TARGETS ${QTMONITOR_HELPER_TARGET}
    DESTINATION ${KAUTH_HELPER_INSTALL_DIR}
)

# polkit action definition, generated from the .actions file.
kauth_install_actions(${QTMONITOR_HELPER_ID}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/helper/org.qtmonitor.actions
)

# D-Bus: a policy letting root own the helper's bus name and everyone send to
# it, plus a service file so the system bus can activate it on demand.
set(HELPER_ID "${QTMONITOR_HELPER_ID}")
set(HELPER_TARGET "${QTMONITOR_HELPER_TARGET}")
set(HELPER_USER "root")
configure_file(${KAUTH_STUB_FILES_DIR}/dbus_policy.stub
               ${CMAKE_CURRENT_BINARY_DIR}/${QTMONITOR_HELPER_ID}.conf @ONLY)
configure_file(${KAUTH_STUB_FILES_DIR}/dbus_service.stub
               ${CMAKE_CURRENT_BINARY_DIR}/${QTMONITOR_HELPER_ID}.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${QTMONITOR_HELPER_ID}.conf
    DESTINATION share/dbus-1/system.d
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${QTMONITOR_HELPER_ID}.service
    DESTINATION share/dbus-1/system-services
)

# The GUI checks for these before offering to authenticate, so that running
# from the build directory reports "not installed" instead of failing oddly.
target_compile_definitions(qtmonitor PRIVATE
    QTMONITOR_KAUTH_HELPER_PATH="${KAUTH_HELPER_INSTALL_ABSOLUTE_DIR}/${QTMONITOR_HELPER_TARGET}"
    QTMONITOR_KAUTH_POLICY_PATH="${KAUTH_POLICY_FILES_INSTALL_DIR}/${QTMONITOR_HELPER_ID}.policy"
)

install(TARGETS qtmonitor
    RUNTIME DESTINATION bin
)

install(FILES org.qtmonitor.app.desktop
    DESTINATION share/applications
)

# AppStream metadata, which is what lists the app in Discover, GNOME Software
# and Flathub. Without it the project is findable only by people who already
# know its name.
install(FILES org.qtmonitor.app.metainfo.xml
    DESTINATION share/metainfo
)

# The desktop entry's Icon=qtmonitor is resolved through the hicolor theme at
# launch, so the SVG has to land on disk as well as inside the binary's Qt
# resources: the compiled-in copy is only reachable once the app is running,
# which is too late for the launcher that has to draw the icon first.
install(FILES src/icons/qtmonitor.svg
    DESTINATION share/icons/hicolor/scalable/apps
)

if(QTMONITOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
