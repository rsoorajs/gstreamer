vulkan_sources = [
  'gstvulkan.c',
  'gstvulkanelement.c',
  'vkdownload.c',
  'vkdeviceprovider.c',
  'vksink.c',
  'vkupload.c',
]

vulkan_headers = [
  'gstvulkanelements.h',
  'vkcolorconvert.h',
  'vkdeviceprovider.h',
  'vkdownload.h',
  'vkh264dec.h',
  'vkh265dec.h',
  'vkimageidentity.h',
  'vkoverlaycompositor.h',
  'vkshaderspv.h',
  'vksink.h',
  'vkupload.h',
  'vkviewconvert.h',
]

glsc_sources = [
  'vkcolorconvert.c',
  'vkimageidentity.c',
  'vkshaderspv.c',
  'vkviewconvert.c',
  'vkoverlaycompositor.c',
]

video_sources = [
  'base/gsth264encoder.c',
  'vkh264enc.c',
  'vkh264dec.c',
  'vkh265dec.c',
]

doc_sources = []
foreach s: vulkan_sources + glsc_sources + video_sources + vulkan_headers
  doc_sources += meson.current_source_dir() / s
endforeach

plugin_sources += {
  'vulkan': pathsep.join(doc_sources)
}

if get_option('vulkan').disabled()
  subdir_done()
endif

if not gstvulkan_dep.found()
  if get_option('vulkan').enabled()
    error('GStreamer Vulkan plugin required via options, but needed dependencies not found.')
  else
    subdir_done()
  endif
endif

extra_deps = []
extra_args = []

vulkan_plugin_enums = []

glslc = find_program('glslc', required: false)
glslang = find_program('glslang', required: false)
if not glslc.found() and not glslang.found()
  if get_option('vulkan').enabled()
    error('GStreamer Vulkan plugin required via options, but glslc or glslang was not found.')
  else
    subdir_done()
  endif
  vulkan_compiled_shader_sources = []
else
  extra_args += ['-DHAVE_GLSLC']
  subdir('shaders')

  vulkan_sources += glsc_sources
  vulkan_plugin_enum_headers = files(
    'vkviewconvert.h',
  )
  vulkan_plugin_enums += gnome.mkenums_simple('gstvulkan-plugins-enumtypes',
      sources : vulkan_plugin_enum_headers,
      body_prefix : '#ifdef HAVE_CONFIG_H\n#include "config.h"\n#endif',
      header_prefix : '#include <gst/gst.h>')
endif

if vulkan_conf.get('GST_VULKAN_HAVE_VIDEO_EXTENSIONS') == 1
  vulkan_sources += video_sources
  extra_deps += gstcodecs_dep
  extra_args += ['-DGST_USE_UNSTABLE_API']
endif

gstvulkan_plugin = library('gstvulkan',
  vulkan_sources, vulkan_compiled_shader_sources, vulkan_plugin_enums,
  c_args : gst_plugins_bad_args + extra_args,
  objc_args : gst_plugins_bad_args,
  link_args : noseh_link_args,
  include_directories : [configinc],
  dependencies : [gstvideo_dep, gstbase_dep, gstvulkan_dep, vulkan_dep, gio_dep] + extra_deps,
  install : true,
  install_dir : plugins_install_dir,
)
plugins += [gstvulkan_plugin]
