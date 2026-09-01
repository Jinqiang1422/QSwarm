# QGC tests add sources to ${CMAKE_PROJECT_NAME} (test/Vehicle/, test/MissionManager/); keep this test on the same target.

target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        GStreamerTest.cc
        GStreamerTest.h
        HwBuffers/common/GStreamerHwBuffersCommonTest.cc
        HwBuffers/d3d/GStreamerD3DTest.cc
        HwBuffers/dmabuf/GStreamerDmaBufTest.cc
        HwBuffers/gl/GStreamerGlTest.cc
        HwBuffers/vulkan/GStreamerVulkanTest.cc
        SourceFactory/GStreamerSourceFactoryTest.cc
        gstqgc/GStreamerGstQgcTest.cc
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Docker's default seccomp profile can reject GLib's close-range operation while
# spawning gst-plugin-scanner. Scan in-process for this test so plugin coverage is
# preserved without relaxing the security profile of the entire test container.
add_qgc_test(GStreamerTest LABELS Integration ENV_MODIFICATION "GST_REGISTRY_FORK=set:no")
