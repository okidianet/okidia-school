// master/ssh/DeployPanel.cpp
#include "DeployPanel.h"
#include "SshCredSelectorWidget.h"
#include "../session/SessionManager.h"
#include "../../common/utils/Logger.h"
#include "../../common/version/OkidiaVersion.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QDateTime>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QEventLoop>
#include <QSplitter>
#include <QFormLayout>
#include <QCheckBox>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QStandardPaths>
#include <QApplication>
#include <QToolButton>
#include <QThreadPool>
#include <QtConcurrent>
#include "../common/OkidiaConfig.h"

// ── POSIX / Windows Sockets ───────────────────────────────────────────────────
#ifdef Q_OS_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <iphlpapi.h>
#  include <icmpapi.h>
#else
#  include <sys/socket.h>
#  include <sys/epoll.h>
#  include <poll.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <cerrno>
#include <csignal>
#include <cctype>

// ── STL ───────────────────────────────────────────────────────────────────────
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>


// ── Debug log (crash anında da çalışır) ──────────────────────────────────────
static FILE* g_scanLog = nullptr;

static void scanLog(const char* fmt, ...) {
    if (!g_scanLog) return;
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    ::fprintf(g_scanLog, "[%4lld.%03lld] ", static_cast<long long>((ms / 1000) % 10000), static_cast<long long>(ms % 1000));
    va_list ap; va_start(ap, fmt);
    ::vfprintf(g_scanLog, fmt, ap);
    va_end(ap);
    ::fputc('\n', g_scanLog);
    ::fflush(g_scanLog);
}

#ifdef Q_OS_UNIX
static void sigsegvHandler(int) {
    if (g_scanLog) {
        ::fprintf(g_scanLog, "\n!!! SIGSEGV — process crashed !!!\n");
        ::fflush(g_scanLog);
        ::fclose(g_scanLog);
        g_scanLog = nullptr;
    }
    ::signal(SIGSEGV, SIG_DFL);
    ::raise(SIGSEGV);
}
#endif

// ── Socket yardımcıları (sadece run() thread'inden çağrılır) ──────────────────

static bool posixTcpConnect(const char* ip, int port, int timeoutMs)
{
    scanLog("        posixTcpConnect(%s:%d) socket() →", ip, port);
#ifdef Q_OS_WIN
    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return false;
    u_long mode = 1;
    ::ioctlsocket(fd, FIONBIO, &mode);
#else
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    scanLog("        posixTcpConnect(%s:%d) inet_pton() →", ip, port);
    ::inet_pton(AF_INET, ip, &addr.sin_addr);
    scanLog("        posixTcpConnect(%s:%d) inet_pton() ← OK", ip, port);

    (void)::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    scanLog("        posixTcpConnect(%s:%d) connect() ← OK", ip, port);

    struct pollfd pfd{};
    pfd.fd      = static_cast<decltype(pfd.fd)>(fd);
    pfd.events  = POLLOUT;
    scanLog("        posixTcpConnect(%s:%d) poll(fd=%d, %dms) →", ip, port, (int)fd, timeoutMs);
#ifdef Q_OS_WIN
    int pr = ::WSAPoll(&pfd, 1, timeoutMs);
#else
    int pr = ::poll(&pfd, 1, timeoutMs);
#endif
    scanLog("        posixTcpConnect(%s:%d) poll() ← %d revents=0x%x", ip, port, pr, pfd.revents);
    bool ok = false;
    if (pr > 0) {
        int err = 0; socklen_t len = sizeof(err);
        scanLog("        posixTcpConnect(%s:%d) getsockopt() →", ip, port);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        scanLog("        posixTcpConnect(%s:%d) getsockopt() ← err=%d", ip, port, err);
        ok = (err == 0) && !(pfd.revents & (POLLERR | POLLHUP));
    }
    scanLog("        posixTcpConnect(%s:%d) close(fd=%d) →", ip, port, (int)fd);
#ifdef Q_OS_WIN
    ::closesocket(fd);
#else
    ::close(fd);
#endif
    scanLog("        posixTcpConnect(%s:%d) ← %s", ip, port, ok ? "true" : "false");
    return ok;
}

/** Reverse DNS — SADECE run() thread'inden çağrılır.
 *  getnameinfo() NSS plugin'lerini dlopen() ile yükler;
 *  başka thread'den çağrılırsa global ld.so state'i bozuluyor → SIGSEGV. */
