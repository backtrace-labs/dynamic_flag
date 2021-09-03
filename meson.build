
anhook_src_dir = 'src'
anhook_include_dir = include_directories('include')

anhook_src_files = '''
	an_hook.c
'''.split()

_anhook_src_files = []
foreach s : anhook_src_files
	_anhook_src_files += join_paths(anhook_src_dir, s)
endforeach

anhook_c_args = [
	'-Wno-unused-function', '-Wno-missing-prototypes',
	'-Wno-missing-declarations',
]

anhook_lib = static_library('anhook', _anhook_src_files,
	include_directories: anhook_include_dir,
	dependencies: [libck_dep, ],
	c_args: anhook_c_args)

libanhook_dep = declare_dependency(link_whole: anhook_lib,
	include_directories: anhook_include_dir)
