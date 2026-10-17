cmake_minimum_required(VERSION 3.30)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(quake_tools)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} CACHE INTERNAL "")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARINGS _USE_MATH_DEFINES)
endif()

if(MSVC)
    include(FetchContent)

    FetchContent_Declare(
        dirent
        URL https://github.com/tronkko/dirent/archive/refs/tags/1.26.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    FetchContent_MakeAvailable(dirent)
endif()

set(IMGDUMP_SOURCES imgdump.cpp)

add_executable(imgdump ${IMGDUMP_SOURCES})

set(MUSENC_SOURCES musenc.cpp)

add_executable(musenc ${MUSENC_SOURCES})

set(UNPAK_SOURCES unpak.cpp)

add_executable(unpak ${UNPAK_SOURCES})

if(MSVC)
    target_include_directories(unpak PRIVATE
        "${dirent_SOURCE_DIR}/include"
    )
	
	target_link_libraries(unpak dirent)
endif()
