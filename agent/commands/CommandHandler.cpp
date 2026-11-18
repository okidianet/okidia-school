// agent/commands/CommandHandler.cpp
#include "CommandHandler.h"
#include "../../common/utils/Logger.h"
#include <QProcess>
CommandHandler::CommandHandler(QObject* parent)
    : QObject(parent) {}

// ---------------------------------------------------------------------------
// Ana dispatch
// ---------------------------------------------------------------------------
void CommandHandler::handleEnvelope(const okidia::Envelope& envelope) {
    // Komut filtreleme
    if (m_agentType > 0) {
        bool allowed = true;
        uint32_t effectiveType = (m_agentType >= 100) ? 1 : m_agentType;
        if (effectiveType == 1) {
            // Tip 1 (Öğrenci PC ve Maskeli Özel Tipler): Öğrenci bilgisayarlarında tüm izleme ve kontrol
            // komutları serbesttir (öğretmen denetimi için).
            allowed = true;
        } else if (effectiveType == 2) {
            // Tip 2 (İdari PC): GİZLİLİK ön planda.
            // Bazı komutlar doğrudan engellenir, bazıları (izleme/kontrol) Tray'de onaya tabidir.
            if (envelope.type() == okidia::MessageType::SCREEN_CAPTURE ||
                envelope.type() == okidia::MessageType::APP_LIST_REQUEST ||
                envelope.type() == okidia::MessageType::SCREEN_LOCK ||
                envelope.type() == okidia::MessageType::SCREEN_UNLOCK ||
                envelope.type() == okidia::MessageType::INPUT_LOCK ||
                envelope.type() == okidia::MessageType::INPUT_UNLOCK ||
                envelope.type() == okidia::MessageType::AUDIO_PLAY_LOCAL ||
                envelope.type() == okidia::MessageType::AUDIO_BROADCAST ||
                envelope.type() == okidia::MessageType::AUDIO_BROADCAST_LIVE ||
                envelope.type() == okidia::MessageType::AUDIO_BROADCAST_FILE ||
                envelope.type() == okidia::MessageType::AUDIO_BROADCAST_STOP ||
                envelope.type() == okidia::MessageType::AUDIO_STOP ||
                envelope.type() == okidia::MessageType::AUDIO_MUTE ||
                envelope.type() == okidia::MessageType::AUDIO_UNMUTE ||
                envelope.type() == okidia::MessageType::AUDIO_UNMUTE_ONLY ||
                envelope.type() == okidia::MessageType::AUDIO_SET_VOLUME) {
                allowed = false;
            }
        } else if (effectiveType == 3) {
            // Tip 3 (Akıllı Tahta): Herkese açık ekran.
            // Giriş kilitleme gibi etkileşimi tamamen kesen komutlar kısıtlanabilir.
            if (envelope.type() == okidia::MessageType::INPUT_LOCK) {
                allowed = false;
            }
        }

        if (!allowed) {
            Logger::warning(QObject::tr("CommandHandler: Komut kullanıcı seviyesinde engellendi (Tip ") + 
                            QString::number(m_agentType) + QObject::tr(") → tip:") + 
                            QString::number(envelope.type()));
            return;
        }
    }

    Logger::info(QObject::tr("CommandHandler: Komut alındı → tip:") +
                 QString::number(envelope.type()));

    switch (envelope.type()) {

    // --- Ekran ---
    case okidia::MessageType::SCREEN_LOCK: {
        okidia::ScreenLockCommand cmd;
        cmd.ParseFromString(envelope.payload());
        screenLock(cmd);
        break;
    }
    case okidia::MessageType::SCREEN_UNLOCK:
        screenUnlock();
        break;

    case okidia::MessageType::SCREEN_CAPTURE:
        screenCapture();
        break;

    case okidia::MessageType::SCREEN_SELECT: {
        okidia::ScreenSelectCommand cmd;
        cmd.ParseFromString(envelope.payload());
        screenSelect(cmd);
        break;
    }
    case okidia::MessageType::SCREEN_INFO_REQUEST:
        screenInfoRequest();
        break;

    case okidia::MessageType::SCREEN_FPS_SET: {
        okidia::ScreenConfig cmd;
        if (cmd.ParseFromString(envelope.payload())) {
            handleScreenConfig(cmd);
        }
        break;
    }

    // --- Input ---
    case okidia::MessageType::INPUT_LOCK:
        inputLock();
        break;

    case okidia::MessageType::INPUT_UNLOCK:
        inputUnlock();
        break;

    case okidia::MessageType::INPUT_TAKE_CONTROL:
        inputTakeControl();
        break;

    case okidia::MessageType::REMOTE_CONTROL_STOP:
        remoteControlStop();
        break;

    // --- Fare / klavye enjeksiyonu ---
    case okidia::MessageType::MOUSE_EVENT: {
        okidia::MouseEvent evt;
        evt.ParseFromString(envelope.payload());
        handleMouseEvent(evt);
        break;
    }
    case okidia::MessageType::KEY_EVENT: {
        okidia::KeyEvent evt;
        evt.ParseFromString(envelope.payload());
        handleKeyEvent(evt);
        break;
    }

    // --- Güç ---
    case okidia::MessageType::POWER_SHUTDOWN: {
        okidia::PowerCommand cmd;
        cmd.ParseFromString(envelope.payload());
        powerShutdown(cmd);
        break;
    }
    case okidia::MessageType::POWER_RESTART: {
        okidia::PowerCommand cmd;
        cmd.ParseFromString(envelope.payload());
        powerRestart(cmd);
        break;
    }
    case okidia::MessageType::POWER_LOGOUT:
        powerLogout();
        break;

    case okidia::MessageType::POWER_HIBERNATE:
        powerHibernate();
        break;

    // --- Ses ---
    case okidia::MessageType::AUDIO_MUTE:
        audioMute();
        break;

    case okidia::MessageType::AUDIO_UNMUTE:
        audioUnmute();
        break;

    case okidia::MessageType::AUDIO_UNMUTE_ONLY:
        audioUnmuteOnly();
        break;

    case okidia::MessageType::AUDIO_PLAY_LOCAL: {
        okidia::AudioPlayLocalCommand cmd;
        if (cmd.ParseFromString(envelope.payload()))
            audioPlayLocal(cmd);
        break;
    }

    case okidia::MessageType::AUDIO_SET_VOLUME: {
        okidia::AudioVolumeCommand cmd;
        cmd.ParseFromString(envelope.payload());
        audioSetVolume(cmd);
        break;
    }

    // --- Mesaj ---
    case okidia::MessageType::MSG_POPUP: {
        okidia::ChatMessage msg;
        msg.ParseFromString(envelope.payload());
        showPopup(msg);
        break;
    }

    // --- Uygulama ---
    case okidia::MessageType::APP_LIST_REQUEST:
        appListRequest();
        break;

    case okidia::MessageType::APP_CLOSE: {
        okidia::AppCommand cmd;
        cmd.ParseFromString(envelope.payload());
        appClose(cmd);
        break;
    }
    case okidia::MessageType::APP_BLOCK_BYNAME: {
        okidia::AppCommand cmd;
        cmd.ParseFromString(envelope.payload());
        appBlockByName(cmd);
        break;
    }
    case okidia::MessageType::APP_BLOCK_BYHASH: {
        okidia::AppCommand cmd;
        cmd.ParseFromString(envelope.payload());
        appBlockByHash(cmd);
        break;
    }
    case okidia::MessageType::APP_WHITELIST_SET: {
        okidia::AppCommand cmd;
        cmd.ParseFromString(envelope.payload());
        appWhitelistSet(cmd);
        break;
    }
    case okidia::MessageType::APP_BLOCK_LIST: {
        okidia::AppCommand cmd;
        cmd.ParseFromString(envelope.payload());
        appBlockList(cmd);
        break;
    }
    case okidia::MessageType::APP_UNBLOCK:
        appUnblock();
        break;

    default:
        Logger::warning(QObject::tr("CommandHandler: Bilinmeyen komut → ") +
                        QString::number(envelope.type()));
        break;
    }
}

