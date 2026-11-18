# master/CMakeLists.txt

set(CMAKE_AUTOUIC_SEARCH_PATHS
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/MainWindow
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ClientThumbnail
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/LoginDialog
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/SettingsDialog
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/AnnounceRecordDialog
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ChatWindow
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/RemoteControlView
    ${CMAKE_CURRENT_SOURCE_DIR}/exam
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ReportPanel
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/BroadcastSourceDialog
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/SetupWizard
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/AgentTypeSelectionDialog
)

add_executable(okidia-master
    main.cpp
    session/SessionManager.cpp
    session/SessionManagerCommands.cpp
    session/SessionManagerDiscovery.cpp
    session/SessionManagerHandlers.cpp
    session/SessionManagerAudio.cpp
    session/NetworkWatcher.cpp
    ui/MainWindow/MainWindow.cpp
    ui/MainWindow/MainWindowActions.cpp
    ui/MainWindow/MainWindowEvents.cpp
    ui/MainWindow/MainWindowLayout.cpp
    ui/MainWindow/MainWindowCommands.cpp
    ui/MainWindow/MainWindowChat.cpp
    ui/MainWindow/MainWindowDiscovery.cpp
    ui/MainWindow/MainWindowClientOps.cpp
    ui/MainWindow/MainWindowExamAndFilter.cpp
    ui/MainWindow/MainWindowSettings.cpp
    ui/AnnounceRecordDialog/AnnounceRecordDialog.cpp
    ui/LiveAnnounceDialog/LiveAnnounceDialog.cpp
    ui/ClientThumbnail/ClientThumbnail.cpp
    ui/LiveTooltip/LiveTooltip.cpp
    ui/ThumbnailGroupFrame/ThumbnailGroupFrame.cpp
    ui/LoginDialog/LoginDialog.cpp
    ui/SetupWizard/SetupWizard.cpp
    ui/UserManagementPanel/UserManagementPanel.cpp
    ui/UserManagementPanel/UserManagementPanelUsers.cpp
    ui/UserManagementPanel/UserManagementPanelTeachers.cpp
    ui/UserManagementPanel/UserManagementPanelClasses.cpp
    ui/SettingsDialog/SettingsDialog.cpp
    ui/UninstallPasswordDialog/UninstallPasswordDialog.cpp
    ui/CustomProfilesDialog/CustomProfilesDialog.cpp
    ui/LogViewerDialog/LogViewerDialog.cpp
    ui/ChatWindow/ChatWindow.cpp
    ui/ChatWindow/RoomJoinRequestDialog.cpp
    ui/RemoteControlView/RemoteControlView.cpp
    ui/ReportPanel/ReportPanel.cpp
    ui/BroadcastSourceDialog/BroadcastSourceDialog.cpp
    ui/BroadcastOverlay/BroadcastOverlayWidget.cpp
    ui/SmartboardConfirmDialog/SmartboardConfirmDialog.cpp
    ui/AgentTypeSelectionDialog/AgentTypeSelectionDialog.cpp
    transfer/FileSender.cpp
    broadcast/ScreenBroadcaster.cpp
    exam/ExamDialog.cpp
    schedule/ScheduleManager.cpp
    audio/AudioBroadcaster.cpp
    audit/AuditLog.cpp
    license/LicenseManager.cpp
    backup/BackupManager.cpp
    ssh/SshClient.cpp
    ssh/AccordionLogWidget.cpp
    ssh/SshCredStore.cpp
    ssh/SshTerminalWidget.cpp
    ssh/SshDeviceSessionWidget.cpp
    ssh/SshCredSelectorWidget.cpp
    ssh/SshManagerPanel.cpp
    ssh/SshManagerPanelUi.cpp
    ssh/SshManagerPanelDiscovery.cpp
    ssh/SshManagerPanelSessions.cpp
    ssh/SshManagerPanelCommands.cpp
    ssh/SshTemplateDialog.cpp
    ssh/DeployPanel.cpp
    ssh/DeployPanelUi.cpp
    ssh/DeployPanelHandlers.cpp
    ssh/DeployPanelCommon.cpp
    ssh/DeployPanelPrepare.cpp
    ssh/DeployPanelExecution.cpp
    ssh/DeployWorker.cpp
    ssh/HostInstallCard.cpp
    ssh/DeployProgressGrid.cpp
    ssh/SshFileManagerWidget.cpp
    ssh/SshFileManagerRemote.cpp
    ssh/SshFileManagerLocal.cpp
    ssh/SshFileManagerTransfer.cpp
    ssh/OtaUpdatePanel.cpp
    ssh/BrokerUpdateManagerDialog.cpp
    transfer/OtaSender.cpp
    transfer/MasterUpdater.cpp
    ui/Widgets/MasterUpdateDialog.cpp
    ui/Widgets/MasterLockOverlay.cpp
    ssh/SshCredentialDialog.cpp
    ssh/TransferConflictDialog.cpp
    ssh/GitHubFetcher.cpp
    ssh/PackageRequirementSolver.cpp
    network/ReleaseManager.cpp
    ui/DownloadManagerDialog.cpp
    tree/TreeManager.cpp
    tree/TreeManagerUi.cpp
    tree/TreeContextMenu.cpp
    chat/RoomManager.cpp
    chat/PrivateChatManager.cpp
    diagnostics/DiagnosticCollector.cpp
    ui/ReportIssueDialog/ReportIssueDialog.cpp
)