// ── NetBIOS Node Status Sorgusu (UDP Port 137) ──────────────────────────────
// Windows ve Samba cihazların tam bilgisayar adını ve çalışma grubunu çözer.
static std::string queryNetbiosName(const char* ip, int timeoutMs = 250) {
#ifdef Q_OS_WIN
    SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return "";
    u_long mode = 1;
    ::ioctlsocket(sock, FIONBIO, &mode);
#else
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "";
    ::fcntl(sock, F_SETFL, ::fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif

    sockaddr_in destAddr{};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(137);
    ::inet_pton(AF_INET, ip, &destAddr.sin_addr);

    // RFC 1002 NetBIOS Node Status Request (* wildcard)
    static const unsigned char nbQuery[] = {
        0x80, 0x01, // Transaction ID
        0x00, 0x00, // Flags: Query
        0x00, 0x01, // Questions: 1
        0x00, 0x00, // Answer RRs: 0
        0x00, 0x00, // Authority RRs: 0
        0x00, 0x00, // Additional RRs: 0
        // Question: * <00> (encoded in NetBIOS 32-byte representation)
        0x20, // Length: 32 bytes
        'C','K','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
        'A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
        0x00,       // Terminating zero
        0x00, 0x21, // Type: NBSTAT (Node Status)
        0x00, 0x01  // Class: IN
    };

    ::sendto(sock, reinterpret_cast<const char*>(nbQuery), sizeof(nbQuery), 0,
             reinterpret_cast<sockaddr*>(&destAddr), sizeof(destAddr));

    struct pollfd pfd{};
    pfd.fd = static_cast<decltype(pfd.fd)>(sock);
    pfd.events = POLLIN;

#ifdef Q_OS_WIN
    int pr = ::WSAPoll(&pfd, 1, timeoutMs);
#else
    int pr = ::poll(&pfd, 1, timeoutMs);
#endif

    std::string resultName;
    if (pr > 0 && (pfd.revents & POLLIN)) {
        unsigned char respBuf[512] = {0};
        sockaddr_in fromAddr{};
        socklen_t fromLen = sizeof(fromAddr);
        int recvd = ::recvfrom(sock, reinterpret_cast<char*>(respBuf), sizeof(respBuf), 0,
                               reinterpret_cast<sockaddr*>(&fromAddr), &fromLen);

        // Parse NetBIOS Node Status response (RFC 1002)
        // Offset 56: Number of Names
        if (recvd >= 57) {
            int numNames = respBuf[56];
            int offset = 57;
            for (int i = 0; i < numNames && (offset + 18) <= recvd; ++i) {
                unsigned char nameType = respBuf[offset + 15];
                unsigned char flags1 = respBuf[offset + 16];
                bool isGroup = (flags1 & 0x80) != 0;

                // Name type 0x00 (Workstation/Computer Name) and NOT a group
                if (nameType == 0x00 && !isGroup && resultName.empty()) {
                    char name[16] = {0};
                    ::memcpy(name, &respBuf[offset], 15);
                    std::string parsed(name);
                    while (!parsed.empty() && (parsed.back() == ' ' || parsed.back() == '\0')) {
                        parsed.pop_back();
                    }
                    if (!parsed.empty()) {
                        resultName = parsed;
                        break;
                    }
                }
                offset += 18;
            }
        }
    }

#ifdef Q_OS_WIN
    ::closesocket(sock);
#else
    ::close(sock);
#endif
    return resultName;
}

// ── Ters DNS & Hostname Çözümleme (Cross-Platform) ──────────────────────────
static std::string queryReverseDns(const char* ip)
{
    // 1. Standart getnameinfo() (DNS, WINS, LLMNR)
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    ::inet_pton(AF_INET, ip, &sa.sin_addr);
    char hostBuf[NI_MAXHOST] = {0};
    if (::getnameinfo(reinterpret_cast<sockaddr*>(&sa), sizeof(sa),
                      hostBuf, sizeof(hostBuf), nullptr, 0, NI_NAMEREQD) == 0) {
        std::string name(hostBuf);
        if (!name.empty() && name != ip) {
            if (name.size() > 6 && name.rfind(".local") == name.size() - 6) {
                name.erase(name.size() - 6);
            }
            return name;
        }
    }

#if defined(__linux__)
    char cmd[256];
    
    // 2. getent hosts (Linux NSS)
    snprintf(cmd, sizeof(cmd), "timeout 1 getent hosts %s 2>/dev/null", ip);
    FILE* pipe = popen(cmd, "r");
    if (pipe) {
        char buf[512];
        if (fgets(buf, sizeof(buf), pipe)) {
            std::string line(buf);
            std::istringstream iss(line);
            std::string resolvedIp, hostname;
            if (iss >> resolvedIp >> hostname) {
                if (hostname != ip) {
                    pclose(pipe);
                    return hostname;
                }
            }
        }
        pclose(pipe);
    }

    // 3. Avahi mDNS resolution
    snprintf(cmd, sizeof(cmd), "timeout 1 avahi-resolve -a %s 2>/dev/null", ip);
    pipe = popen(cmd, "r");
    if (pipe) {
        char buf[256];
        if (fgets(buf, sizeof(buf), pipe)) {
            std::string line(buf);
            size_t tab = line.find('\t');
            if (tab != std::string::npos) {
                std::string avahi_name = line.substr(tab + 1);
                while (!avahi_name.empty() && (avahi_name.back() == '\n' || avahi_name.back() == '\r')) {
                    avahi_name.pop_back();
                }
                pclose(pipe);
                pipe = nullptr;

                if (!avahi_name.empty()) {
                    if (avahi_name.size() > 6 && avahi_name.rfind(".local") == avahi_name.size() - 6) {
                        avahi_name.erase(avahi_name.size() - 6);
                    }
                    return avahi_name;
                }
            }
        }
        if (pipe) pclose(pipe);
    }
#endif

    return "";
}

// ── Ping TTL Sorgusu (ICMP Time-To-Live ile OS Tespiti) ───────────────────────
// Windows varsayılan TTL: 128 (yerel ağda 65-128 arası)
// Linux/macOS varsayılan TTL: 64 (yerel ağda 1-64 arası)
static int queryPingTtl(const char* ip) {
#ifdef Q_OS_WIN
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) return -1;

    unsigned long ipAddr = inet_addr(ip);
    if (ipAddr == INADDR_NONE) {
        IcmpCloseHandle(hIcmp);
        return -1;
    }

    char sendData[32] = "OKIDIA_PING";
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData) + 8;
    std::vector<char> replyBuf(replySize, 0);

    DWORD replies = IcmpSendEcho(
        hIcmp,
        ipAddr,
        sendData,
        sizeof(sendData),
        nullptr,
        replyBuf.data(),
        replySize,
        1000 // 1 saniye timeout
    );

    int ttl = -1;
    if (replies > 0) {
        PICMP_ECHO_REPLY pEchoReply = reinterpret_cast<PICMP_ECHO_REPLY>(replyBuf.data());
        if (pEchoReply && pEchoReply->Status == IP_SUCCESS) {
            ttl = static_cast<int>(pEchoReply->Options.Ttl);
        }
    }

    IcmpCloseHandle(hIcmp);
    return ttl;
