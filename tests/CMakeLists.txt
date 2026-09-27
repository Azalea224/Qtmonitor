# Parser tests.
#
# The provider sources are compiled straight into the test binary rather than
# linked from a library. The alternative — extracting a providers library the
# app also links — would be the right move once there is a second test target,
# but today it would add a build artefact to buy nothing.
#
# GpuRegistry is deliberately not pulled in: it constructs the nvidia-smi
# backend, which probes PATH. DrmGpuProvider is tested on its own, and only
# its sysfs discovery honours the injected root.

qt_add_executable(tst_providers
    tst_providers.cpp
    ../src/providers/cpuprovider.cpp
    ../src/providers/diskprovider.cpp
    ../src/providers/drmgpuprovider.cpp
    ../src/providers/gpusensors.cpp
    ../src/providers/hwmonprovider.cpp
    ../src/providers/memoryprovider.cpp
    ../src/providers/networkprovider.cpp
    ../src/providers/optionaltools.cpp
)

target_include_directories(tst_providers PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Qt6::Network is needed because the network provider asks QNetworkInterface
# for addresses. That query hits the real host rather than the fixture root,
# so the tests assert only on the counters parsed out of /proc/net/dev.
target_link_libraries(tst_providers PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
)

# The fixtures are read from the source tree, so the tests run correctly from
# any build directory without an install step or a working-directory rule.
target_compile_definitions(tst_providers PRIVATE
    QTMONITOR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

add_test(NAME providers COMMAND tst_providers)

# QTest needs no display for these — nothing constructs a widget — but the
# offscreen platform keeps it from failing on a machine with no X or Wayland
# session, which is every CI runner.
set_tests_properties(providers PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
