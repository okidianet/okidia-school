# agent/CMakeLists.txt

# ─────────────────────────────────────────────────────────────────────────────
# okidia-agent — Sistem servisi (QCoreApplication, root / SYSTEM)
# ─────────────────────────────────────────────────────────────────────────────
add_executable(okidia-agent
    main.cpp
    core/AgentApp.cpp
    core/LocalScheduleWatchdog.cpp
    core/AgentIpcServer.cpp
    core/AgentDiscoveryCoordinator.cpp
    system/SystemInfo.cpp
    system/PersistentStateManager.cpp
    system/SessionSwitchManager.cpp
    web/WebFilter.cpp
    OtaUpdater.cpp
    network/DiscoveryService.cpp
    transfer/FileReceiver.cpp
    auth/OfflineAuthManager.cpp
)

target_include_directories(okidia-agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(okidia-agent PRIVATE
    okidia_common
    Qt6::Core
    Qt6::Network
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
)

if(WIN32)
    target_link_libraries(okidia-agent PRIVATE
        ws2_32
        iphlpapi
        user32
        advapi32
        shell32
        wtsapi32
        userenv
    )
else()
    if(SYSTEMD_FOUND)
        target_link_libraries(okidia-agent PRIVATE ${SYSTEMD_LIBRARIES})
        target_include_directories(okidia-agent PRIVATE ${SYSTEMD_INCLUDE_DIRS})
        target_compile_definitions(okidia-agent PRIVATE HAVE_SYSTEMD)
    endif()
endif()

# ─────────────────────────────────────────────────────────────────────────────
# okidia-tray — Kullanıcı oturumu UI (QApplication, autostart)
# ─────────────────────────────────────────────────────────────────────────────
set(CMAKE_AUTOUIC_SEARCH_PATHS
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ScreenLockOverlay
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/PopupMessage
)

add_executable(okidia-tray
    main_tray.cpp
    capture/ScreenCapture.cpp
    input/InputController.cpp
    commands/CommandHandler.cpp
    commands/CommandHandlerScreen.cpp
    commands/CommandHandlerInput.cpp
    commands/CommandHandlerPower.cpp
    commands/CommandHandlerAudio.cpp
    commands/CommandHandlerApp.cpp
    system/SystemInfo.cpp
    system/SessionSwitchManager.cpp
    auth/OfflineAuthManager.cpp
    ui/ScreenLockOverlay/ScreenLockOverlay.cpp
    ui/SmartboardLockOverlay/SmartboardLockOverlay.cpp
    ui/RecessDismissDialog/RecessDismissDialog.cpp
    ui/PopupMessage/PopupMessage.cpp
    ui/BellToastBanner/BellToastBanner.cpp
    ui/AgentTray/AgentTray.cpp
    ui/AgentChatWindow/AgentChatWindow.cpp
    chat/ChatHandler.cpp
    transfer/FileReceiver.cpp
    broadcast/ScreenBroadcastReceiver.cpp
    audio/AudioPlayer.cpp
    audio/LocalPlaylistPlayer.cpp
    exam/ExamHandler.cpp
)

target_include_directories(okidia-tray PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/auth
    ${CMAKE_CURRENT_SOURCE_DIR}/system
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ScreenLockOverlay
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/SmartboardLockOverlay
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/RecessDismissDialog
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/PopupMessage
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/BellToastBanner
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/AgentTray
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/AgentChatWindow
    ${CMAKE_CURRENT_SOURCE_DIR}/chat
    ${CMAKE_CURRENT_SOURCE_DIR}/transfer
    ${CMAKE_CURRENT_SOURCE_DIR}/broadcast
)

target_link_libraries(okidia-tray PRIVATE
    okidia_common
    Qt6::Core
    Qt6::Network
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
)

if(WIN32)
    target_link_options(okidia-tray PRIVATE -mwindows)
    target_link_libraries(okidia-tray PRIVATE
        user32
        gdi32
        dwmapi
        shell32
        ole32
        oleaut32
        wtsapi32
        userenv
    )
else()
    target_link_libraries(okidia-tray PRIVATE
        X11
        Xfixes
    )

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XTST xtst)
    pkg_check_modules(XFIXES xfixes)

    if(XTST_FOUND)
        target_link_libraries(okidia-tray PRIVATE ${XTST_LIBRARIES})
        target_include_directories(okidia-tray PRIVATE ${XTST_INCLUDE_DIRS})
        target_compile_definitions(okidia-tray PRIVATE HAVE_XTST)
    endif()

    if(XFIXES_FOUND)
        target_link_libraries(okidia-tray PRIVATE ${XFIXES_LIBRARIES})
        target_include_directories(okidia-tray PRIVATE ${XFIXES_INCLUDE_DIRS})
        target_compile_definitions(okidia-tray PRIVATE HAVE_XFIXES)
    endif()
endif()

qt_add_translations(okidia-tray
    TS_FILES
        ${CMAKE_SOURCE_DIR}/translations/okidia_en.ts
        ${CMAKE_SOURCE_DIR}/translations/okidia_tr.ts
)