#else
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "timeout 1 ping -c 1 -W 1 %s 2>/dev/null", ip);
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return -1;
    char buf[512];
    int ttl = -1;
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string s(buf);
        size_t idx = s.find("ttl=");
        if (idx == std::string::npos) {
            idx = s.find("TTL=");
        }
        if (idx != std::string::npos) {
            int val = 0;
            if (sscanf(s.c_str() + idx + 4, "%d", &val) == 1) {
                ttl = val;
                break;
            }
        }
    }
    pclose(pipe);
    return ttl;
#endif
}

// ── SSH Banner Sorgusu (OS Tespiti için OpenSSH Banner Okuma) ─────────────────
static std::string querySshBanner(const char* ip, int timeoutMs = 800) {
#ifdef Q_OS_WIN
    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return "";

    u_long nonBlock = 1;
    ::ioctlsocket(fd, FIONBIO, &nonBlock);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(22);
    ::inet_pton(AF_INET, ip, &addr.sin_addr);

    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    if (::WSAPoll(&pfd, 1, timeoutMs / 2) <= 0 || (pfd.revents & (POLLERR | POLLHUP))) {
        ::closesocket(fd);
        return "";
    }

    // Connect başarılı oldu, şimdi sunucunun SSH banner göndermesini bekle
    pfd.events = POLLIN;
    if (::WSAPoll(&pfd, 1, timeoutMs) <= 0 || !(pfd.revents & POLLIN)) {
        ::closesocket(fd);
        return "";
    }

    char buf[256] = {0};
    int r = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::closesocket(fd);

    if (r > 0) {
        std::string s(buf, r);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
            s.pop_back();
        }
        return s;
    }
    return "";
