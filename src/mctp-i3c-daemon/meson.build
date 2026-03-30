i3c_daemon_inc = include_directories('include')

systemd_dep = dependency('libsystemd')
sdbusplus_dep = dependency('sdbusplus')
jsonc_dep = dependency('json-c', required: true)
boost_dep = dependency('boost', modules: ['system'])

i3c_daemon_deps = [systemd_dep, sdbusplus_dep, boost_dep, jsonc_dep]

if get_option('aspeed-soc').enabled()
    add_project_arguments('-DASPEED_SOC', language: ['c', 'cpp'])
endif

if get_option('aspeed-2700-soc').enabled()
    add_project_arguments('-DASPEED_2700_SOC', language: ['c', 'cpp'])
endif

if get_option('nuvoton-soc').enabled()
    add_project_arguments('-DNUVOTON_SOC', language: ['c', 'cpp'])
endif

if get_option('client-socket-interface').enabled()
    add_project_arguments('-DCLIENT_SOCKET_INTERFACE', language: ['c', 'cpp'])
endif

cxx = meson.get_compiler('cpp')
add_project_arguments(
    cxx.get_supported_arguments([
        '-DBOOST_ALL_NO_LIB',
        '-DBOOST_ALLOW_DEPRECATED_HEADERS',
        '-DBOOST_ASIO_DISABLE_THREADS',
        '-DBOOST_ASIO_NO_DEPRECATED',
        '-DBOOST_URL_NO_SOURCE_LOCATION',
        '-DBOOST_SPIRIT_X3_NO_RTTI',
        '-DBOOST_NO_RTTI',
        '-DSDBUSPLUS_DISABLE_BOOST_COROUTINES',
    ]),
    language: 'cpp',
)

executable('mctp-i3c-daemon',
    sources: [
        'mctp-i3c-main.cpp',
        'mctp-netlink.cpp',
        'mctp-log.cpp',
        'utils.cpp',
        'MctpDevice.cpp',
        'mctp-ctrl-cmds.cpp',
        'mctp-encode.cpp',
        'mctp-i3c-client-sock.cpp',
    ],
    dependencies: i3c_daemon_deps,
    include_directories: i3c_daemon_inc,
    install: true,
)

executable('mctp-req',
    sources: ['mctp-req.cpp'],
    install: true,
)

executable('mctpctrl-cli-sock-test',
    sources: ['mctpctrl-cli-sock-test.cpp'],
    install: true,
)
