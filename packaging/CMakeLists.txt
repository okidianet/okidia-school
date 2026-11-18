# packaging/CMakeLists.txt
# CPack ile .deb paketi üretimi

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "okidia")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "OKIDIA")
set(CPACK_PACKAGE_CONTACT "Onur Tamer Ekmenci <info@okidia.net>")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Onur Tamer Ekmenci <info@okidia.net>")
set(CPACK_DEBIAN_PACKAGE_SECTION "admin")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://www.okidia.net")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "OKIDIA — Okul İçi Denetim ve İletişim Ağı")

# ── Derleme platformu tespiti ────────────────────────────────────

set(DEB_ARCH "amd64")

if(UNIX AND NOT APPLE)
    set(OS_ID "linux")
    set(OS_VERSION_ID "0")
    set(OS_TAG "linux")

    if(EXISTS "/etc/os-release")
        file(READ "/etc/os-release" OS_RELEASE_CONTENT)

        # ID çıkar
        if(OS_RELEASE_CONTENT MATCHES "(^|\n)ID=\"?([-a-zA-Z0-9_.]+)\"?")
            set(OS_ID "${CMAKE_MATCH_2}")
        endif()

        # ID_LIKE çıkar
        if(OS_RELEASE_CONTENT MATCHES "(^|\n)ID_LIKE=\"?([-a-zA-Z0-9_. ]+)\"?")
            set(OS_ID_LIKE "${CMAKE_MATCH_2}")
        endif()

        # VERSION_ID çıkar
        if(OS_RELEASE_CONTENT MATCHES "(^|\n)VERSION_ID=\"?([-a-zA-Z0-9_.]+)\"?")
            set(OS_VERSION_ID "${CMAKE_MATCH_2}")
        endif()

        # VERSION_CODENAME çıkar
        if(OS_RELEASE_CONTENT MATCHES "(^|\n)VERSION_CODENAME=\"?([-a-zA-Z0-9_.]+)\"?")
            set(VERSION_CODENAME "${CMAKE_MATCH_2}")
        endif()

        # UBUNTU_CODENAME çıkar
        if(OS_RELEASE_CONTENT MATCHES "(^|\n)UBUNTU_CODENAME=\"?([-a-zA-Z0-9_.]+)\"?")
            set(UBUNTU_CODENAME "${CMAKE_MATCH_2}")
        endif()
    endif()

    set(OS_TAG "")

    # 1. First try explicit known codenames
    if(UBUNTU_CODENAME STREQUAL "noble")
        set(OS_TAG "ubuntu24")
    elseif(UBUNTU_CODENAME STREQUAL "jammy")
        set(OS_TAG "ubuntu22")
    elseif(UBUNTU_CODENAME STREQUAL "focal")
        set(OS_TAG "ubuntu20")
    elseif(VERSION_CODENAME MATCHES "^(trixie|bookworm|bullseye|buster)$")
        if(VERSION_CODENAME STREQUAL "trixie")
            set(OS_TAG "debian13")
        elseif(VERSION_CODENAME STREQUAL "bookworm")
            set(OS_TAG "debian12")
        elseif(VERSION_CODENAME STREQUAL "bullseye")
            set(OS_TAG "debian11")
        elseif(VERSION_CODENAME STREQUAL "buster")
            set(OS_TAG "debian10")
        endif()
    endif()

    # 2. Try Debian base via /etc/debian_version (Global fallback for Pardus, Kali, MX Linux etc.)
    if(OS_TAG STREQUAL "" AND EXISTS "/etc/debian_version")
        file(READ "/etc/debian_version" DEB_VER_CONTENT)
        if(DEB_VER_CONTENT MATCHES "^([0-9]+)")
            set(OS_TAG "debian${CMAKE_MATCH_1}")
        endif()
    endif()

    # 3. Final Fallbacks for standard names
    if(OS_TAG STREQUAL "")
        if(OS_ID STREQUAL "ubuntu")
            string(REGEX REPLACE "[^0-9]" "" SANITIZED_VER "${OS_VERSION_ID}")
            set(OS_TAG "ubuntu${SANITIZED_VER}")
        elseif(OS_ID STREQUAL "debian")
            string(REGEX REPLACE "[^0-9]" "" SANITIZED_VER "${OS_VERSION_ID}")
            set(OS_TAG "debian${SANITIZED_VER}")
        elseif(OS_ID STREQUAL "arch" OR OS_ID_LIKE MATCHES "arch")
            set(OS_TAG "arch")
        elseif(OS_ID_LIKE MATCHES "rhel|fedora|centos|suse")
            set(OS_TAG "rhel")
        else()
            string(REGEX REPLACE "[^a-z0-9]" "" SANITIZED_ID "${OS_ID}")
            string(REGEX REPLACE "[^0-9]" "" SANITIZED_VER "${OS_VERSION_ID}")
            set(OS_TAG "${SANITIZED_ID}${SANITIZED_VER}")
        endif()
    endif()

    # Mimariyi dinamik olarak tespit et (Debian paket ismi için)
    execute_process(
        COMMAND dpkg --print-architecture
        OUTPUT_VARIABLE DETECTED_DEB_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(DETECTED_DEB_ARCH AND NOT DETECTED_DEB_ARCH STREQUAL "")
        set(DEB_ARCH "${DETECTED_DEB_ARCH}")
    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
            set(DEB_ARCH "amd64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i.86|x86")
            set(DEB_ARCH "i386")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
            set(DEB_ARCH "arm64")
        else()
            set(DEB_ARCH "amd64")
        endif()
    endif()
elseif(APPLE)
    execute_process(COMMAND sw_vers -productVersion
        OUTPUT_VARIABLE OS_VERSION_ID OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX REPLACE "[^0-9]" "" OS_VERSION_ID "${OS_VERSION_ID}")
    set(OS_TAG "macos${OS_VERSION_ID}")
elseif(WIN32)
    set(OS_TAG "windows")
else()
    set(OS_TAG "linux")
endif()

message(STATUS "OKIDIA: Derleme platformu → ${OS_TAG}")

# Bileşen bazlı paketleme
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL okidia-agent okidia-master okidia-broker)