#else
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";

    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(22);
    ::inet_pton(AF_INET, ip, &addr.sin_addr);

    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    if (::poll(&pfd, 1, timeoutMs / 2) <= 0 || (pfd.revents & (POLLERR | POLLHUP))) {
        ::close(fd);
        return "";
    }

    pfd.events = POLLIN;
    if (::poll(&pfd, 1, timeoutMs) <= 0 || !(pfd.revents & POLLIN)) {
        ::close(fd);
        return "";
    }

    char buf[256] = {0};
    ssize_t r = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);

    if (r > 0) {
        std::string s(buf, r);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
            s.pop_back();
        }
        return s;
    }
    return "";
#endif
}

// ============================================================================
// Paket Kataloğu
// ============================================================================

AgentPackage agentPackageFor(const QString& osTag, const QString& arch, const QString& label)
{
    // Ortak deb kurulum komutu — %1 = remote tmp yolu
    static const QString DEB_INSTALL =
        "sudo DEBIAN_FRONTEND=noninteractive dpkg -i %1"
        " && sudo systemctl enable --now okidia-agent"
        " && rm -f %1";

    if (osTag == "windows") {
        return { "okidia-agent-win64-setup.exe",
                 "C:\\Windows\\Temp\\okidia-agent-setup.exe",
                 "C:\\Windows\\Temp\\okidia-agent-setup.exe /S /SILENT",
                 "Windows 10/11 (ileride)" };
    }

    // Dosya adını tag ve arch bilgileriyle standartlaştırıyoruz.
    const QString fn = QString("okidia-agent_%1_%2_%3.deb").arg(osTag, Okidia::Version::string(), arch);

    return { fn, "/tmp/okidia-agent.deb",
             DEB_INSTALL.arg("/tmp/okidia-agent.deb"),
             label };
}

// ── Önbellek klasörü ─────────────────────────────────────────────────────────


// ============================================================================
// DeployPanel
// ============================================================================

DeployPanel::DeployPanel(SessionManager* session, QWidget* parent) : QWidget(parent), m_session(session) {
    setupUi();

    if (m_credSelector) {
        m_credSelector->loadSettings("DeployPanel");
        connect(m_credSelector, &SshCredSelectorWidget::configChanged, this, [this]() {
            m_credSelector->saveSettings("DeployPanel");
        });
    }

    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, &DeployPanel::globalContextMenuRequested);

    if (m_session) {
        connect(m_session, &SessionManager::sshCredentialsLoaded, m_credSelector, &SshCredSelectorWidget::refreshCredentials);
        m_session->fetchSshCredentials();
    }
}

DeployPanel::~DeployPanel() {
    if (m_scanner) { m_scanner->stop(); m_scanner->wait(); }
    for (auto* c : m_activeClients) c->deleteLater();
    if (m_credSelector) {
        delete m_credSelector;
        m_credSelector = nullptr;
    }
}

void DeployPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_session) {
        m_session->fetchSshCredentials();
    }
}


NetworkScanner::NetworkScanner(const QStringList& ipRanges, QObject* parent)
    : QThread(parent), m_ranges(ipRanges) {}

// ── Socket tarama yardımcıları ───────────────────────────────────────────────

namespace {

using Clock    = std::chrono::steady_clock;
using Ms       = std::chrono::milliseconds;
using TimePoint= std::chrono::time_point<Clock>;

/** Non-blocking TCP socket oluştur ve connect başlat. */
static int startConnect(const char* ip, int port) {
#ifdef Q_OS_WIN
    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return -1;
    u_long mode = 1;
    ::ioctlsocket(fd, FIONBIO, &mode);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, ip, &addr.sin_addr);
    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return static_cast<int>(fd);
#else
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, ip, &addr.sin_addr);
    ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return fd;
#endif
}

} // namespace

// ── Phase 1: TCP port taraması (poll / WSAPoll) ──────────────────────────────
struct DiscoveredHost {
    std::string ip;
    bool hasSsh = false;   // 22 (SSH)
    bool hasRpc = false;   // 135 (MSRPC)
    bool hasSmb = false;   // 445 (SMB)
    bool hasRdp = false;   // 3389 (RDP)
    bool hasWinRm = false; // 5985 (WinRM)
    bool hasWsd = false;   // 5357 (WSDAPI HTTP)
    std::string sshBanner;
};

