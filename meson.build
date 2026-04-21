project('macos-rdp-server', ['c', 'objc'],
    version: '0.1.0',
    meson_version: '>= 0.60.0',
    default_options: [
        'warning_level=2',
        'c_std=c11',
    ])

cc = meson.get_compiler('c')
objc = meson.get_compiler('objc')

glib_dep = dependency('glib-2.0', version: '>= 2.70.0')
gio_dep = dependency('gio-2.0')
cairo_dep = dependency('cairo')
freerdp_dep = dependency('freerdp3', version: '>= 3.0.0')
freerdp_server_dep = dependency('freerdp-server3', version: '>= 3.0.0')
winpr_dep = dependency('winpr3', version: '>= 3.0.0')

coregraphics_dep = dependency('appleframeworks', modules: ['CoreGraphics'])
coremedia_dep = dependency('appleframeworks', modules: ['CoreMedia'])
corevideo_dep = dependency('appleframeworks', modules: ['CoreVideo'])
videotoolbox_dep = dependency('appleframeworks', modules: ['VideoToolbox'])
screencapturekit_dep = dependency('appleframeworks', modules: ['ScreenCaptureKit'])
accelerate_dep = dependency('appleframeworks', modules: ['Accelerate'])
appservices_dep = dependency('appleframeworks', modules: ['ApplicationServices'])
carbon_dep = dependency('appleframeworks', modules: ['Carbon'])
foundation_dep = dependency('appleframeworks', modules: ['Foundation'])
appkit_dep = dependency('appleframeworks', modules: ['AppKit'])
iokit_dep = dependency('appleframeworks', modules: ['IOKit'])

all_deps = [
    glib_dep,
    gio_dep,
    cairo_dep,
    freerdp_dep,
    freerdp_server_dep,
    winpr_dep,
    coregraphics_dep,
    coremedia_dep,
    corevideo_dep,
    videotoolbox_dep,
    screencapturekit_dep,
    accelerate_dep,
    appservices_dep,
    carbon_dep,
    foundation_dep,
    appkit_dep,
    iokit_dep,
]

sources = files(
    'src/main.c',
    'src/rdp/mrd-rdp-server.c',
    'src/rdp/mrd-session-rdp.c',
    'src/rdp/mrd-session-rdp-frame.c',
    'src/rdp/mrd-session-rdp-cursor.c',
    'src/rdp/mrd-session-rdp-handshake.c',
    'src/rdp/mrd-rdp-graphics-pipeline.c',
    'src/rdp/mrd-rdp-disp.c',
    'src/rdp/mrd-rdp-clipboard.m',
    'src/rdp/mrd-rdp-audio.c',
    'src/platform/mrd-screen-capture.m',
    'src/platform/mrd-virtual-display.m',
    'src/platform/mrd-input-injector.m',
    'src/platform/mrd-cursor-capture.m',
    'src/encoding/mrd-encode-session.m',
    'src/util/mrd-auth.c',
    'src/util/mrd-bitstream.c',
    'src/util/mrd-damage-utils.c',
    'src/util/mrd-telemetry.c',
)

inc = include_directories('src')

exe = executable('rdp-server',
    sources,
    dependencies: all_deps,
    include_directories: inc,
    install: true,
    objc_args: ['-fobjc-arc'],
)

codesign = find_program('codesign')
custom_target('codesign',
    output: 'rdp-server.signed',
    input: exe,
    command: [codesign, '--force', '--sign', '-', '@INPUT@'],
    build_by_default: true,
)

summary({
    'GLib version': glib_dep.version(),
    'FreeRDP version': freerdp_dep.version(),
}, section: 'Dependencies')
