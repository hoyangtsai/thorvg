gst_dep = [
    dependency('gstreamer-1.0', required: false),
    dependency('gstreamer-app-1.0', required: false),
    dependency('gstreamer-video-1.0', required: false)
]

gst_found = true
foreach dep : gst_dep
    if not dep.found()
        gst_found = false
    endif
endforeach

if gst_found
    compiler_flags += ['-DTHORVG_LINUX_MEDIA_SUPPORT=1']

    source_file = [
        'tvgGstMediaLoader.h',
        'tvgGstMediaLoader.cpp'
    ]

    media_dep += [declare_dependency(
        dependencies : gst_dep,
        include_directories : include_directories('.'),
        sources : source_file
    )]
endif
