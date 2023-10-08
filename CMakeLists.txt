cmake_minimum_required(VERSION 3.12)

project(IPOverTelegram VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(fmt)
set(ENABLE_CXX ON)
add_subdirectory(libtuntap)
add_subdirectory(rapidyaml)
add_subdirectory(td)

find_package(Threads REQUIRED)
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost COMPONENTS headers program_options REQUIRED)
message(STATUS "Boost version: ${Boost_VERSION}")
message(STATUS "Boost include dirs: ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost libraries: ${Boost_LIBRARIES}")


add_executable(IPOverTelegram main.cpp)
target_link_libraries(IPOverTelegram PUBLIC
        Threads::Threads
        Boost::headers
        Boost::program_options
        fmt
        tuntap++
        ryml::ryml
        tdclient
        )

# add address sanitizers and ub sanitizers
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(IPOverTelegram PRIVATE -fsanitize=address -fsanitize=undefined -g)
    target_link_options(IPOverTelegram PRIVATE -fsanitize=address -fsanitize=undefined)
endif ()