static std::unordered_map<std::string, DiscoveredHost> tcpScan(
    const std::vector<std::string>& allIps,
    const std::atomic<bool>& stopFlag,
    int totalForProgress,   // progress hesabı için
    const std::function<void(int)>& progressCb,
    const std::function<void(const std::string&)>& foundCb)
{
    const int PORTS[]       = {22, 135, 445, 3389, 5357, 5985};
    const int BATCH_IPS     = 100;  // 100 × 6 = 600 socket per batch
    const int CONNECT_MS    = 1500; // bağlantı timeout

    std::unordered_map<std::string, DiscoveredHost> aliveMap;
    const int total = (int)allIps.size();

    for (int bStart = 0; bStart < total && !stopFlag.load(); bStart += BATCH_IPS) {
        const int bEnd = std::min(bStart + BATCH_IPS, total);
        scanLog("  Batch %d-%d başladı", bStart, bEnd - 1);

        struct FdMeta {
            std::string ip;
            int port;
        };
        std::vector<pollfd> pollFds;
        std::vector<FdMeta> fdMetas;

        for (int i = bStart; i < bEnd; ++i) {
            for (int port : PORTS) {
                int fd = startConnect(allIps[i].c_str(), port);
                if (fd < 0) continue;
                pollfd pfd{};
                pfd.fd = static_cast<decltype(pfd.fd)>(fd);
                pfd.events = POLLOUT | POLLIN;
                pollFds.push_back(pfd);
                fdMetas.push_back({allIps[i], port});
            }
        }

        TimePoint deadline = Clock::now() + Ms(CONNECT_MS);

        while (!pollFds.empty() && !stopFlag.load()) {
            int ms = (int)std::chrono::duration_cast<Ms>(deadline - Clock::now()).count();
            if (ms <= 0) break;
#ifdef Q_OS_WIN
            int n = ::WSAPoll(pollFds.data(), static_cast<ULONG>(pollFds.size()), ms);
#else
            int n = ::poll(pollFds.data(), pollFds.size(), ms);
#endif
            if (n <= 0) break;

            for (size_t i = 0; i < pollFds.size(); ) {
                if (pollFds[i].revents != 0) {
                    auto rawFd = pollFds[i].fd;
                    const auto meta = fdMetas[i];

                    int err = 0; socklen_t el = sizeof(err);
                    ::getsockopt(rawFd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);

                    bool connected = (err == 0 && !(pollFds[i].revents & (POLLERR | POLLHUP)));

                    if (connected) {
                        bool isNew = (aliveMap.find(meta.ip) == aliveMap.end());
                        auto& host = aliveMap[meta.ip];
                        host.ip = meta.ip;

                        if (isNew) {
                            scanLog("  hostFound: %s → emit", meta.ip.c_str());
                            foundCb(meta.ip);
                            scanLog("  hostFound: %s emit OK", meta.ip.c_str());
                        }

                        if (err == 0) {
                            if (meta.port == 22) {
                                host.hasSsh = true;
                                char banner[128] = {0};
#ifdef Q_OS_WIN
                                u_long nonBlock = 1;
                                ::ioctlsocket(rawFd, FIONBIO, &nonBlock);
                                int r = ::recv(rawFd, banner, sizeof(banner) - 1, 0);
#else
                                ssize_t r = ::recv(rawFd, banner, sizeof(banner) - 1, MSG_DONTWAIT);
#endif
                                if (r > 0) {
                                    host.sshBanner = std::string(banner, r);
                                }
                            } else if (meta.port == 135) {
                                host.hasRpc = true;
                            } else if (meta.port == 445) {
                                host.hasSmb = true;
                            } else if (meta.port == 3389) {
                                host.hasRdp = true;
                            } else if (meta.port == 5357) {
                                host.hasWsd = true;
                            } else if (meta.port == 5985) {
                                host.hasWinRm = true;
                            }
                        }
                    }

#ifdef Q_OS_WIN
                    ::closesocket(rawFd);
#else
                    ::close(rawFd);
#endif
                    pollFds.erase(pollFds.begin() + i);
                    fdMetas.erase(fdMetas.begin() + i);
                } else {
                    ++i;
                }
            }
        }

        // Kalan fd'leri kapat
        for (const auto& pfd : pollFds) {
#ifdef Q_OS_WIN
            ::closesocket(pfd.fd);
#else
            ::close(pfd.fd);
#endif
        }

        const int pct = (bEnd * 55) / totalForProgress;
        scanLog("  Batch %d-%d bitti, progress=%d, alive=%zu",
                bStart, bEnd - 1, pct, aliveMap.size());
        progressCb(pct);
    }

#ifdef Q_OS_WIN
    // --- Aşama 1.5: Windows Kernel ARP Tablosu (GetIpNetTable) — 0 ms Gecikme ---
    scanLog("  Windows ARP tablosu kontrol ediliyor...");
    ULONG tableSize = 0;
    if (::GetIpNetTable(nullptr, &tableSize, FALSE) == ERROR_INSUFFICIENT_BUFFER && tableSize > 0) {
        std::vector<BYTE> buffer(tableSize);
        PMIB_IPNETTABLE pTable = reinterpret_cast<PMIB_IPNETTABLE>(buffer.data());
        if (::GetIpNetTable(pTable, &tableSize, FALSE) == NO_ERROR) {
            for (DWORD idx = 0; idx < pTable->dwNumEntries; ++idx) {
                const auto& row = pTable->table[idx];
                // dwType: 3 = Dynamic (Aktif canlı cihaz), 4 = Static
                if ((row.dwType == 3 || row.dwType == 4) && row.dwPhysAddrLen == 6) {
                    bool validMac = false;
                    for (DWORD b = 0; b < row.dwPhysAddrLen; ++b) {
                        if (row.bPhysAddr[b] != 0) { validMac = true; break; }
                    }
                    if (validMac) {
                        in_addr addr;
                        addr.s_addr = row.dwAddr;
                        char* ipStr = ::inet_ntoa(addr);
                        if (ipStr) {
                            std::string foundIp(ipStr);
                            if (aliveMap.find(foundIp) == aliveMap.end() && std::find(allIps.begin(), allIps.end(), foundIp) != allIps.end()) {
                                aliveMap[foundIp].ip = foundIp;
                                scanLog("  hostFound via ARP Cache: %s → emit", foundIp.c_str());
                                foundCb(foundIp);
                            }
                        }
                    }
                }
            }
        }
    }
#else
    // --- Aşama 1.5: İşletim Sisteminin ARP Tablosunu Kontrol Et ---
    scanLog("  ARP tablosu kontrol ediliyor...");
    std::ifstream arpFile("/proc/net/arp");
    std::string line;
    if (std::getline(arpFile, line)) {
        while (std::getline(arpFile, line)) {
            std::istringstream iss(line);
            std::string ip, hw, flags, mac, mask, dev;
            if (iss >> ip >> hw >> flags >> mac >> mask >> dev) {
                if (flags == "0x2" && mac != "00:00:00:00:00:00" && aliveMap.find(ip) == aliveMap.end()) {
                    if (std::find(allIps.begin(), allIps.end(), ip) != allIps.end()) {
                        aliveMap[ip].ip = ip;
                        scanLog("  hostFound via ARP: %s (MAC=%s) → emit", ip.c_str(), mac.c_str());
                        foundCb(ip);
                        scanLog("  hostFound via ARP: %s emit OK", ip.c_str());
                    }
                }
            }
        }
    }
#endif

    return aliveMap;
}

