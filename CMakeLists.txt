cmake_minimum_required(VERSION 3.20)

# Otomatik artan versiyon (Git Commit Count)
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE OKIDIA_COMMIT_COUNT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
else()
    set(OKIDIA_COMMIT_COUNT "0")
endif()

project(OKIDIA VERSION 0.1.${OKIDIA_COMMIT_COUNT} LANGUAGES CXX)

# Varsayılan olarak Release modunu seç (performans ve gecikmeleri önlemek için)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "CMAKE_BUILD_TYPE belirtilmedi, varsayılan olarak 'Release' seçiliyor.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build tipi seçin: Debug, Release, RelWithDebInfo, MinSizeRel" FORCE)
endif()

# C++20 zorunlu
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt için gerekli
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_SOURCE_DIR}/master/ui)

# Build dizinine compile_commands.json üret (clangd için)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Qt6 paketleri
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Network
    Widgets
    Concurrent
    Multimedia
    Sql
    LinguistTools
)

# OpenSSL
find_package(OpenSSL REQUIRED)

# POSIX Threads (std::async için)
find_package(Threads REQUIRED)

# libssh2 & Systemd
if(UNIX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSSH2 REQUIRED libssh2)
    pkg_check_modules(SYSTEMD libsystemd)
else()
    find_package(libssh2 CONFIG QUIET)
    if(NOT libssh2_FOUND)
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(LIBSSH2 libssh2)
        endif()
    endif()
    if(NOT libssh2_FOUND AND NOT LIBSSH2_FOUND)
        message(STATUS "Windows: libssh2 FetchContent ile temin ediliyor...")
        include(FetchContent)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(CRYPTO_BACKEND "OpenSSL" CACHE STRING "" FORCE)
        set(ENABLE_ZLIB_COMPRESSION OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            libssh2
            GIT_REPOSITORY https://github.com/libssh2/libssh2.git
            GIT_TAG master
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(libssh2)
        set(LIBSSH2_LIBRARIES libssh2_static)
        set(LIBSSH2_INCLUDE_DIRS ${libssh2_SOURCE_DIR}/include)
        set(LIBSSH2_FOUND TRUE)
    endif()
endif()

# Protobuf
if(WIN32)
    find_package(Protobuf QUIET)
    if(NOT Protobuf_FOUND)
        message(STATUS "Windows: Protobuf kütüphanesi aranıyor veya FetchContent ile v21.12 derleniyor...")
        include(FetchContent)
        set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(protobuf_BUILD_PROTOC OFF CACHE BOOL "" FORCE)
        set(protobuf_BUILD_LIBPROTOC OFF CACHE BOOL "" FORCE)
        set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
        set(protobuf_DISABLE_RTTI OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            protobuf
            GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
            GIT_TAG v21.12
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(protobuf)
        set(Protobuf_LIBRARIES libprotobuf)
        set(Protobuf_INCLUDE_DIRS ${protobuf_SOURCE_DIR}/src)
        set(Protobuf_FOUND TRUE)

        find_program(PROTOC_BIN protoc HINTS "C:/CODE/mingw64/bin")
        if(NOT PROTOC_BIN)
            set(PROTOC_BIN protoc)
        endif()

        function(protobuf_generate_cpp SRCS HDRS)
            cmake_parse_arguments(protobuf "" "EXPORT_MACRO" "" ${ARGN})
            set(EXTRA_ARGS "")
            if(protobuf_EXPORT_MACRO)
                set(EXTRA_ARGS --dllexport_decl=${protobuf_EXPORT_MACRO})
            endif()

            set(${SRCS})
            set(${HDRS})
            foreach(FIL ${protobuf_UNPARSED_ARGUMENTS})
                get_filename_component(ABS_FIL "${FIL}" ABSOLUTE)
                get_filename_component(FIL_WE "${FIL}" NAME_WLE)
                get_filename_component(FIL_DIR "${FIL}" DIRECTORY)
                if(FIL_DIR)
                    set(OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/${FIL_DIR}")
                else()
                    set(OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
                endif()

                set(OUT_CC "${OUT_DIR}/${FIL_WE}.pb.cc")
                set(OUT_H "${OUT_DIR}/${FIL_WE}.pb.h")
                list(APPEND ${SRCS} "${OUT_CC}")
                list(APPEND ${HDRS} "${OUT_H}")

                add_custom_command(
                    OUTPUT "${OUT_CC}" "${OUT_H}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${OUT_DIR}"
                    COMMAND ${PROTOC_BIN}
                    ARGS --cpp_out=${EXTRA_ARGS}:${CMAKE_CURRENT_BINARY_DIR}
                         -I "${CMAKE_CURRENT_SOURCE_DIR}"
                         -I "${protobuf_SOURCE_DIR}/src"
                         "${ABS_FIL}"
                    DEPENDS "${ABS_FIL}"
                    COMMENT "Running C++ protocol buffer compiler on ${FIL}"
                    VERBATIM
                )
            endforeach()
            set_source_files_properties(${${SRCS}} ${${HDRS}} PROPERTIES GENERATED TRUE SKIP_AUTOGEN ON SKIP_AUTOMOC ON SKIP_AUTOUIC ON)
            set(${SRCS} ${${SRCS}} PARENT_SCOPE)
            set(${HDRS} ${${HDRS}} PARENT_SCOPE)
        endfunction()
    endif()
else()
    find_package(Protobuf REQUIRED)
endif()

# Alt modüller
add_subdirectory(common)
add_subdirectory(broker)
add_subdirectory(agent)
add_subdirectory(master)

# Paketleme (Debian)
if(UNIX)
    include(packaging/CMakeLists.txt)
endif()

# Versiyon bilgisi (OkidiaVersion kütüphanesine izole edilmiştir)
message(STATUS "OKIDIA v${PROJECT_VERSION}")
message(STATUS "Qt version: ${Qt6_VERSION}")
message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")