// ---------------------------------------------------------------------------
// Ekran komutları
// ---------------------------------------------------------------------------
void CommandHandler::sendResult(bool success, const QString& message) {
    if (!m_sender) return;

    okidia::Envelope env;
    env.set_type(okidia::MessageType::COMMAND_RESULT);

    okidia::CommandResult result;
    result.set_success(success);
    result.set_error_message(message.toStdString());
    env.set_payload(result.SerializeAsString());

    m_sender(env);
}

void CommandHandler::runCommand(const QString& program,
                                 const QStringList& args,
                                 bool waitForFinished) {
    QProcess* proc = new QProcess();
    proc->start(program, args);
    if (waitForFinished) {
        proc->waitForFinished(5000);
        int code = proc->exitCode();
        if (code != 0) {
            QString err = proc->readAllStandardError().trimmed();
            Logger::warning(QObject::tr("CommandHandler: Komut başarısız (exit ") +
                            QString::number(code) + ") → " +
                            program + " " + args.join(" ") +
                            (err.isEmpty() ? "" : " — " + err));
        } else {
            Logger::info(QObject::tr("CommandHandler: Komut → ") +
                         program + " " + args.join(" "));
        }
        delete proc;
    } else {
        QObject::connect(proc,
                         QOverload<int, QProcess::ExitStatus>::of(
                             &QProcess::finished),
                         proc, &QProcess::deleteLater);
        Logger::info(QObject::tr("CommandHandler: Komut (async) → ") +
                     program + " " + args.join(" "));
    }
}

bool CommandHandler::runCommandChecked(const QString& program,
                                        const QStringList& args) {
    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForStarted(2000)) {
        Logger::info(QObject::tr("CommandHandler: Komut bulunamadı → ") + program);
        return false;
    }
    proc.waitForFinished(5000);
    int code = proc.exitCode();
    if (code != 0) {
        Logger::info(QObject::tr("CommandHandler: ") + program + QObject::tr(" başarısız (exit ") +
                     QString::number(code) + QObject::tr("), fallback deneniyor"));
        return false;
    }
    Logger::info(QObject::tr("CommandHandler: ") + program + QObject::tr(" → OK"));
    return true;
}

QString CommandHandler::runCommandOutput(const QString& program,
                                          const QStringList& args) {
    QProcess proc;
    proc.start(program, args);
    proc.waitForFinished(5000);
    return QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
}