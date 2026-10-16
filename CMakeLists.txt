cmake_minimum_required(VERSION 3.16)

project(sky2hook CXX C)

add_library(sky2hook STATIC
	src/api.cpp
	src/device.cpp
	src/gl.cpp
	src/hooks.cpp
	src/object_manager.cpp
	src/overlay.cpp
	src/render_queue.cpp
	src/renderer.cpp
	src/rw_to_gl.cpp
	src/shader_manager.cpp
	src/state.cpp
	src/texture_decoder.cpp
	src/texture_manager.cpp
	src/trace.cpp
	src/window.cpp
	src/common/frame.cpp
	src/common/geometry_pool.cpp
	src/enhancements/feedback_blur.cpp
	src/enhancements/interpolator.cpp
	src/enhancements/interpolator_debug.cpp
	src/enhancements/msaa.cpp
	src/enhancements/shadow_detail.cpp
	src/material/material.cpp
	src/material/pipeline_dump.cpp
	src/pipelines/atomic/atomic.cpp
	src/pipelines/atomic/ptank.cpp
	src/pipelines/im2d/im2d.cpp
	src/pipelines/im3d/im3d.cpp
	src/pipelines/rt2d/rt2d.cpp
	src/pipelines/sector/native_data.cpp
	src/pipelines/sector/sector.cpp
	src/pipelines/sector/vif.cpp
	thirdparty/glad/gl.c
	thirdparty/imgui/src/imgui.cpp
	thirdparty/imgui/src/imgui_draw.cpp
	thirdparty/imgui/src/imgui_tables.cpp
	thirdparty/imgui/src/imgui_widgets.cpp)

set_target_properties(sky2hook PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON C_STANDARD 11)

target_compile_options(sky2hook PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_include_directories(sky2hook
	PUBLIC include
	PRIVATE src src/common src/material
	PRIVATE thirdparty/glad/include thirdparty/imgui/include)

# The shaders are read from disk at runtime, so editing one needs no rebuild. SKY2_SHADER_DIR
# overrides this default for a deployed binary.
target_compile_definitions(sky2hook PRIVATE SKY2_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders")

find_package(SDL3 REQUIRED)  # the overlay reads the host's SDL events
find_package(ZLIB REQUIRED)       # per-pass capture dumps are PNG
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED COMPONENTS OpenGL GLX)
find_package(X11 REQUIRED)        # the self-created window, for headless and replay runs
find_library(XXHASH_LIBRARY NAMES xxhash REQUIRED)

target_link_libraries(sky2hook
	PRIVATE SDL3::SDL3 ZLIB::ZLIB Threads::Threads OpenGL::GL OpenGL::GLX X11::X11 ${XXHASH_LIBRARY})
