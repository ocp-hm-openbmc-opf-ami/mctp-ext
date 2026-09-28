boost = dependency(
    'boost',
    version: '>=1.84.0',
    include_type: 'system',
    modules: ['context', 'coroutine'],
)
sdbusplus = dependency('sdbusplus')
phosphor_logging_dep = dependency('phosphor-logging')

cc = meson.get_compiler('cpp')
i2c = declare_dependency(dependencies: [cc.find_library('i2c')])

json_c = dependency('json-c')

reactor_deps = [
    boost,
    i2c,
    json_c,
    phosphor_logging_dep,
    sdbusplus,
]

reactor_sources = [
    'MCTPReactorMain.cpp',
    'MCTPReactor.cpp',
    'MCTPReactorConfig.cpp',
    'MCTPDiscovery.cpp',
    'MCTPI2CDiscovery.cpp',
    'MCTPI3CDiscovery.cpp',
    'MCTPPCIeDiscovery.cpp',
    'MCTPUSBDiscovery.cpp',
    'MCTPRoutingTableDiscovery.cpp',
    'HostStateMonitor.cpp',
    'MCTPConfigMonitor.cpp',
    'MCTPEndpoint.cpp',
    'Utils.cpp',
    'DeviceMgmt.cpp',
    'config/reader.cpp',
    'config/wrapper_impl.cpp',
]

_il_quoted = []
foreach addr : get_option('mctp-i2c-arp-ignorelist')
    _il_quoted += ['"' + addr + '"']
endforeach
_il_define = ','.join(_il_quoted)

# Build I3C devices initializer list from key=value pipe-delimited strings
# Format: name=val|bus_num=val|pid_mask=val|device_pid=val|role=val|is_secondary_bus_owner=val
_i3c_dev_entries = []
foreach dev : get_option('mctp-i3c-devices')
    _kv = {}
    foreach pair : dev.split('|')
        kv = pair.split('=')
        _kv += {kv[0]: kv[1]}
    endforeach
    _i3c_dev_entries += ['{"' + _kv['name'] + '",' + _kv['bus_num'] + ',"' + _kv['pid_mask'] + '","' + _kv['device_pid'] + '","' + _kv['role'] + '",' + _kv['is_secondary_bus_owner'] + '}']
endforeach
_i3c_devices_define = '{' + ','.join(_i3c_dev_entries) + '}'

# Build I2C static devices initializer list from key=value pipe-delimited
# strings. Format: name=val|bus=val|address=val|static_endpoint_id=val
_i2c_dev_entries = []
foreach dev : get_option('mctp-i2c-devices')
    _kv = {}
    foreach pair : dev.split('|')
        kv = pair.split('=')
        _kv += {kv[0]: kv[1]}
    endforeach
    _i2c_dev_entries += ['{"' + _kv.get('name', '') + '","' + _kv.get('bus', '') + '","' + _kv.get('address', '') + '","' + _kv.get('static_endpoint_id', '') + '"}']
endforeach
_i2c_devices_define = '{' + ','.join(_i2c_dev_entries) + '}'

reactor_conf = configuration_data()
reactor_conf.set_quoted('MCTPD_JSON_FILE_DEFAULT',
    get_option('prefix') / get_option('datadir') / 'mctp/mctp_ext_options.json')
reactor_conf.set('DEFAULT_LOCAL_EID', get_option('default-local-eid'))
reactor_conf.set('DEFAULT_MTU', get_option('default-mtu'))
reactor_conf.set('DEFAULT_I2C_NET', get_option('mctp-i2c-net'))
reactor_conf.set('DEFAULT_I2C_ARP_IGNORELIST', _il_define)
reactor_conf.set('DEFAULT_I2C_DEVICES', _i2c_devices_define)
reactor_conf.set('DEFAULT_I3C_NET', get_option('mctp-i3c-net'))
reactor_conf.set('DEFAULT_I3C_DEVICES', _i3c_devices_define)
reactor_conf.set('DEFAULT_PCIE_NET', get_option('mctp-pcie-net'))
reactor_conf.set('DEFAULT_USB_NET', get_option('mctp-usb-net'))