target_include_directories(okidia-master PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/session
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/MainWindow
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ClientThumbnail
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ThumbnailGroupFrame
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/RemoteControlView
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ChatWindow
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ReportPanel
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/BroadcastSourceDialog
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/ReportIssueDialog
    ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics
    ${CMAKE_CURRENT_SOURCE_DIR}/transfer
    ${CMAKE_CURRENT_SOURCE_DIR}/broadcast
    ${CMAKE_CURRENT_SOURCE_DIR}/exam
    ${CMAKE_CURRENT_SOURCE_DIR}/schedule
    ${CMAKE_CURRENT_SOURCE_DIR}/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/audit
    ${CMAKE_CURRENT_SOURCE_DIR}/license
    ${CMAKE_CURRENT_SOURCE_DIR}/backup
    ${CMAKE_CURRENT_SOURCE_DIR}/ssh
    ${CMAKE_CURRENT_SOURCE_DIR}/tree
    ${CMAKE_CURRENT_SOURCE_DIR}/chat
    ${LIBSSH2_INCLUDE_DIRS}
)

target_link_libraries(okidia-master PRIVATE
    okidia_common
    Qt6::Core
    Qt6::Network
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Multimedia
    Qt6::Sql
    OpenSSL::SSL
    OpenSSL::Crypto
    ${LIBSSH2_LIBRARIES}
    Threads::Threads
)

if(WIN32)
    target_link_libraries(okidia-master PRIVATE ws2_32 iphlpapi)
    target_link_options(okidia-master PRIVATE -mwindows)
endif()

qt_add_translations(okidia-master
    TS_FILES
        ${CMAKE_SOURCE_DIR}/translations/okidia_en.ts
        ${CMAKE_SOURCE_DIR}/translations/okidia_tr.ts
)

qt6_add_resources(okidia-master "resources"
    PREFIX "/"
    BASE "${CMAKE_SOURCE_DIR}/resources"
    FILES
        ${CMAKE_SOURCE_DIR}/resources/styles/default.qss
        ${CMAKE_SOURCE_DIR}/resources/styles/dark.qss
        ${CMAKE_SOURCE_DIR}/resources/icons/master/okidia-master-16.png
        ${CMAKE_SOURCE_DIR}/resources/icons/master/okidia-master-22.png
        ${CMAKE_SOURCE_DIR}/resources/icons/master/okidia-master-32.png
        ${CMAKE_SOURCE_DIR}/resources/icons/master/okidia-master-48.png
        ${CMAKE_SOURCE_DIR}/resources/icons/master/okidia-master-64.png
        ${CMAKE_SOURCE_DIR}/resources/icons/master/okidia-master-128.png
        ${CMAKE_SOURCE_DIR}/resources/icons/master/okidia-master-256.png
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
        ${CMAKE_SOURCE_DIR}/resources/ui/arrow_up.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/arrow_up.svg
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
        ${CMAKE_SOURCE_DIR}/resources/ui/linux.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/linux.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/windows.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/windows.svg
        ${CMAKE_SOURCE_DIR}/resources/ui/shield.svg
        ${CMAKE_SOURCE_DIR}/resources/ui_dark/shield.svg
        ${CMAKE_SOURCE_DIR}/resources/sound/dingdong.wav
)

install(TARGETS okidia-master RUNTIME DESTINATION /usr/bin COMPONENT okidia-master)

# Başlat menüsü / masaüstü kısayolu
install(FILES
    ${CMAKE_SOURCE_DIR}/packaging/debian/okidia-master.desktop
    DESTINATION /usr/share/applications
    COMPONENT okidia-master)

install(FILES
    ${CMAKE_SOURCE_DIR}/LICENSE.txt
    DESTINATION /usr/share/doc/okidia-master
    RENAME copyright
    COMPONENT okidia-master)

# Başlat menüsü ikonu — freedesktop icon theme hiyerarşisi
# 256px: başlat menüsü, uygulama switcher
# 48px : küçük liste görünümü
# 32px : bazı DE'lerin varsayılan boyutu
foreach(SIZE 16 22 32 48 64 128 256)
    install(FILES
        ${CMAKE_SOURCE_DIR}/resources/icons/master/okidia-master-${SIZE}.png
        DESTINATION /usr/share/icons/hicolor/${SIZE}x${SIZE}/apps
        COMPONENT okidia-master
        RENAME okidia-master.png)
endforeach()