// ── NetworkScanner::run() ────────────────────────────────────────────────────

void NetworkScanner::run()
{
    QString debugLogPath = QDir::temp().filePath("okidia_scan_debug.log");
    g_scanLog = ::fopen(debugLogPath.toLocal8Bit().constData(), "w");
#ifdef Q_OS_UNIX
    ::signal(SIGSEGV, sigsegvHandler);
#endif
    scanLog("=== NetworkScanner::run() başladı (tek thread) ===");

    // IP listesi
    std::vector<std::string> allIps;
    for (const QString& range : m_ranges)
        for (const QString& ip : expandRange(range))
            allIps.push_back(ip.toStdString());
    std::sort(allIps.begin(), allIps.end());
    allIps.erase(std::unique(allIps.begin(), allIps.end()), allIps.end());

    // ── Kendi IP'lerini hariç tut ─────────────────────────────────
    std::unordered_set<std::string> localIps;
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        for (const auto& entry : iface.addressEntries()) {
            const auto addr = entry.ip();
            if (addr.protocol() == QAbstractSocket::IPv4Protocol)
                localIps.insert(addr.toString().toStdString());
        }
    }
    {
        std::vector<std::string> filtered;
        filtered.reserve(allIps.size());
        for (const auto& ip : allIps) {
            if (localIps.count(ip)) {
                scanLog("Kendi IP'si atlandı: %s", ip.c_str());
            } else {
                filtered.push_back(ip);
            }
        }
        allIps = std::move(filtered);
    }

    const int total = (int)allIps.size();
    scanLog("Toplam IP: %d", total);
    if (total == 0) { emit finished(); goto cleanup; }

    emit progress(0, 100);

    {
    // ── Aşama 1: TCP tarama ────────────────────────────────
    scanLog("--- Aşama 1 başladı (port 22/135/445/3389) ---");

    auto aliveMap = tcpScan(
        allIps, m_stop, total,
        [this](int pct) { emit progress(pct, 100); },
        [this](const std::string& ip) {
            emit hostFound(QString::fromStdString(ip));
        });

    scanLog("--- Aşama 1 bitti, alive=%zu ---", aliveMap.size());

    if (m_stop.load()) { emit finished(); goto cleanup; }

    // ── Aşama 2: DNS, NetBIOS & OS İmza Çözümlemesi (Paralel) ──
    scanLog("--- Aşama 2 başladı (DNS, NetBIOS & OS çözümleme, alive=%zu) ---", aliveMap.size());

    std::vector<DiscoveredHost> aliveHosts;
    for (const auto& [ip, h] : aliveMap) {
        aliveHosts.push_back(h);
    }

    const int aliveTotal = (int)aliveHosts.size();
    if (aliveTotal > 0) {
        QThreadPool pool;
        pool.setMaxThreadCount(4);
        std::atomic<int> completed{0};

        for (int i = 0; i < aliveTotal; ++i) {
            const auto host = aliveHosts[i];
            std::ignore = QtConcurrent::run(&pool, [this, host, aliveTotal, &completed]() {
                if (m_stop.load()) return;
                
                const char* c_ip = host.ip.c_str();
                scanLog("  [%s] çözümleniyor worker thread içinde", c_ip);

                // 1. NetBIOS Sorgusu (Windows & Samba makineler için anında tam bilgisayar adı)
                std::string netbios = queryNetbiosName(c_ip, 250);
                std::string hostname = netbios;

                // 2. DNS / Ters DNS Çözümleme
                if (hostname.empty()) {
                    hostname = queryReverseDns(c_ip);
                }

                // 3. Ping TTL Kontrolü (Hızlı ve Paralel)
                int pingTtl = queryPingTtl(c_ip);

                // 3.5. SSH Banner Tespiti (Port 22 açıksa güvenilir şekilde oku)
                std::string sshBanner = host.sshBanner;
                if (host.hasSsh && sshBanner.empty()) {
                    sshBanner = querySshBanner(c_ip, 800);
                }

                // 4. İşletim Sistemi İmzası ve Cihaz Sınıflandırması
                std::string osHint;
                const bool hasWindowsPorts = !netbios.empty() || host.hasRpc || host.hasSmb || host.hasRdp || host.hasWinRm || host.hasWsd;
                const bool isWinBanner = (sshBanner.find("Windows") != std::string::npos ||
                                          sshBanner.find("Microsoft") != std::string::npos ||
                                          sshBanner.find("OpenSSH_for_Windows") != std::string::npos);
                const bool hasValidHostname = (!hostname.empty() && hostname != "Bilinmeyen" && hostname != "Gizli Cihaz" &&
                                               hostname != "Bilinmeyen Cihaz" && hostname != tr("Bilinmeyen Cihaz").toStdString());

                if (host.hasSsh) {
                    // ── Sınıf 1: SSH Açık (Hazır İstemci) ──
                    if (hasWindowsPorts || isWinBanner || (pingTtl > 64 && pingTtl <= 128)) {
                        osHint = "Windows";
                    } else if (sshBanner.find("Ubuntu") != std::string::npos) {
                        osHint = "Ubuntu";
                    } else if (sshBanner.find("Debian") != std::string::npos) {
                        osHint = "Debian";
                    } else if (sshBanner.find("Arch") != std::string::npos ||
                               sshBanner.find("CentOS") != std::string::npos ||
                               sshBanner.find("Fedora") != std::string::npos ||
                               sshBanner.find("RedHat") != std::string::npos ||
                               sshBanner.find("Pardus") != std::string::npos ||
                               sshBanner.find("Linux") != std::string::npos) {
                        osHint = "Linux";
                    } else if (sshBanner.find("Apple") != std::string::npos ||
                               sshBanner.find("Darwin") != std::string::npos) {
                        osHint = "macOS";
                    } else {
                        // SSH açık ama özel banner yoksa TTL'e göre genel tahmin
                        osHint = (pingTtl > 64 && pingTtl <= 128) ? "Windows" : "Linux";
                    }
                } else if (hasWindowsPorts) {
                    // ── Sınıf 2A: Windows PC ama SSH Servisi Kapalı / Yüklü Değil ──
                    osHint = "Windows (SSH Kapalı)";
                } else if (hasValidHostname) {
                    // ── Sınıf 2B: SSH kapalı ama rDNS / Avahi'den tescilli bilgisayar adı gelmiş (Muhtemel PC) ──
                    osHint = (pingTtl > 64 && pingTtl <= 128) ? "Windows (SSH Kapalı)" : "Linux (SSH Kapalı)";
                } else {
                    // ── Sınıf 3: Ne SSH var, ne Windows portları, ne de tescilli makine adı (Akıllı fırın, telefon, TV, IoT vb.) ──
                    // Sırf TTL<=64 diye asla 'Linux PC' denmez!
                    osHint = tr("Akıllı Cihaz / IoT").toStdString();
                }

                // 5. Varsayılan İsim Ataması
                if (hostname.empty() || hostname == "Bilinmeyen" || hostname == "Gizli Cihaz" || hostname == "Bilinmeyen Cihaz" ||
                    hostname == tr("Bilinmeyen Cihaz").toStdString() || hostname == tr("Gizli Cihaz").toStdString() || hostname == tr("Bilinmeyen").toStdString()) {
                    if (osHint == "Windows") {
                        hostname = "Windows PC";
                    } else if (osHint == "Ubuntu") {
                        hostname = "Ubuntu PC";
                    } else if (osHint == "Debian") {
                        hostname = "Debian PC";
                    } else if (osHint == "Linux") {
                        hostname = "Linux PC";
                    } else if (osHint == "macOS") {
                        hostname = "Mac";
                    } else if (osHint == "Windows (SSH Kapalı)") {
                        hostname = "Windows PC (SSH Kapalı)";
                    } else if (osHint == "Linux (SSH Kapalı)") {
                        hostname = "Linux PC (SSH Kapalı)";
                    } else {
                        hostname = tr("Ağ / Akıllı Cihaz").toStdString();
                    }
                }

                const QString qip   = QString::fromStdString(host.ip);
                const QString qhost = QString::fromStdString(hostname);
                const QString qos   = QString::fromStdString(osHint);

                emit hostUpdated(qip, qhost, qos);

                int c = ++completed;
                emit progress(55 + (c * 45) / aliveTotal, 100);
            });
        }

        pool.waitForDone();
    }

    scanLog("=== Tarama tamamlandı ===");
    emit progress(100, 100);
    emit finished();
    } // end scope

