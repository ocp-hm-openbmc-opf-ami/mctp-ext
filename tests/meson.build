add_languages('cpp', native: false)

gtest_dep = dependency('gtest', main: false, required: false)
gmock_dep = dependency('gmock', required: false)

if not gtest_dep.found() or not gmock_dep.found()
    cmake = import('cmake')
    opt_var = cmake.subproject_options()
    opt_var.add_cmake_defines({'CMAKE_CXX_FLAGS': '-Wno-pedantic'})
    opt_var.set_override_option('warning_level', '3')
    opt_var.set_override_option('werror', 'true')

    gtest_proj = cmake.subproject('gtest', options: opt_var, required: true)
    if gtest_proj.found()
        gtest_dep = declare_dependency(
            dependencies: [dependency('threads'), gtest_proj.dependency('gtest')]
        )
        gmock_dep = gtest_proj.dependency('gmock')
    else
        assert(
            not get_option('tests').enabled(),
            'Googletest is required if tests are enabled',
        )
    endif
endif

# Generate a test-specific config.h with default option values.
# This lets test executables compile production sources that include "config.h"
# without depending on the reactor production build target.
test_conf = configuration_data()
test_conf.set_quoted('MCTPD_CONF_FILE_DEFAULT', '/tmp/test-mctpd.conf')
test_conf.set_quoted(
    'MCTPD_JSON_FILE_DEFAULT',
    '/tmp/test-mctp-ext.json',
)
test_conf.set('DEFAULT_LOCAL_EID', get_option('default-local-eid'))
test_conf.set('DEFAULT_I2C_NET', get_option('mctp-i2c-net'))
test_conf.set('DEFAULT_I2C_WHITELIST', '{}')
test_conf.set('DEFAULT_I3C_NET', get_option('mctp-i3c-net'))
test_conf.set('DEFAULT_I3C_BUSOWNER_EID', get_option('mctp-i3c-busowner-eid'))
test_conf.set('DEFAULT_I3C_ENDPOINT_EID', get_option('mctp-i3c-endpoint-eid'))
test_conf.set_quoted('DEFAULT_PLATFORM_SOC', get_option('platform-soc'))
test_conf.set('DEFAULT_I3C_DEVICES', '{}')
test_conf.set('DEFAULT_PCIE_NET', get_option('mctp-pcie-net'))
test_conf.set('DEFAULT_USB_NET', get_option('mctp-usb-net'))
test_conf.set('DEFAULT_I2C_POLL_INTERVAL', get_option('mctp-i2c-poll-interval'))
test_conf.set('DEFAULT_I3C_POLL_INTERVAL', get_option('mctp-i3c-poll-interval'))
test_conf.set('DEFAULT_PCIE_POLL_INTERVAL', get_option('mctp-pcie-poll-interval'))
test_conf.set('DEFAULT_USB_POLL_INTERVAL', get_option('mctp-usb-poll-interval'))
test_conf.set(
    'DEFAULT_ROUTING_TABLE_POLL_INTERVAL',
    get_option('mctp-routing-table-poll-interval'),
)
test_conf.set('PCIE_DISCOVERY_DELAY_TIME', get_option('pcie-discovery-delay-time'))
test_conf.set('MCTP_I2C_ENABLED', 0)
test_conf.set('MCTP_I2C_ARP_ENABLED', 0)
test_conf.set('MCTP_I3C_ENABLED', 0)
test_conf.set('MCTP_PCIE_ENABLED', 0)
test_conf.set('MCTP_USB_ENABLED', 0)
test_conf.set('MCTP_USB_HOTPLUG_ENABLED', 0)
test_conf.set('REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED', 0)
test_conf.set_quoted('MCTP_PCIE_ROLE', 'endpoint')

test_config_h = configure_file(
    output: 'config.h',
    configuration: test_conf,
)

# Reactor source directory (headers + source files for test compilation)
reactor_src = '../src/reactor'
reactor_inc = include_directories(
    '.',        # includes build/tests/ where test-generated config.h resides
    reactor_src,
)

sdbusplus = dependency('sdbusplus')
phosphor_logging_dep = dependency('phosphor-logging')
boost = dependency(
    'boost',
    version: '>=1.84.0',
    include_type: 'system',
)
json_c = dependency('json-c')

# Shared production sources used by multiple test targets
reactor_common_sources = [
    reactor_src / 'Utils.cpp',
    reactor_src / 'DeviceMgmt.cpp',
]

# ----------------------------------------------------------------------------
# Utils + DeviceMgmt + MCTPEndpoint type tests
# ----------------------------------------------------------------------------
test_utils_exe = executable(
    'test_utils',
    ['test_main.cpp', 'test_utils.cpp'] + reactor_common_sources,
    include_directories: reactor_inc,
    dependencies: [
        gtest_dep,
        gmock_dep,
        sdbusplus,
        phosphor_logging_dep,
        boost,
        toml_dep,
    ],
    cpp_args: ['-std=c++23', '-Wno-psabi'],
)

test('unit_test_utils', test_utils_exe)

# ----------------------------------------------------------------------------
# MCTPReactorConfig TOML + JSON config parsing tests
# ----------------------------------------------------------------------------
test_reactor_config_exe = executable(
    'test_reactor_config',
    [
        'test_main.cpp',
        'test_reactor_config.cpp',
        reactor_src / 'MCTPReactorConfig.cpp',
    ] + reactor_common_sources,
    include_directories: reactor_inc,
    dependencies: [
        gtest_dep,
        gmock_dep,
        sdbusplus,
        phosphor_logging_dep,
        boost,
        json_c,
        toml_dep,
    ],
    cpp_args: ['-std=c++23', '-Wno-psabi'],
)

test('unit_test_reactor_config', test_reactor_config_exe)
