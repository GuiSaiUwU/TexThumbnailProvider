cmake_minimum_required(VERSION 3.20)

project(TexThumbnailProvider LANGUAGES CXX)

if(NOT WIN32)
	message(FATAL_ERROR "TexThumbnailProvider builds only on Windows.")
endif()

add_library(TexThumbnailProvider SHARED
	src/Dll.cpp
	src/TexThumbnailProvider.cpp
	src/s3tc.cpp
	TexThumbnailProvider.def
)

target_include_directories(TexThumbnailProvider PRIVATE src)

target_compile_definitions(TexThumbnailProvider PRIVATE
	_WINDOWS
	_USRDLL
)

if(MSVC)
	target_compile_options(TexThumbnailProvider PRIVATE /W3 /WX)
	target_link_options(TexThumbnailProvider PRIVATE
		/SUBSYSTEM:WINDOWS
		$<$<CONFIG:Release>:/OPT:REF>
		$<$<CONFIG:Release>:/OPT:ICF>
	)
	set_property(TARGET TexThumbnailProvider PROPERTY
		MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(TexThumbnailProvider PRIVATE
	shlwapi
	windowscodecs
)
