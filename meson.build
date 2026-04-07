project('dpdk_forwarder', 'c',
  version : '0.1',
  default_options : ['werror=true', 'optimization=3', 'c_std=c11']
)

dpdk = dependency('libdpdk', required : true)

cc = meson.get_compiler('c')
add_project_arguments('-D_GNU_SOURCE', language : 'c')

if get_option('buildtype').contains('debug')
  add_project_arguments('-DDEBUG', language : 'c')
endif

sources = files(
  'main.c'
)

executable('forwarder',
  sources,
  dependencies : dpdk,
  install : true
)
