project('mctp-ext', ['c', 'cpp'],
    version: '1.0',
    meson_version: '>= 1.1.1',
    default_options: [
        'cpp_std=c++23',
        'warning_level=2',
    ],
)

if get_option('tests').enabled()
  add_project_arguments('-Wno-error=pedantic',language:['cpp','c'])
  add_project_arguments('-Wno-error=discarded-qualifiers',language:['cpp','c'])
  sdbusplus_dep = subproject('sdbusplus').get_variable('sdbusplus_dep')
  meson.override_dependency('sdbusplus',sdbusplus_dep)
  phosphor_dbus_interfaces_dep = dependency('phosphor-dbus-interfaces-ami')
  phosphor_logging_dep = subproject('phosphor-logging').get_variable('phosphor_logging_dep')
  meson.override_dependency('phosphor-logging',phosphor_logging_dep)
endif
# tomlc99 - bundled C library for TOML parsing
toml_dep = declare_dependency(
    sources: ['lib/tomlc99/toml.c'],
    include_directories: include_directories('lib/tomlc99'),
)

if get_option('build-mctp-i3c-daemon').enabled()
    subdir('src/mctp-i3c-daemon')
endif

if get_option('build-reactor').enabled()
    subdir('src/reactor')
endif

if get_option('tests').enabled()
    subdir('tests')
endif
