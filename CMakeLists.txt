cmake_minimum_required(VERSION 3.24)

project(qtmonitor
    VERSION 0.1.0
    DESCRIPTION "A Qt6/KF6 resource and task monitor for Linux"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(KF6Auth REQUIRED)
find_package(KF6Config REQUIRED)
find_package(KF6Solid REQUIRED)

qt_standard_project_setup()

qt_add_executable(qtmonitor
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/tabs/placeholderpage.cpp
    src/tabs/placeholderpage.h
    src/resources.qrc
)

target_link_libraries(qtmonitor PRIVATE
    Qt6::Widgets
    KF6::AuthCore
    KF6::ConfigCore
    KF6::ConfigGui
    KF6::Solid
)

install(TARGETS qtmonitor
    RUNTIME DESTINATION bin
)

install(FILES org.qtmonitor.app.desktop
    DESTINATION share/applications
)
