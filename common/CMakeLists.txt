set(PROTO_FILES
    protocol/messages.proto
)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

# GNU Make / Ninja paralel derleme esnasında alt dizin bağımlılık (.d) yarışlarını önlemek için dizinleri oluştur
file(MAKE_DIRECTORY
    ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/okidia_common.dir/db
    ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/okidia_common.dir/crypto
    ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/okidia_common.dir/network
    ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/okidia_common.dir/utils
    ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/okidia_common.dir/ui
    ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/okidia_common.dir/ui/chat
    ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/okidia_common.dir/protocol
    ${CMAKE_CURRENT_BINARY_DIR}/protocol
)

# İzole sürüm kütüphanesi: Commit sayısı değiştiğinde 250 dosya yerine sadece OkidiaVersion.cpp derlenir.
add_library(okidia_version STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/version/OkidiaVersion.h
    ${CMAKE_CURRENT_SOURCE_DIR}/version/OkidiaVersion.cpp
)
target_link_libraries(okidia_version PRIVATE Qt6::Core)
target_include_directories(okidia_version PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/version
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(okidia_version PRIVATE
    "OKIDIA_BUILD_VERSION=\"${PROJECT_VERSION}\""
    "OKIDIA_BUILD_OS_TAG=\"${OS_TAG}\""
)

add_library(okidia_common STATIC
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/db/DbRecords.h
    ${CMAKE_CURRENT_SOURCE_DIR}/db/OkidiaDatabase.h
    ${CMAKE_CURRENT_SOURCE_DIR}/db/OkidiaDatabase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/db/OkidiaAuditDatabase.h
    ${CMAKE_CURRENT_SOURCE_DIR}/db/OkidiaAuditDatabase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/db/OkidiaChatDatabase.h
    ${CMAKE_CURRENT_SOURCE_DIR}/db/OkidiaChatDatabase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto/KeyManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto/AesGcm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto/BackupPackage.h
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto/BackupPackage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/network/Connection.h
    ${CMAKE_CURRENT_SOURCE_DIR}/network/Connection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/network/LocalConnection.h
    ${CMAKE_CURRENT_SOURCE_DIR}/network/LocalConnection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/network/TlsServer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/network/TlsServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/network/TlsClient.h
    ${CMAKE_CURRENT_SOURCE_DIR}/network/TlsClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/network/MdnsManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/network/MdnsManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/Logger.h
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/Logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/PlatformService.h
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/PlatformService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/QrCodeGenerator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/QrCodeGenerator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/qrcodegen.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/qrcodegen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/OkidiaUtils.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ThemeManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ThemeManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/chat/ChatMessage.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/chat/ChatModel.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/chat/ChatModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/chat/ChatMessageDelegate.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/chat/ChatMessageDelegate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/chat/ChatWidget.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/chat/ChatWidget.cpp
)

target_include_directories(okidia_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/network
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/protocol
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(okidia_common PUBLIC
    okidia_version
    Qt6::Core
    Qt6::Network
    Qt6::Gui
    Qt6::Widgets
    Qt6::Sql
    OpenSSL::SSL
    OpenSSL::Crypto
    ${Protobuf_LIBRARIES}
)

if(WIN32)
    target_link_libraries(okidia_common PUBLIC
        ws2_32
        iphlpapi
        user32
        advapi32
        shell32
        wtsapi32
        userenv
        crypt32
    )
endif()

# Avahi (mDNS)
pkg_check_modules(AVAHI avahi-client)

if(AVAHI_FOUND)
    target_include_directories(okidia_common PUBLIC ${AVAHI_INCLUDE_DIRS})
    target_link_libraries(okidia_common PUBLIC ${AVAHI_LIBRARIES})
    target_compile_definitions(okidia_common PUBLIC HAVE_AVAHI)
    message(STATUS "Avahi version: ${AVAHI_VERSION}")
else()
    message(WARNING "Avahi bulunamadı, mDNS desteği devre dışı")
endif()