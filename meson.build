dynamic_flag_src_dir = 'src'
dynamic_flag_include_dir = include_directories('include')

dynamic_flag_src_files = '''
	dynamic_flag.c
'''.split()

_dynamic_flag_src_files = []
foreach s : dynamic_flag_src_files
	_dynamic_flag_src_files += join_paths(dynamic_flag_src_dir, s)
endforeach

dynamic_flag_c_args = [
	'-Wno-unused-function', '-Wno-missing-prototypes',
	'-Wno-missing-declarations',
]

dynamic_flag_lib = static_library('dynamic_flag', _dynamic_flag_src_files,
	include_directories: dynamic_flag_include_dir,
	dependencies: [],
	c_args: dynamic_flag_c_args)

libdynamic_flag_dep = declare_dependency(link_whole: dynamic_flag_lib,
	include_directories: dynamic_flag_include_dir)

libdynamic_flag_header_dep = declare_dependency(
	include_directories: dynamic_flag_include_dir)

executable('dynamic_flag_test_feature_flags', 'tests/feature_flags.c',
	dependencies: [libdynamic_flag_dep],
	link_language: 'c', install: false)