reactor_conf.set('DEFAULT_I2C_POLL_INTERVAL', get_option('mctp-i2c-poll-interval'))
reactor_conf.set('DEFAULT_I3C_POLL_INTERVAL', get_option('mctp-i3c-poll-interval'))
reactor_conf.set('DEFAULT_PCIE_POLL_INTERVAL', get_option('mctp-pcie-poll-interval'))
reactor_conf.set('DEFAULT_USB_POLL_INTERVAL', get_option('mctp-usb-poll-interval'))
reactor_conf.set('DEFAULT_ROUTING_TABLE_POLL_INTERVAL', get_option('mctp-routing-table-poll-interval'))
reactor_conf.set('PCIE_DISCOVERY_DELAY_TIME', get_option('pcie-discovery-delay-time'))

# Default all feature flags to 0 (disabled)
reactor_conf.set('MCTP_I2C_ENABLED', 0)
reactor_conf.set('MCTP_I2C_ARP_ENABLED', 0)
reactor_conf.set('MCTP_I3C_ENABLED', 0)
reactor_conf.set('MCTP_PCIE_ENABLED', 0)
reactor_conf.set('MCTP_USB_ENABLED', 0)
reactor_conf.set('MCTP_USB_HOTPLUG_ENABLED', 0)
reactor_conf.set('REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED', 0)
reactor_conf.set('MCTP_WAIT_FOR_EID_ENABLED', 0)
reactor_conf.set_quoted('MCTP_PCIE_ROLE', 'endpoint')

if get_option('mctp-i2c').enabled()
    reactor_conf.set('MCTP_I2C_ENABLED', 1)
    if get_option('mctp-i2c-arp').enabled()
        reactor_conf.set('MCTP_I2C_ARP_ENABLED', 1)
    endif
endif

if get_option('mctp-i3c').enabled()
    reactor_conf.set('MCTP_I3C_ENABLED', 1)
endif

if get_option('mctp-pcie').enabled()
    reactor_conf.set('MCTP_PCIE_ENABLED', 1)
    reactor_conf.set_quoted('MCTP_PCIE_ROLE', get_option('mctp-pcie-role'))
endif

usb_lib = dependency('libusb-1.0')
reactor_deps += [usb_lib]

if get_option('mctp-usb').enabled()
    reactor_conf.set('MCTP_USB_ENABLED', 1)
    if get_option('mctp-usb-hotplug').enabled()
        reactor_conf.set('MCTP_USB_HOTPLUG_ENABLED', 1)
    endif
endif

if get_option('register-reactor-mctp-device-repository').enabled()
    reactor_conf.set('REGISTER_REACTOR_MCTP_DEVICE_REPOSITORY_ENABLED', 1)
endif

if get_option('wait-for-eid').enabled()
    reactor_conf.set('MCTP_WAIT_FOR_EID_ENABLED', 1)
endif

if get_option('mctp-i3c-multiple-bo-with-diff-eids')
    reactor_conf.set('MCTP_I3C_MULTIPLE_BO_WITH_DIFF_EIDS', 1)
endif

if get_option('nuvoton-sdk')
    reactor_conf.set('NUVOTON_SDK', 1)
endif

if get_option('multi-host-mode-support')
    reactor_conf.set('MULTI_HOST_MODE_SUPPORT', 1)
endif

reactor_config_h = configure_file(
    output: 'config.h',
    configuration: reactor_conf,
)

executable(
    'mctpreactor',
    reactor_sources + [reactor_config_h],
    dependencies: [reactor_deps],
    install: true,
    install_dir: get_option('sbindir'),
    cpp_args: ['-std=c++23', '-Wno-psabi'],
)
