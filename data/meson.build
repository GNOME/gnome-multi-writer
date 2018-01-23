subdir('appdata')
subdir('icons')

compiled = gnome.compile_schemas()
install_data('org.gnome.MultiWriter.gschema.xml',
  install_dir : 'share/glib-2.0/schemas',
)

i18n.merge_file(
  input: 'org.gnome.MultiWriter.desktop.in',
  output: 'org.gnome.MultiWriter.desktop',
  type: 'desktop',
  po_dir: join_paths(meson.source_root(), 'po'),
  install: true,
  install_dir: join_paths(prefixed_datadir, 'applications'),
)

i18n.merge_file(
  input: 'org.gnome.MultiWriter.policy.in',
  output: 'org.gnome.MultiWriter.policy',
  type: 'xml',
  po_dir: join_paths(meson.source_root(), 'po'),
  install: true,
  install_dir: join_paths(prefixed_datadir, 'polkit-1', 'actions'),
)