# ── Bağımlılıklar ─────────────────────────────────────────────
set(CPACK_DEBIAN_OKIDIA-AGENT_PACKAGE_DEPENDS
    "libqt6core6 (>= 6.4), libqt6network6, libqt6sql6, libqt6gui6, libqt6widgets6, libqt6multimedia6, libqt6svg6, qt6-translations-l10n, libssl3, libprotobuf32 | libprotobuf-dev, libavahi-client3, avahi-daemon, libxtst6, libxcb-cursor0, x11-utils, openssl, iptables, alsa-utils, mpg123, gstreamer1.0-plugins-good, gstreamer1.0-alsa, gstreamer1.0-pulseaudio")

set(CPACK_DEBIAN_OKIDIA-MASTER_PACKAGE_DEPENDS
    "libqt6core6 (>= 6.4), libqt6network6, libqt6gui6, libqt6widgets6, libqt6multimedia6, libqt6sql6, libqt6svg6, qt6-translations-l10n, libssl3, libprotobuf32 | libprotobuf-dev, libavahi-client3, avahi-daemon, libsqlite3-0, libssh2-1, openssl, libxcb-cursor0, openssh-client, alsa-utils, gstreamer1.0-plugins-good, gstreamer1.0-alsa, gstreamer1.0-pulseaudio")

set(CPACK_DEBIAN_OKIDIA-BROKER_PACKAGE_DEPENDS
    "libqt6core6 (>= 6.4), libqt6network6, libqt6sql6, libssl3, libprotobuf32 | libprotobuf-dev, libavahi-client3, avahi-daemon, openssl")

# ── Paket Çakışmaları (Conflicts) ─────────────────────────────────
# okidia-agent, master ve broker ile çakışır (aynı makineye kurulamaz)
set(CPACK_DEBIAN_OKIDIA-AGENT_PACKAGE_CONFLICTS "okidia-master, okidia-broker")
set(CPACK_DEBIAN_OKIDIA_AGENT_PACKAGE_CONFLICTS "okidia-master, okidia-broker")

# okidia-master ve okidia-broker aynı makineye kurulabilir ama agent ile çakışırlar
set(CPACK_DEBIAN_OKIDIA-MASTER_PACKAGE_CONFLICTS "okidia-agent")
set(CPACK_DEBIAN_OKIDIA_MASTER_PACKAGE_CONFLICTS "okidia-agent")

set(CPACK_DEBIAN_OKIDIA-BROKER_PACKAGE_CONFLICTS "okidia-agent")
set(CPACK_DEBIAN_OKIDIA_BROKER_PACKAGE_CONFLICTS "okidia-agent")

# ── Control scriptleri ────────────────────────────────────────
foreach(COMP okidia-agent okidia-master okidia-broker)
    string(TOUPPER ${COMP} COMP_UPPER)
    set(CPACK_DEBIAN_${COMP_UPPER}_PACKAGE_CONTROL_EXTRA
        "${CMAKE_SOURCE_DIR}/packaging/debian/preinst"
        "${CMAKE_SOURCE_DIR}/packaging/debian/postinst"
        "${CMAKE_SOURCE_DIR}/packaging/debian/prerm"
        "${CMAKE_SOURCE_DIR}/packaging/debian/postrm")
endforeach()

# ── Paket adları — platform dahil ────────────────────────────
# Format: okidia-agent-pardus23_0.1.0_amd64.deb
# Debian kuralı: paket-adı_versiyon_arch.deb
# Paket adında sadece: a-z 0-9 + - .
# Örnek sonuçlar:
#   Pardus 23   → okidia-agent-pardus23_0.1.0_amd64.deb
#   Ubuntu 22   → okidia-agent-ubuntu22_0.1.0_amd64.deb
#   Debian 12   → okidia-agent-debian12_0.1.0_amd64.deb
# ── Paket adları — platform dahil ────────────────────────────
# Her component için paket adını açıkça belirt
# (CPACK_PACKAGE_NAME=okidia + component=okidia-agent → okidia-okidia-agent hatası önlenir)
set(CPACK_DEBIAN_OKIDIA-AGENT_PACKAGE_NAME  "okidia-agent")
set(CPACK_DEBIAN_OKIDIA-MASTER_PACKAGE_NAME "okidia-master")
set(CPACK_DEBIAN_OKIDIA-BROKER_PACKAGE_NAME "okidia-broker")

set(CPACK_DEBIAN_OKIDIA-AGENT_FILE_NAME
    "okidia-agent_${OS_TAG}_${PROJECT_VERSION}_${DEB_ARCH}.deb")
set(CPACK_DEBIAN_OKIDIA-MASTER_FILE_NAME
    "okidia-master_${OS_TAG}_${PROJECT_VERSION}_${DEB_ARCH}.deb")
set(CPACK_DEBIAN_OKIDIA-BROKER_FILE_NAME
    "okidia-broker_${OS_TAG}_${PROJECT_VERSION}_${DEB_ARCH}.deb")

include(CPack)

add_custom_target(deb
    COMMAND ${CMAKE_CPACK_COMMAND} --config CPackConfig.cmake -G DEB -B packages
    COMMAND ${CMAKE_COMMAND} -E rm -rf packages/_CPack_Packages
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Building DEB packages and cleaning up temporary CPack files..."
)
