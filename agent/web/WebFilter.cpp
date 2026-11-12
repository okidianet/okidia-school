// agent/web/WebFilter.cpp
#include "WebFilter.h"
#include "../../common/utils/Logger.h"

#include <QProcess>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QTextStream>
#include <QRegularExpression>
#include <QUrl>
#include <QDesktopServices>

#include <QTimer>

#if !defined(Q_OS_WIN)
#  include <unistd.h>
#endif

#if defined(Q_OS_WIN)
static bool runWinCmd(const QString& prog, const QStringList& args) {
    QProcess p;
    p.start(prog, args);
    if (!p.waitForStarted(3000)) return false;
    p.waitForFinished(10000);
    return p.exitCode() == 0;
}
#endif

WebFilter::WebFilter(QObject* parent)
    : QObject(parent) {
    m_verifyTimer = new QTimer(this);
    m_verifyTimer->setInterval(20000); // 20 saniyede bir teyit et
    connect(m_verifyTimer, &QTimer::timeout, this, &WebFilter::verifyRules);
}

// ---------------------------------------------------------------------------
void WebFilter::handleEnvelope(const okidia::Envelope& envelope) {
    switch (envelope.type()) {

    case okidia::MessageType::WEB_BLOCK_ALL: {
        okidia::WebFilterCommand cmd;
        QString reason;
        if (cmd.ParseFromString(envelope.payload()))
            reason = QString::fromStdString(cmd.reason());
        blockAll(reason);
        break;
    }

    case okidia::MessageType::WEB_BLOCK_DOMAINS: {
        okidia::WebFilterCommand cmd;
        QString reason;
        QStringList domains;
        if (cmd.ParseFromString(envelope.payload())) {
            reason = QString::fromStdString(cmd.reason());
            for (const auto& d : cmd.domains()) {
                domains.append(QString::fromStdString(d).trimmed());
            }
        }
        blockDomains(domains, reason);
        break;
    }

    case okidia::MessageType::WEB_UNBLOCK:
        unblock();
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
void WebFilter::applyFirewallRules() {
#if defined(Q_OS_WIN)
    ensureFirewallServiceRunning();

    // 1. Windows Firewall ile tüm giden (outbound) 80 ve 443 TCP portlarını engelle
    runWinCmd("netsh", {"advfirewall", "firewall", "delete", "rule", "name=OKIDIA_WebBlock"});
    runWinCmd("netsh", {"advfirewall", "firewall", "add", "rule", "name=OKIDIA_WebBlock",
                        "dir=out", "action=block", "protocol=TCP", "remoteport=80,443"});

    // 2. WinHTTP ve Sistem Registry Proxy'sini loopback'e yönlendir
    runWinCmd("netsh", {"winhttp", "set", "proxy", "127.0.0.1:1"});
    runWinCmd("reg", {"add", "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                      "/v", "ProxyEnable", "/t", "REG_DWORD", "/d", "1", "/f"});
    runWinCmd("reg", {"add", "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                      "/v", "ProxyServer", "/t", "REG_SZ", "/d", "127.0.0.1:1", "/f"});
#else
    setGnomeProxy("127.0.0.1", 1);
    if (::geteuid() == 0) {
        // Varsa eski kuralı temizle
        QProcess::execute("iptables", {"-D", "OUTPUT", "-p", "tcp", "-m", "multiport", "--dports", "80,443", "-j", "REJECT", "-m", "comment", "--comment", "OKIDIA_WEB_BLOCK"});
        // En başa kuralı ekle
        QProcess::execute("iptables", {"-I", "OUTPUT", "1", "-p", "tcp", "-m", "multiport", "--dports", "80,443", "-j", "REJECT", "-m", "comment", "--comment", "OKIDIA_WEB_BLOCK"});
    }
#endif
}

void WebFilter::removeFirewallRules() {
#if defined(Q_OS_WIN)
    runWinCmd("netsh", {"advfirewall", "firewall", "delete", "rule", "name=OKIDIA_WebBlock"});
    runWinCmd("netsh", {"winhttp", "reset", "proxy"});
    runWinCmd("reg", {"add", "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                      "/v", "ProxyEnable", "/t", "REG_DWORD", "/d", "0", "/f"});
    runWinCmd("reg", {"delete", "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                      "/v", "ProxyServer", "/f"});
    runWinCmd("reg", {"delete", "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                      "/v", "AutoConfigURL", "/f"});
#else
    clearGnomeProxy();
    if (::geteuid() == 0) {
        QProcess::execute("iptables", {"-D", "OUTPUT", "-p", "tcp", "-m", "multiport", "--dports", "80,443", "-j", "REJECT", "-m", "comment", "--comment", "OKIDIA_WEB_BLOCK"});
    }
#endif
}

bool WebFilter::isFirewallRuleActive() {
#if defined(Q_OS_WIN)
    QProcess p;
    p.start("netsh", {"advfirewall", "firewall", "show", "rule", "name=OKIDIA_WebBlock"});
    if (!p.waitForStarted(2000)) return false;
    p.waitForFinished(4000);
    return (p.exitCode() == 0 && p.readAllStandardOutput().contains("OKIDIA_WebBlock"));
#else
    if (::geteuid() != 0) return true; // root değilsek iptables denetlenemez
    int exitCode = QProcess::execute("iptables", {"-C", "OUTPUT", "-p", "tcp", "-m", "multiport", "--dports", "80,443", "-j", "REJECT", "-m", "comment", "--comment", "OKIDIA_WEB_BLOCK"});
    return (exitCode == 0);
#endif
}

bool WebFilter::isProxyActive() {
#if defined(Q_OS_WIN)
    QProcess p;
    p.start("reg", {"query", "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", "/v", "ProxyEnable"});
    if (!p.waitForStarted(2000)) return false;
    p.waitForFinished(4000);
    QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    return out.contains("0x1");
#else
    // Linux gsettings kontrolü
    return true;
#endif
}

#if defined(Q_OS_WIN)
void WebFilter::ensureFirewallServiceRunning() {
    QProcess p;
    p.start("sc", {"query", "MpsSvc"});
    if (p.waitForStarted(2000)) {
        p.waitForFinished(4000);
        QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
        if (!out.contains("RUNNING")) {
            Logger::warning(QObject::tr("WebFilter: Windows Güvenlik Duvarı (MpsSvc) çalışmıyor, başlatılıyor..."));
            runWinCmd("sc", {"config", "MpsSvc", "start=", "auto"});
            runWinCmd("net", {"start", "MpsSvc"});
        }
    }
    // Tüm profiller için firewall'u aç
    runWinCmd("netsh", {"advfirewall", "set", "allprofiles", "state", "on"});
}
#endif

void WebFilter::verifyRules() {
    if (!m_active) return;

    bool fwOk = isFirewallRuleActive();
    bool proxyOk = isProxyActive();

    if (!fwOk || !proxyOk) {
        Logger::warning(QObject::tr("WebFilter: Güvenlik duvarı veya proxy kuralı bozulmuş/kaldırılmış tespit edildi! Yeniden uygulanıyor. (FW: %1, Proxy: %2)")
                        .arg(fwOk ? QObject::tr("Aktif") : QObject::tr("Kayıp"))
                        .arg(proxyOk ? QObject::tr("Aktif") : QObject::tr("Kayıp")));
        applyFirewallRules();
    }
}

// ---------------------------------------------------------------------------
// Tüm interneti engelle
void WebFilter::blockAll(const QString& reason) {
    m_lastReason = reason;
    Logger::info(QObject::tr("WebFilter: Tüm internet engelleniyor. Sebep: ") + reason);
    applyFirewallRules();
    m_active = true;
    if (m_verifyTimer && !m_verifyTimer->isActive()) {
        m_verifyTimer->start();
    }
    emit filterChanged(true);
}

void WebFilter::blockDomains(const QStringList& domains, const QString& reason) {
    m_lastReason = reason;
    m_blockedDomains = domains;
    Logger::info(QObject::tr("WebFilter: Belirli alan adları engelleniyor (%1 alan adı). Sebep: %2")
                 .arg(domains.size()).arg(reason));

    applyDomainBlockRules();
    m_active = !m_blockedDomains.isEmpty();
    showBlockedPage();
    emit filterChanged(m_active);
}

void WebFilter::showBlockedPage() {
    QString targetPath = QDir::tempPath() + "/okidia_blocked.html";
    if (!QFile::exists(targetPath)) {
        QFile resFile(":/html/blocked.html");
        if (resFile.open(QIODevice::ReadOnly)) {
            QFile out(targetPath);
            if (out.open(QIODevice::WriteOnly)) {
                out.write(resFile.readAll());
                out.close();
            }
            resFile.close();
        }
    }

#if defined(Q_OS_WIN)
    runWinCmd("cmd.exe", {"/c", "start", targetPath});
#else
    QProcess::startDetached("xdg-open", {targetPath});
#endif
}

void WebFilter::applyDomainBlockRules() {
#if defined(Q_OS_WIN)
    QString hostsPath = "C:\\Windows\\System32\\drivers\\etc\\hosts";
#else
    QString hostsPath = "/etc/hosts";
#endif

    QFile file(hostsPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        Logger::warning(QObject::tr("WebFilter: Hosts dosyası açılamadı → ") + hostsPath);
        return;
    }

    QString content = QString::fromUtf8(file.readAll());
    file.close();

    // Varsa eski Okidia bloğunu temizle
    QRegularExpression rx("\n?# --- OKIDIA_WEB_FILTER_START ---.*?# --- OKIDIA_WEB_FILTER_END ---\n?",
                          QRegularExpression::DotMatchesEverythingOption);
    content.remove(rx);

    if (!m_blockedDomains.isEmpty()) {
        QString blockText = "\n# --- OKIDIA_WEB_FILTER_START ---\n";
        for (const QString& d : m_blockedDomains) {
            QString clean = d.trimmed().toLower();
            if (clean.isEmpty()) continue;
            if (clean.startsWith("http://")) clean.remove(0, 7);
            if (clean.startsWith("https://")) clean.remove(0, 8);
            if (clean.contains("/")) clean = clean.section('/', 0, 0);

            blockText += QString("127.0.0.1 %1\n").arg(clean);
            if (!clean.startsWith("www.")) {
                blockText += QString("127.0.0.1 www.%1\n").arg(clean);
            }
        }
        blockText += "# --- OKIDIA_WEB_FILTER_END ---\n";
        content += blockText;
    }

    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        file.write(content.toUtf8());
        file.close();
        Logger::info(QObject::tr("WebFilter: Hosts kuralları güncellendi"));
    }

    // DNS önbelleğini temizle
#if defined(Q_OS_WIN)
    runWinCmd("ipconfig", {"/flushdns"});
#else
    QProcess::execute("resolvectl", {"flush-caches"});
    QProcess::execute("systemd-resolve", {"--flush-caches"});
#endif
}

void WebFilter::removeDomainBlockRules() {
#if defined(Q_OS_WIN)
    QString hostsPath = "C:\\Windows\\System32\\drivers\\etc\\hosts";
#else
    QString hostsPath = "/etc/hosts";
#endif

    QFile file(hostsPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;

    QString content = QString::fromUtf8(file.readAll());
    file.close();

    QRegularExpression rx("\n?# --- OKIDIA_WEB_FILTER_START ---.*?# --- OKIDIA_WEB_FILTER_END ---\n?",
                          QRegularExpression::DotMatchesEverythingOption);
    if (content.contains(rx)) {
        content.remove(rx);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            file.write(content.toUtf8());
            file.close();
            Logger::info(QObject::tr("WebFilter: Hosts kuralları temizlendi"));
        }
    }

#if defined(Q_OS_WIN)
    runWinCmd("ipconfig", {"/flushdns"});
#else
    QProcess::execute("resolvectl", {"flush-caches"});
    QProcess::execute("systemd-resolve", {"--flush-caches"});
#endif
}

void WebFilter::unblock() {
    Logger::info(QObject::tr("WebFilter: Internet engeli kaldırılıyor"));
    if (m_verifyTimer && m_verifyTimer->isActive()) {
        m_verifyTimer->stop();
    }
    removeFirewallRules();
    removeDomainBlockRules();
    m_blockedDomains.clear();
    m_active = false;
    emit filterChanged(false);
}

#if !defined(Q_OS_WIN)
// GNOME proxy ayarları (Pardus = GNOME tabanlı)
void WebFilter::setGnomeProxy(const QString& host, int port) {
    runGsettingsBatch({
        {"set", "org.gnome.system.proxy", "mode", "manual"},
        {"set", "org.gnome.system.proxy.http",  "host", host},
        {"set", "org.gnome.system.proxy.http",  "port", QString::number(port)},
        {"set", "org.gnome.system.proxy.https", "host", host},
        {"set", "org.gnome.system.proxy.https", "port", QString::number(port)},
        {"set", "org.gnome.system.proxy.ftp",   "host", host},
        {"set", "org.gnome.system.proxy.ftp",   "port", QString::number(port)},
    });
}

void WebFilter::clearGnomeProxy() {
    runGsettingsBatch({
        {"set", "org.gnome.system.proxy", "mode", "none"},
    });
}

void WebFilter::runGsettings(const QStringList& args) {
    runGsettingsBatch({args});
}

void WebFilter::runGsettingsBatch(
        const QList<QStringList>& commands) {
    // Oturum açmış kullanıcının UID'sini bul
    QProcess who;
    who.start("sh", {"-c", "id -u $(who | awk '/:0/{print $1;exit}')"});
    who.waitForFinished(2000);
    QString uid = who.readAllStandardOutput().trimmed();
    if (uid.isEmpty()) uid = "1000";

    // Tek bir bash script olarak çalıştır
    QStringList lines;
    lines << "#!/bin/bash";
    lines << "export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/" + uid + "/bus";
    lines << "export XDG_RUNTIME_DIR=/run/user/" + uid;
    for (const auto& args : commands) {
        QString cmd = "gsettings";
        for (const auto& a : args)
            cmd += " '" + QString(a).replace("'", "'\\''") + "'";
        lines << cmd;
    }

    // Script dosyasına yaz
    QString scriptPath = "/tmp/okidia_gsettings.sh";
    QFile f(scriptPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    f.write(lines.join("\n").toUtf8());
    f.close();
    QFile::setPermissions(scriptPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
        QFileDevice::ExeOwner | QFileDevice::ReadGroup |
        QFileDevice::ExeGroup);

    // Kullanıcı olarak tek seferde çalıştır
    QProcess proc;
    proc.start("sudo", {"-u", "#" + uid, "bash", scriptPath});
    proc.waitForFinished(5000);

    QFile::remove(scriptPath);
    Logger::info(QObject::tr("WebFilter: gsettings batch (") +
                 QString::number(commands.size()) +
                 QObject::tr(" komut) → exit:") +
                 QString::number(proc.exitCode()));
}
#endif
