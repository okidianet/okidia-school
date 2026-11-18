// master/backup/BackupManager.cpp
#include "BackupManager.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"

#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QRandomGenerator>

BackupManager& BackupManager::instance() {
    static BackupManager s;
    return s;
}

BackupManager::BackupManager(QObject* parent)
    : QObject(parent) {}

bool BackupManager::backup(const QString& targetDir,
                           const QString& adminPassword,
                           const BackupContent& content) {
    QDir().mkpath(targetDir);

    QString filename = targetDir + "/okidia_" +
        QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".okbak";

    QString err;
    if (!BackupPackage::packToFile(filename, adminPassword, content, &err)) {
        Logger::error(QObject::tr("BackupManager: Yedekleme başarısız → ") + err);
        emit backupFinished(false, QString());
        return false;
    }

    emit backupFinished(true, filename);
    return true;
}

bool BackupManager::restore(const QString& backupPath,
                            const QString& adminPassword,
                            BackupContent& outContent) {
    QString err;
    if (!BackupPackage::unpackFromFile(backupPath, adminPassword, outContent, &err)) {
        Logger::error(QObject::tr("BackupManager: Geri yükleme başarısız → ") + err);
        emit restoreFinished(false);
        return false;
    }

    emit restoreFinished(true);
    return true;
}

QStringList BackupManager::generateRecoveryPhrase() {
    const QStringList& words = wordList();
    QStringList phrase;
    for (int i = 0; i < 24; ++i)
        phrase << words[QRandomGenerator::global()->bounded(words.size())];
    return phrase;
}

QString BackupManager::passwordFromPhrase(const QStringList& phrase) {
    return OkidiaUtils::sha256(phrase.join(" ").toUtf8());
}

QList<QDateTime> BackupManager::listBackups(const QString& dir) const {
    QList<QDateTime> list;
    QDir d(dir);
    for (const auto& fi : d.entryInfoList({"*.okbak"}, QDir::Files, QDir::Time))
        list << fi.lastModified();
    return list;
}

const QStringList& BackupManager::wordList() {
    static const QStringList words = {
        "elma","araba","masa","kalem","kitap",tr("kapı"),"pencere",tr("bahçe"),
        "okul",tr("müzik"),"deniz",tr("dağ"),"orman","nehir",tr("göl"),tr("şehir"),
        tr("köprü"),tr("yıldız"),tr("güneş"),"ay","bulut",tr("yağmur"),"kar",tr("rüzgar"),
        tr("çiçek"),tr("ağaç"),"yaprak",tr("taş"),"toprak",tr("ateş"),"su","hava",
        "kedi",tr("köpek"),tr("kuş"),tr("balık"),"at","aslan","kaplan",tr("ayı"),
        tr("kırmızı"),"mavi",tr("yeşil"),tr("sarı"),"beyaz","siyah","turuncu","mor",
        "bir","iki",tr("üç"),tr("dört"),tr("beş"),tr("altı"),"yedi","sekiz","dokuz","on"
    };
    return words;
}
