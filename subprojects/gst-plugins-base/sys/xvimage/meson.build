xvimage_sources = [
  'xvcontext.c',
  'xvimage.c',
  'xvimageallocator.c',
  'xvimagepool.c',
  'xvimagesink.c',
]

xvimage_headers = [
  'xvcontext.h',
  'xvimagesink.h',
  'xvimagepool.h',
  'xvimageallocator.h',
]

doc_sources = []
foreach s: xvimage_sources + xvimage_headers
  doc_sources += meson.current_source_dir() / s
endforeach

plugin_sources += {
  'xvimagesink': pathsep.join(doc_sources)
}

no_warn_args = []
# XKeycodeToKeysym is deprecated, but we use it when Xkb is unavailable
if cc.has_argument ('-Wno-deprecated-declarations')
  no_warn_args += '-Wno-deprecated-declarations'
endif

xvideo_dep = dependency('xv', required : get_option('xvideo'))

if xvideo_dep.found() and x11_dep.found()
  gstxvimage = library('gstxvimagesink',
    xvimage_sources,
    c_args : gst_plugins_base_args + no_warn_args,
    include_directories: [configinc, libsinc],
    dependencies : [video_dep, gst_base_dep, gst_dep, x11_dep, xshm_dep, xvideo_dep, xi_dep, libm],
    install : true,
    install_dir : plugins_install_dir,
  )
  plugins += [gstxvimage]
endif
