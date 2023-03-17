cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

project(
    AltheaDemo
    VERSION 0.1.0
    LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include_directories(Include Src)

function(glob_files out_var_name regexes)
    set(files "")
    foreach(arg ${ARGV})
        list(APPEND regexes_only "${arg}")
    endforeach()
    list(POP_FRONT regexes_only)
    if (GLOB_USE_CONFIGURE_DEPENDS)
        file(GLOB_RECURSE files CONFIGURE_DEPENDS ${regexes_only})
    else()
        file(GLOB files ${regexes_only})
    endif()
    set(${ARGV0} "${files}" PARENT_SCOPE)
endfunction()

glob_files(SRC_FILES_LIST Src/*.cpp)
add_executable(AltheaDemo ${SRC_FILES_LIST})

target_compile_definitions(${PROJECT_NAME} PRIVATE MAX_UV_COORDS=4)

add_subdirectory(Extern/Althea)
# if (MSVC)
#     target_compile_options(${targetName} PRIVATE /W4 /WX /wd4201 /bigobj)
# else()
#     target_compile_options(${targetName} PRIVATE -Werror -Wall -Wextra -Wconversion -Wpedantic -Wshadow -Wsign-conversion)
# endif()
target_link_libraries(${PROJECT_NAME} PUBLIC Althea)

