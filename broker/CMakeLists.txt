# broker/CMakeLists.txt

# ── okidia-broker: saf servis (QCoreApplication, headless) ───────────────────
add_executable(okidia-broker
    main.cpp
    handlers/MessageHandler.cpp
    handlers/AuthHandler.cpp
    handlers/ClientHandler.cpp
    handlers/ClassHandler.cpp
    handlers/ChatHandler.cpp
    handlers/SystemHandler.cpp
    BrokerIpcServer.cpp
    CertSigner.cpp
    ClientRegistry.cpp
    MulticastManager.cpp
    BrokerTransferManager.cpp
    EnrollmentManager.cpp
    BellScheduler.cpp
)

target_link_libraries(okidia-broker PRIVATE
    okidia_common
    Qt6::Core
    Qt6::Network
    Qt6::Sql
)

if(WIN32)
    target_link_libraries(okidia-broker PRIVATE
        ws2_32
        iphlpapi
        user32
        advapi32
        shell32
        wtsapi32
        userenv
    )
endif()

if(SYSTEMD_FOUND)
    target_link_libraries(okidia-broker PRIVATE ${SYSTEMD_LIBRARIES})
    target_include_directories(okidia-broker PRIVATE ${SYSTEMD_INCLUDE_DIRS})
    target_compile_definitions(okidia-broker PRIVATE HAVE_SYSTEMD)
endif()

# ── okidia-broker-tray: masaüstü tray (QApplication) ────────────────────────
add_executable(okidia-broker-tray
    main_tray.cpp
)

target_link_libraries(okidia-broker-tray PRIVATE
    okidia_common
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

if(WIN32)
    target_link_options(okidia-broker-tray PRIVATE -mwindows)
endif()

qt_add_translations(okidia-broker-tray
    TS_FILES
        ${CMAKE_SOURCE_DIR}/translations/okidia_en.ts
        ${CMAKE_SOURCE_DIR}/translations/okidia_tr.ts
)

# Broker tray ikonlarını resource olarak göm
qt6_add_resources(okidia-broker-tray "broker_tray_resources"
    PREFIX "/"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES
        ${CMAKE_SOURCE_DIR}/resources/icons/broker/okidia-broker-16.png
        ${CMAKE_SOURCE_DIR}/resources/icons/broker/okidia-broker-22.png
        ${CMAKE_SOURCE_DIR}/resources/icons/broker/okidia-broker-24.png
        ${CMAKE_SOURCE_DIR}/resources/icons/broker/okidia-broker-32.png
        ${CMAKE_SOURCE_DIR}/resources/icons/broker/okidia-broker-48.png
        ${CMAKE_SOURCE_DIR}/resources/icons/broker/okidia-broker-64.png
        ${CMAKE_SOURCE_DIR}/resources/icons/broker/okidia-broker-128.png
        ${CMAKE_SOURCE_DIR}/resources/icons/broker/okidia-broker-256.png
)

# ── Kurulum hedefleri ─────────────────────────────────────────────────────────
install(TARGETS okidia-broker okidia-broker-tray
    RUNTIME DESTINATION /usr/bin
    COMPONENT okidia-broker)

install(FILES
    ${CMAKE_SOURCE_DIR}/packaging/debian/okidia-broker.service
    DESTINATION /etc/systemd/system
    COMPONENT okidia-broker)

# XDG autostart — tray masaüstü oturumunda otomatik başlar
install(FILES
    ${CMAKE_SOURCE_DIR}/packaging/debian/okidia-broker-tray.desktop
    DESTINATION /etc/xdg/autostart
    COMPONENT okidia-broker)

# Başlat menüsünde gizli desktop entry
install(FILES
    ${CMAKE_SOURCE_DIR}/packaging/debian/okidia-broker.desktop
    DESTINATION /usr/share/applications
    COMPONENT okidia-broker)

install(FILES
    ${CMAKE_SOURCE_DIR}/LICENSE.txt
    DESTINATION /usr/share/doc/okidia-broker
    RENAME copyright
    COMPONENT okidia-broker)

# hicolor icon theme
foreach(SIZE 16 22 32)
    install(FILES
        ${CMAKE_SOURCE_DIR}/resources/icons/broker/okidia-broker-${SIZE}.png
        DESTINATION /usr/share/icons/hicolor/${SIZE}x${SIZE}/apps
        COMPONENT okidia-broker
        RENAME okidia-broker.png)
endforeach()
