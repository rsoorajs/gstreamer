project('GStreamer manuals and tutorials', 'c',
  version: '1.27.1.1',
  meson_version : '>= 1.4')

hotdoc_p = find_program('hotdoc')
if not hotdoc_p.found()
    message('Hotdoc not found, not building the documentation')
    subdir_done()
endif

hotdoc_req = '>= 0.12.2'
hotdoc_version = run_command(hotdoc_p, '--version', check: false).stdout()
if not hotdoc_version.version_compare(hotdoc_req)
    error('Hotdoc version @0@ not found, got @1@'.format(hotdoc_req, hotdoc_version))
endif

hotdoc = import('hotdoc')
hotdoc_subprojects = []

api_version = '1.0'
if host_machine.system() == 'windows'
  pathsep = ';'
else
  pathsep = ':'
endif

python3 = import('python').find_installation()

built_subprojects = get_option('built_subprojects')
if built_subprojects != ''
  message('Have subprojects list from options')
else
  read_file_contents = '''
import os
import sys

assert len(sys.argv) >= 2
fname = sys.argv[1]

with open(fname, 'r') as f:
    for l in f:
        print(l)
'''

  # gst-build will generate this file for us to consume so that subproject
  # changes can still work
  fname = join_paths(meson.project_build_root(), '..', '..', 'GstDocumentedSubprojects')
  cmdres = run_command(
      python3,
      '-c', read_file_contents,
      fname,
      check: false,
    )
  if cmdres.returncode() == 0
    built_subprojects = cmdres.stdout().strip()
    message('Have subprojects from file: @0@'.format(fname))
  endif
endif

libs = ''
plugins_doc = ''
deps = []
plugins_sitemap = ''
plugin_configs = []
libs_configs = []
if built_subprojects != ''
    foreach project_name: built_subprojects.split(',')
        sub = subproject(project_name)
        if sub.get_variable('build_hotdoc')
            message('Building @0@ documentation'.format(project_name))

            foreach lib: sub.get_variable('libs_doc', [])
              hotdoc_subprojects += [lib]
              libs += lib.full_path() + pathsep
              deps += [lib]
            endforeach

            foreach plugin_doc: sub.get_variable('plugins_doc', [])
              warning(project_name+ ': variable plugins_doc is deprecated, use gst_plugins_doc instead')
              plugins_doc += plugin_doc.full_path() + pathsep
              hotdoc_subprojects += [plugin_doc]
              deps += [plugin_doc]
            endforeach

            foreach plugin_doc: sub.get_variable('gst_plugins_doc', [])
              plugins_doc += plugin_doc + pathsep
            endforeach

            plugin_configs += sub.get_variable('plugin_hotdoc_configs', [])

            # Handle the GStreamer special case with two custom targets
            libs_configs += sub.get_variable('lib_hotdoc_config', [])
            libs_configs += sub.get_variable('libs_hotdoc_configs', [])
        else
            message('@0@ did not build hotdoc documentation, can\'t build API doc'.format(project_name))
        endif
    endforeach
endif
deps += [dependency('gstreamer-' + api_version, fallback: ['gstreamer', 'gst_dep'])]

if get_option('use_portal_index')
  index = 'markdown/index.md'
else
  index = 'markdown/simple-index.md'
endif
sitemap_gen = find_program('scripts/generate_sitemap.py')
sitemap = custom_target(command: [
  sitemap_gen,
  '--input-sitemap', '@INPUT@',
  '--output-sitemap', '@OUTPUT@',
  '--markdown-index', 'gi-index',
  '--libs', libs,
  '--plugins', plugins_doc,
  '--plugin-configs', plugin_configs,
  '--lib-configs', libs_configs],
  input: 'sitemap.txt',
  output: 'sitemap.txt')
deps += [sitemap]

html_theme = 'https://github.com/hotdoc/hotdoc_lumen_theme/releases/download/0.16/hotdoc_lumen_theme-0.16.tar.xz?sha256=b7d7dde51285d1c90836c44ae298754e4cfa957e9a6d14ee5844b8a2cac04b5a'

gstreamer_doc = hotdoc.generate_doc('GStreamer',
    project_version: api_version,
    sitemap: sitemap,
    index: index,
    gi_index: index,
    install: true,
    extra_assets: [join_paths(meson.current_source_dir(), 'images')],
    syntax_highlighting_activate: true,
    html_theme: html_theme,
    include_paths: [
      meson.current_source_dir() / 'examples',
      meson.current_source_dir() / 'markdown' / 'tutorials' / 'basic',
      meson.current_source_dir() / 'markdown' / 'templates',
      meson.current_source_dir() / 'markdown' / 'tutorials' / 'playback',
    ],
    html_extra_theme: join_paths(meson.current_source_dir(), 'theme/extra'),
    dependencies: deps,
    subprojects: hotdoc_subprojects,
    disable_incremental_build: true,
    gst_list_plugins_page: 'gst-index',
    gst_index: join_paths(meson.current_source_dir(), 'markdown/plugins_doc.md'),
    devhelp_activate: true,
    devhelp_online: 'https://gstreamer.freedesktop.org/documentation/',
    build_always_stale: true,
    edit_on_github_repository: 'https://gitlab.freedesktop.org/gstreamer/gst-docs/',
    previous_symbol_index: join_paths(meson.current_source_dir(), 'symbols', 'symbol_index.json'),
    fatal_warnings: get_option('fatal_warnings')
)

# For devhelp
meson.add_devenv({'XDG_DATA_DIRS': meson.current_build_dir() / 'GStreamer-doc'},
  method: 'prepend')

cdata = configuration_data()
cdata.set('GST_API_VERSION', api_version)
readme = configure_file(input: 'scripts/RELEASE_README.md',
               output: 'README.md',
               configuration : cdata)

run_target('release',
  command: [find_program('scripts/release.py'),
            gstreamer_doc.full_path(),
            meson.project_version(),
            meson.current_build_dir()],
  depends: [gstreamer_doc]
)

meson.add_dist_script('scripts/gen-changelog.py', meson.project_name(), '1.24.0', meson.project_version())
