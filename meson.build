project('mctp-ext', ['c', 'cpp'],
    version: '1.0',
    meson_version: '>= 0.59.0',
    default_options: [
        'cpp_std=c++23',
        'warning_level=2',
    ],
)

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
