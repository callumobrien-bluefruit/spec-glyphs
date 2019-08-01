cmake_minimum_required(VERSION 2.8.12)
project(spec-glyphs)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup()

add_executable(spec-glyphs main.c)
target_link_libraries(spec-glyphs ${CONAN_LIBS})

if(MSVC)
	target_compile_options(spec-glyphs PRIVATE /W4)
else()
	target_compile_options(spec-glyphs PRIVATE -Wall -Wextra -std=c99 -pedantic)
endif()
