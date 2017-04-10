cargs = ['-DG_LOG_DOMAIN="Gmw"']

gnome_multi_writer_resources = gnome.compile_resources(
  'gnome-multi-writer-resources',
  'gnome-multi-writer.gresource.xml',
  source_dir : '.',
  c_name : 'gpm'
)

executable(
  'gnome-multi-writer',
  gnome_multi_writer_resources,
  sources : [
    'gmw-device.c',
    'gmw-main.c',
  ],
  include_directories : [
    include_directories('..'),
  ],
  dependencies : [
    gtk,
    gusb,
    udisks,
    gudev,
    libcanberra,
    polkit,
  ],
  c_args : cargs,
  install : true,
  install_dir : 'bin'
)

executable(
  'gnome-multi-writer-probe',
  sources : [
    'gmw-probe.c',
  ],
  include_directories : [
    include_directories('..'),
  ],
  dependencies : [
    gudev,
    gio,
  ],
  c_args : cargs,
  install : true,
  install_dir : 'libexec'
)
