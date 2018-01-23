docbook2man = find_program('docbook2man', required : false)
if docbook2man.found()
  custom_target('gnome-multi-writer-man',
    output : 'gnome-multi-writer.1',
    input : 'gnome-multi-writer.sgml',
    command : [docbook2man, '@INPUT@', '--output', 'man'],
    install : true,
    install_dir : join_paths(prefixed_mandir, 'man1'),
  )
endif
