// common/crypto/KeyManagerTest.h
#pragma once

#include "KeyManager.h"
#include "../utils/Logger.h"
#include <QDir>

class KeyManagerTest {
public:
    static bool runAll() {
        Logger::info(QObject::tr("=== KeyManager Testleri Başlıyor ==="));

        bool ok = true;
        ok &= testGenerateKeyPair();
        ok &= testSignAndVerify();
        ok &= testWrongKey();
        ok &= testFingerprint();
        ok &= testChallenge();

        if (ok)
            Logger::info(QObject::tr("=== Tüm testler BAŞARILI ==="));
        else
            Logger::error(QObject::tr("=== Bazı testler BAŞARISIZ ==="));

        return ok;
    }

private:
    static bool testGenerateKeyPair() {
        Logger::info(QObject::tr("Test 1: Keypair üretimi..."));

        bool ok = KeyManager::generateKeyPair("/tmp/test_private.pem",
                                               "/tmp/test_public.pem");
        if (!ok) {
            Logger::error(QObject::tr("  BAŞARISIZ: Keypair üretilemedi"));
            return false;
        }

        // Dosyalar oluştu mu?
        if (!QFile::exists("/tmp/test_private.pem") ||
            !QFile::exists("/tmp/test_public.pem")) {
            Logger::error(QObject::tr("  BAŞARISIZ: Dosyalar oluşmadı"));
            return false;
        }

        Logger::info(QObject::tr("  BAŞARILI"));
        return true;
    }

    static bool testSignAndVerify() {
        Logger::info(QObject::tr("Test 2: İmzalama ve doğrulama..."));

        QByteArray challenge = KeyManager::generateChallenge();
        if (challenge.isEmpty()) {
            Logger::error(QObject::tr("  BAŞARISIZ: Challenge üretilemedi"));
            return false;
        }

        QByteArray signature = KeyManager::signChallenge(
            challenge, "/tmp/test_private.pem");
        if (signature.isEmpty()) {
            Logger::error(QObject::tr("  BAŞARISIZ: İmzalanamadı"));
            return false;
        }

        bool valid = KeyManager::verifySignature(
            challenge, signature, "/tmp/test_public.pem");
        if (!valid) {
            Logger::error(QObject::tr("  BAŞARISIZ: Doğrulama başarısız"));
            return false;
        }

        Logger::info(QObject::tr("  BAŞARILI"));
        return true;
    }

    static bool testWrongKey() {
        Logger::info(QObject::tr("Test 3: Yanlış key ile doğrulama (başarısız olmalı)..."));

        // İkinci bir keypair üret
        KeyManager::generateKeyPair("/tmp/test_private2.pem",
                                     "/tmp/test_public2.pem");

        QByteArray challenge = KeyManager::generateChallenge();
        QByteArray signature = KeyManager::signChallenge(
            challenge, "/tmp/test_private.pem");

        // Farklı public key ile doğrula → başarısız olmalı
        bool valid = KeyManager::verifySignature(
            challenge, signature, "/tmp/test_public2.pem");

        if (valid) {
            Logger::error(QObject::tr("  BAŞARISIZ: Yanlış key kabul edildi!"));
            return false;
        }

        Logger::info(QObject::tr("  BAŞARILI (yanlış key reddedildi)"));
        return true;
    }

    static bool testFingerprint() {
        Logger::info(QObject::tr("Test 4: Fingerprint..."));

        QString fp = KeyManager::getFingerprint("/tmp/test_public.pem");
        if (fp.isEmpty()) {
            Logger::error(QObject::tr("  BAŞARISIZ: Fingerprint alınamadı"));
            return false;
        }

        Logger::info(QObject::tr("  Fingerprint: ") + fp);
        Logger::info(QObject::tr("  BAŞARILI"));
        return true;
    }

    static bool testChallenge() {
        Logger::info(QObject::tr("Test 5: Her challenge farklı olmalı..."));

        QByteArray c1 = KeyManager::generateChallenge();
        QByteArray c2 = KeyManager::generateChallenge();

        if (c1 == c2) {
            Logger::error(QObject::tr("  BAŞARISIZ: İki challenge aynı!"));
            return false;
        }

        if (c1.size() != 32 || c2.size() != 32) {
            Logger::error(QObject::tr("  BAŞARISIZ: Challenge boyutu 32 byte değil"));
            return false;
        }

        Logger::info(QObject::tr("  BAŞARILI"));
        return true;
    }
};