cleanup:
    if (g_scanLog) { ::fclose(g_scanLog); g_scanLog = nullptr; }
}

QStringList NetworkScanner::expandRange(const QString& range) const {
    QStringList result;

    // CIDR: 192.168.1.0/24
    if (range.contains('/')) {
        QStringList parts = range.split('/');
        if (parts.size() != 2) return result;
        int prefix = parts[1].toInt();
        QStringList octets = parts[0].split('.');
        if (octets.size() != 4) return result;

        if (prefix == 32) {
            result << parts[0];
            return result;
        }

        quint32 base = 0;
        for (int i = 0; i < 4; ++i)
            base = (base << 8) | octets[i].toUInt();

        quint32 mask  = prefix ? (~0u << (32 - prefix)) : 0;
        quint32 start = (prefix == 31) ? (base & mask) : ((base & mask) + 1);
        quint32 end   = (prefix == 31) ? (base | ~mask) : ((base | ~mask) - 1);

        for (quint32 ip = start; ip <= end && !m_stop; ++ip) {
            result << QString("%1.%2.%3.%4")
                .arg((ip>>24)&0xFF).arg((ip>>16)&0xFF)
                .arg((ip>>8)&0xFF).arg(ip&0xFF);
        }
        return result;
    }

    // Aralık: 192.168.1.1-254
    QRegularExpression re(
        R"((\d+)\.(\d+)\.(\d+)\.(\d+)-(\d+))");
    auto m = re.match(range);
    if (m.hasMatch()) {
        int a=m.captured(1).toInt(), b=m.captured(2).toInt(),
            c=m.captured(3).toInt();
        int from=m.captured(4).toInt(), to=m.captured(5).toInt();
        for (int i = from; i <= to && !m_stop; ++i)
            result << QString("%1.%2.%3.%4").arg(a).arg(b).arg(c).arg(i);
        return result;
    }

    // Tek IP
    result << range.trimmed();
    return result;
}

void DeployPanel::updateDiscoveryCount(int count) {
    if (m_actNewClients) {
        m_actNewClients->setEnabled(count > 0);
        m_actNewClients->setText(count > 0
            ? QString(tr("Yeni İstemci İstekleri  (%1)")).arg(count)
            : tr("Yeni İstemci İstekleri"));
    }

    if (m_btnClients) {
        m_btnClients->setProperty("hasRequests", count > 0);
        m_btnClients->style()->unpolish(m_btnClients);
        m_btnClients->style()->polish(m_btnClients);
        m_btnClients->setText(count > 0
            ? QString(tr("İstemciler  (%1)")).arg(count)
            : tr("İstemciler"));
    }
}