qt6_add_resources(okidia-tray "agent_resources"
    PREFIX "/"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES
        ${CMAKE_SOURCE_DIR}/resources/html/blocked.html
        ${CMAKE_SOURCE_DIR}/resources/sound/dingdong.wav
        ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-connected-16.png
        ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-connected-22.png
        ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-connected-32.png
        ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-search1-16.png
        ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-search1-22.png
        ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-search1-32.png
        ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-search2-16.png
        ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-search2-22.png
        ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-search2-32.png
        ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-search3-16.png
        ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-search3-22.png
        ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-search3-32.png
        ${CMAKE_SOURCE_DIR}/resources/ui/add.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/add.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/admin.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/admin.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/arrow_left.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/arrow_left.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/arrow_right.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/arrow_right.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/bell.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/bell.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/broadcast.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/broadcast.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/chart.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/chart.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/chat.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/chat.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/check_circle.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/check_circle.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/checked.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/checked.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/clear.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/clear.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/clock.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/clock.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/crown.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/crown.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/custom_profile.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/custom_profile.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/discovery.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/discovery.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/edit.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/edit.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/error_circle.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/error_circle.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/false.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/false.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/file.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/file.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/forbidden.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/forbidden.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/fullscreen.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/fullscreen.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/graduation.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/graduation.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/group.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/group.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/hand.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/hand.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/home.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/home.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/hourglass.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/hourglass.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/key.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/key.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/lock.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/lock.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/monitor.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/monitor.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/mute.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/mute.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/network.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/network.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/offline.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/offline.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/online.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/online.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/play.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/play.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/pause.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/pause.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/play_cmd.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/play_cmd.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/power.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/power.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/record.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/record.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/refresh.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/refresh.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/root_drive.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/root_drive.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/save.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/save.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/save_template.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/save_template.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/school.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/school.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/smartboard.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/smartboard.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/stop.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/stop.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/student.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/student.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/teacher.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/teacher.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/settings.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/settings.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/template_settings.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/template_settings.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/transfer_left.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/transfer_left.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/transfer_right.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/transfer_right.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/trash.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/trash.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/true.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/true.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/unlock.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/unlock.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/volume.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/volume.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/warning.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/warning.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/shield.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/shield.svg
)

# ─────────────────────────────────────────────────────────────────────────────
# Kurulum (Linux Packaging)
# ─────────────────────────────────────────────────────────────────────────────
if(UNIX)
    install(TARGETS okidia-agent RUNTIME DESTINATION /usr/bin COMPONENT okidia-agent)
    install(TARGETS okidia-tray RUNTIME DESTINATION /usr/bin COMPONENT okidia-agent)

    install(FILES ${CMAKE_SOURCE_DIR}/packaging/debian/okidia-agent.service
        DESTINATION /etc/systemd/system COMPONENT okidia-agent)

    install(FILES ${CMAKE_SOURCE_DIR}/packaging/debian/okidia-agent-recover.service
        DESTINATION /etc/systemd/system COMPONENT okidia-agent)

    install(PROGRAMS ${CMAKE_SOURCE_DIR}/packaging/debian/okidia-agent-recover.sh
        DESTINATION /usr/bin COMPONENT okidia-agent)

    install(FILES ${CMAKE_SOURCE_DIR}/packaging/debian/okidia-tray.desktop
        DESTINATION /etc/xdg/autostart COMPONENT okidia-agent)

    install(FILES ${CMAKE_SOURCE_DIR}/packaging/debian/okidia-agent.desktop
        DESTINATION /usr/share/applications COMPONENT okidia-agent)

    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE.txt
        DESTINATION /usr/share/doc/okidia-agent
        RENAME copyright COMPONENT okidia-agent)

    install(DIRECTORY ${CMAKE_SOURCE_DIR}/resources/sound/
        DESTINATION /usr/share/okidia/sounds COMPONENT okidia-agent
        FILES_MATCHING PATTERN "*.wav" PATTERN "*.mp3")

    foreach(SIZE 16 22 32)
        install(FILES ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-connected-${SIZE}.png
            DESTINATION /usr/share/icons/hicolor/${SIZE}x${SIZE}/apps
            COMPONENT okidia-agent RENAME okidia-agent.png)
        install(FILES ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-search1-${SIZE}.png
            DESTINATION /usr/share/icons/hicolor/${SIZE}x${SIZE}/apps
            COMPONENT okidia-agent RENAME okidia-agent-search1.png)
        install(FILES ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-search2-${SIZE}.png
            DESTINATION /usr/share/icons/hicolor/${SIZE}x${SIZE}/apps
            COMPONENT okidia-agent RENAME okidia-agent-search2.png)
        install(FILES ${CMAKE_SOURCE_DIR}/resources/icons/agent/okidia-agent-search3-${SIZE}.png
            DESTINATION /usr/share/icons/hicolor/${SIZE}x${SIZE}/apps
            COMPONENT okidia-agent RENAME okidia-agent-search3.png)
    endforeach()
endif()
