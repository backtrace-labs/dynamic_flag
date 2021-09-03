
an_hook_src_dir = 'src'
an_hook_include_dir = include_directories('include')

an_hook_src_files = '''
	an_hook.c
'''.split()

_an_hook_src_files = []
foreach s : an_hook_src_files
	_an_hook_src_files += join_paths(an_hook_src_dir, s)
endforeach

an_hook_c_args = [
	'-Wno-unused-function', '-Wno-missing-prototypes',
	'-Wno-missing-declarations',
]

an_hook_lib = static_library('an_hook', _an_hook_src_files,
	include_directories: an_hook_include_dir,
	dependencies: [],
	c_args: an_hook_c_args)

liban_hook_dep = declare_dependency(link_whole: an_hook_lib,
	include_directories: an_hook_include_dir)

executable('an_hook_test_feature_flags', 'tests/feature_flags.c',
	dependencies: [liban_hook_dep],
	link_language: 'c', install: false)
