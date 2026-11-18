// master/ssh/SshFileManagerTransfer.cpp
#include "SshFileManagerWidget.h"
#include "TransferConflictDialog.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"

#include <QMessageBox>
#include <QInputDialog>
#include <QtConcurrent>
#include <QProcess>
#include <QUuid>
#include <QFileInfo>
#include <QDir>

// ----------------------------------------------------------------------------
// TRANSFER FUNCTIONS
// ----------------------------------------------------------------------------

void SshFileManagerWidget::onTransferToRemoteClicked() {
    if (!isConnected()) {
        QMessageBox::warning(this, tr("Hata"), tr("Lütfen önce uzak cihaza bağlanın."));
        return;
    }

    QList<QPair<QString, bool>> selectedFiles; // name, isDir

    if (m_viewMode == Details) {
        QList<QTableWidgetItem*> selectedItems = m_localFileTable->selectedItems();
        QList<int> rows;
        for (auto* item : selectedItems) {
            if (!rows.contains(item->row())) {
                rows.append(item->row());
            }
        }
        for (int r : rows) {
            QString name = m_localFileTable->item(r, 0)->text();
            bool isDir = m_localFileTable->item(r, 0)->data(Qt::UserRole + 2).toBool();
            selectedFiles.append({name, isDir});
        }
    } else {
        auto selectedItems = m_localIconList->selectedItems();
        for (auto* item : selectedItems) {
            QString name = item->text();
            bool isDir = item->data(Qt::UserRole + 2).toBool();
            selectedFiles.append({name, isDir});
        }
    }

    if (selectedFiles.isEmpty()) return;

    struct TransferTask {
        QString localPath;
        QString filename;
        QString remotePath;
        bool isDir;
        TransferConflictDialog::Resolution resolution;
    };
    QList<TransferTask> tasks;
    TransferConflictDialog::Resolution globalFileRes = TransferConflictDialog::Resolution::Cancel;
    TransferConflictDialog::Resolution globalFolderRes = TransferConflictDialog::Resolution::Cancel;
    bool hasGlobalFileRes = false;
    bool hasGlobalFolderRes = false;

    for (const auto& selFile : selectedFiles) {
        QString filename = selFile.first;
        bool isDir = selFile.second;
        QString localPath = QDir(m_localCurrentPath).absoluteFilePath(filename);
        QString remotePath = m_remoteCurrentPath;
        if (!remotePath.endsWith("/")) remotePath += "/";
        remotePath += filename;

        bool exists = false;
        for (int rr = 0; rr < m_remoteFileTable->rowCount(); ++rr) {
            if (m_remoteFileTable->item(rr, 0)->text() == filename) {
                exists = true; break;
            }
        }

        TransferConflictDialog::Resolution res = TransferConflictDialog::Resolution::Overwrite;
        if (exists) {
            if (isDir && hasGlobalFolderRes) { res = globalFolderRes; }
            else if (!isDir && hasGlobalFileRes) { res = globalFileRes; }
            else {
                TransferConflictDialog dlg(filename, isDir, this);
                if (dlg.exec() == QDialog::Accepted) {
                    res = dlg.getResolution();
                    if (dlg.applyToAll()) {
                        if (isDir) { hasGlobalFolderRes = true; globalFolderRes = res; }
                        else { hasGlobalFileRes = true; globalFileRes = res; }
                    }
                } else {
                    res = TransferConflictDialog::Resolution::Cancel;
                }
            }
        }

        if (res == TransferConflictDialog::Resolution::Cancel) return;
        if (res == TransferConflictDialog::Resolution::Skip) continue;

        if (res == TransferConflictDialog::Resolution::Copy) {
            int copyIdx = 1;
            QString baseName = filename;
            QString ext = "";
            if (!isDir) {
                int dotIdx = filename.lastIndexOf('.');
                if (dotIdx > 0) {
                    baseName = filename.left(dotIdx);
                    ext = filename.mid(dotIdx);
                }
            }
            QString newName;
            while (true) {
                newName = QString("%1 (%2)%3").arg(baseName).arg(copyIdx).arg(ext);
                bool nameExists = false;
                for (int rr = 0; rr < m_remoteFileTable->rowCount(); ++rr) {
                    if (m_remoteFileTable->item(rr, 0)->text() == newName) { nameExists = true; break; }
                }
                if (!nameExists) break;
                copyIdx++;
            }
            remotePath = m_remoteCurrentPath;
            if (!remotePath.endsWith("/")) remotePath += "/";
            remotePath += newName;
            res = TransferConflictDialog::Resolution::Overwrite; 
        }

        tasks.append({localPath, filename, remotePath, isDir, res});
    }

    if (tasks.isEmpty()) return;

    m_progressBar->setVisible(true);
    setUiEnabled(false);

    std::ignore = QtConcurrent::run([this, tasks]() {
        bool allOk = true;
        QString lastErr;
        
        for (int i = 0; i < tasks.size(); ++i) {
            const auto& t = tasks[i];
            QString opStr = QString(tr("Yükleniyor (%1/%2): %3")).arg(i+1).arg(tasks.size()).arg(t.filename);
            
            QMetaObject::invokeMethod(this, [this, opStr]() {
                m_progressLbl->setText(opStr);
                m_progressBar->setValue(0);
            }, Qt::BlockingQueuedConnection);

            bool ok = false;
            QString errStr;
            QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);

            if (t.isDir) {
                QString localTar = QDir::temp().absoluteFilePath("okidia_up_" + uuid + ".tar.gz");
                QString remoteTar = m_isRemoteWindows ? ("C:/Windows/Temp/okidia_up_" + uuid + ".tar.gz") : ("/tmp/okidia_up_" + uuid + ".tar.gz");

                QProcess proc;
                proc.setWorkingDirectory(QFileInfo(t.localPath).absolutePath());
                proc.start("tar", {"-czf", localTar, QFileInfo(t.localPath).fileName()});
                proc.waitForFinished(-1);
                
                if (proc.exitCode() == 0) {
                    bool uploadOk = m_client->uploadFile(localTar, remoteTar, [this](qint64 s, qint64 total) {
                        QMetaObject::invokeMethod(this, [this, s, total]() {
                            updateProgress(s, total, tr("Sıkıştırılmış Klasör Yükleniyor"));
                        }, Qt::QueuedConnection);
                    });
                    
                    QFile::remove(localTar);

                    if (uploadOk) {
                        QString rParent = m_remoteCurrentPath;
                        QString extractCmdFull = OkidiaUtils::remoteTarExtractCmd(
                            remoteTar, rParent, m_isRemoteWindows,
                            (t.resolution == TransferConflictDialog::Resolution::Overwrite),
                            (t.filename != QFileInfo(t.remotePath).fileName() ? t.remotePath : ""),
                            uuid);

                        int remoteExit = -1;
                        QString out = m_client->executeSync(extractCmdFull, 60000, &remoteExit);
                        if (remoteExit == 0) {
                            ok = true;
                        } else if (!m_isRemoteWindows) {
                            QString pw;
                            QMetaObject::invokeMethod(this, [this, &pw]() { pw = m_sudoPassword; }, Qt::BlockingQueuedConnection);
                            if (pw.isEmpty()) {
                                bool userOk = false;
                                QMetaObject::invokeMethod(this, [this, &pw, &userOk]() {
                                    pw = QInputDialog::getText(this, tr("Yönetici Parolası"), tr("Klasör açma işlemi başarısız oldu. Yönetici parolası ile denenecek:"), QLineEdit::Password, "", &userOk);
                                }, Qt::BlockingQueuedConnection);
                                if (userOk && !pw.isEmpty()) {
                                    QMetaObject::invokeMethod(this, [this, pw]() { m_sudoPassword = pw; }, Qt::BlockingQueuedConnection);
                                }
                            }
                            if (!pw.isEmpty()) {
                                QString sudoCmd = QString("echo '%1' | sudo -S sh -c '%2'").arg(pw, extractCmdFull.replace("'", "'\\''"));
                                remoteExit = -1;
                                out = m_client->executeSync(sudoCmd, 60000, &remoteExit);
                                if (remoteExit == 0) ok = true;
                                else errStr = tr("Sudo ile açma başarısız: ") + out.trimmed();
                            } else {
                                errStr = tr("Açma başarısız: ") + out.trimmed();
                            }
                            if (!ok) m_client->executeSync(QString("rm -f %1").arg(remoteTar), 5000);
                        } else {
                            errStr = tr("Windows klasör açma başarısız: ") + out.trimmed();
                            m_client->executeSync(QString("cmd.exe /c del /f /q \"%1\"").arg(QDir::toNativeSeparators(remoteTar)), 5000);
                        }
                    } else {
                        errStr = m_client->lastError();
                    }
                } else {
                    errStr = tr("Yerel klasör sıkıştırılamadı.");
                }
            } else {
                ok = m_client->uploadFile(t.localPath, t.remotePath, [this](qint64 s, qint64 total) {
                    QMetaObject::invokeMethod(this, [this, s, total]() { updateProgress(s, total, tr("Yükleniyor")); }, Qt::QueuedConnection);
                });
                
                if (!ok) {
                    errStr = m_client->lastError();
                    if (errStr.contains("Permission", Qt::CaseInsensitive) || errStr.contains("denied", Qt::CaseInsensitive) || errStr.isEmpty()) {
                        QString pw;
                        QMetaObject::invokeMethod(this, [this, &pw]() { pw = m_sudoPassword; }, Qt::BlockingQueuedConnection);
                        if (pw.isEmpty()) {
                            bool userOk = false;
                            QMetaObject::invokeMethod(this, [this, &pw, &userOk]() {
                                pw = QInputDialog::getText(this, tr("Yönetici Parolası"), tr("Dosya yükleme başarısız. Yönetici parolası ile tekrar denenecek:"), QLineEdit::Password, "", &userOk);
                            }, Qt::BlockingQueuedConnection);
                            if (userOk && !pw.isEmpty()) {
                                QMetaObject::invokeMethod(this, [this, pw]() { m_sudoPassword = pw; }, Qt::BlockingQueuedConnection);
                            }
                        }
                        if (!pw.isEmpty()) {
                            QString tmpPath = "/tmp/okidia_up_" + uuid;
                            bool upOk = m_client->uploadFile(t.localPath, tmpPath, [this](qint64 s, qint64 total) {
                                QMetaObject::invokeMethod(this, [this, s, total]() { updateProgress(s, total, tr("Sudo Yükleniyor")); }, Qt::QueuedConnection);
                            });
                            if (upOk) {
                                QString cmd = QString("echo '%1' | sudo -S mv '%2' '%3'").arg(pw, tmpPath, t.remotePath);
                                int exitCode = -1;
                                QString out = m_client->executeSync(cmd, 10000, &exitCode);
                                if (exitCode == 0) ok = true;
                                else {
                                    errStr = tr("Sudo işlemi başarısız: ") + out.trimmed();
                                    m_client->executeSync(QString("rm -f '%1'").arg(tmpPath), 5000);
                                }
                            } else {
                                errStr = tr("Sudo geçici dosya yükleme başarısız: ") + m_client->lastError();
                            }
                        }
                    }
                }
            }

            if (!ok) {
                allOk = false;
                lastErr = errStr;
                break; // stop loop on error
            }
        }

        QMetaObject::invokeMethod(this, [this, allOk, lastErr]() {
            m_progressBar->setVisible(false);
            m_progressLbl->clear();
            setUiEnabled(true);
            if (allOk) {
                Logger::info(QObject::tr("Tüm aktarımlar tamamlandı."));
                loadRemoteDirectory(m_remoteCurrentPath);
            } else {
                QMessageBox::critical(this, tr("Hata"), tr("Aktarım sırasında hata oluştu:\n") + lastErr);
            }
        }, Qt::QueuedConnection);
    });
}

void SshFileManagerWidget::onTransferToLocalClicked() {
    if (!isConnected()) {
        QMessageBox::warning(this, tr("Hata"), tr("Lütfen önce uzak cihaza bağlanın."));
        return;
    }

    QList<QPair<QString, bool>> selectedFiles; // name, isDir

    if (m_viewMode == Details) {
        QList<QTableWidgetItem*> selectedItems = m_remoteFileTable->selectedItems();
        QList<int> rows;
        for (auto* item : selectedItems) {
            if (!rows.contains(item->row())) {
                rows.append(item->row());
            }
        }
        for (int r : rows) {
            QString name = m_remoteFileTable->item(r, 0)->text();
            bool isDir = m_remoteFileTable->item(r, 0)->data(Qt::UserRole + 2).toBool();
            selectedFiles.append({name, isDir});
        }
    } else {
        auto selectedItems = m_remoteIconList->selectedItems();
        for (auto* item : selectedItems) {
            QString name = item->text();
            bool isDir = item->data(Qt::UserRole + 2).toBool();
            selectedFiles.append({name, isDir});
        }
    }

    if (selectedFiles.isEmpty()) return;

    struct TransferTask {
        QString localPath;
        QString filename;
        QString remotePath;
        bool isDir;
        TransferConflictDialog::Resolution resolution;
    };
    QList<TransferTask> tasks;
    TransferConflictDialog::Resolution globalFileRes = TransferConflictDialog::Resolution::Cancel;
    TransferConflictDialog::Resolution globalFolderRes = TransferConflictDialog::Resolution::Cancel;
    bool hasGlobalFileRes = false;
    bool hasGlobalFolderRes = false;

    for (const auto& selFile : selectedFiles) {
        QString filename = selFile.first;
        bool isDir = selFile.second;
        QString remotePath = m_remoteCurrentPath;
        if (!remotePath.endsWith("/")) remotePath += "/";
        remotePath += filename;
        
        QString localPath = QDir(m_localCurrentPath).absoluteFilePath(filename);

        bool exists = QFile::exists(localPath);

        TransferConflictDialog::Resolution res = TransferConflictDialog::Resolution::Overwrite;
        if (exists) {
            if (isDir && hasGlobalFolderRes) { res = globalFolderRes; }
            else if (!isDir && hasGlobalFileRes) { res = globalFileRes; }
            else {
                TransferConflictDialog dlg(filename, isDir, this);
                if (dlg.exec() == QDialog::Accepted) {
                    res = dlg.getResolution();
                    if (dlg.applyToAll()) {
                        if (isDir) { hasGlobalFolderRes = true; globalFolderRes = res; }
                        else { hasGlobalFileRes = true; globalFileRes = res; }
                    }
                } else {
                    res = TransferConflictDialog::Resolution::Cancel;
                }
            }
        }

        if (res == TransferConflictDialog::Resolution::Cancel) return;
        if (res == TransferConflictDialog::Resolution::Skip) continue;

        if (res == TransferConflictDialog::Resolution::Copy) {
            int copyIdx = 1;
            QString baseName = filename;
            QString ext = "";
            if (!isDir) {
                int dotIdx = filename.lastIndexOf('.');
                if (dotIdx > 0) {
                    baseName = filename.left(dotIdx);
                    ext = filename.mid(dotIdx);
                }
            }
            QString newName;
            while (true) {
                newName = QString("%1 (%2)%3").arg(baseName).arg(copyIdx).arg(ext);
                QString newLocalPath = QDir(m_localCurrentPath).absoluteFilePath(newName);
                if (!QFile::exists(newLocalPath)) {
                    localPath = newLocalPath;
                    break;
                }
                copyIdx++;
            }
            res = TransferConflictDialog::Resolution::Overwrite; 
        }

        tasks.append({localPath, filename, remotePath, isDir, res});
    }

    if (tasks.isEmpty()) return;

    m_progressBar->setVisible(true);
    setUiEnabled(false);

    std::ignore = QtConcurrent::run([this, tasks]() {
        bool allOk = true;
        QString lastErr;
        
        for (int i = 0; i < tasks.size(); ++i) {
            const auto& t = tasks[i];
            QString opStr = QString(tr("İndiriliyor (%1/%2): %3")).arg(i+1).arg(tasks.size()).arg(t.filename);
            
            QMetaObject::invokeMethod(this, [this, opStr]() {
                m_progressLbl->setText(opStr);
                m_progressBar->setValue(0);
            }, Qt::BlockingQueuedConnection);

            bool ok = false;
            QString errStr;
            QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);

            if (t.isDir) {
                QString localTar = QDir::temp().absoluteFilePath("okidia_dl_" + uuid + ".tar.gz");
                QString remoteTar = OkidiaUtils::remoteTempPath(m_isRemoteWindows, "okidia_dl_" + uuid + ".tar.gz");

                QString folderName = QFileInfo(t.remotePath).fileName();
                QString rParent = m_remoteCurrentPath;
                QString compressCmd = OkidiaUtils::remoteTarCompressCmd(remoteTar, rParent, folderName, m_isRemoteWindows);

                int exitCode = -1;
                QString out = m_client->executeSync(compressCmd, 60000, &exitCode);
                
                if (exitCode != 0 && !m_isRemoteWindows) {
                    // SUDO FALLBACK FOR COMPRESS
                    QString pw;
                    QMetaObject::invokeMethod(this, [this, &pw]() { pw = m_sudoPassword; }, Qt::BlockingQueuedConnection);
                    if (pw.isEmpty()) {
                        bool userOk = false;
                        QMetaObject::invokeMethod(this, [this, &pw, &userOk]() {
                            pw = QInputDialog::getText(this, tr("Yönetici Parolası"), tr("Klasör sıkıştırma başarısız. Yönetici parolası ile denenecek:"), QLineEdit::Password, "", &userOk);
                        }, Qt::BlockingQueuedConnection);
                        if (userOk && !pw.isEmpty()) {
                            QMetaObject::invokeMethod(this, [this, pw]() { m_sudoPassword = pw; }, Qt::BlockingQueuedConnection);
                        }
                    }
                    if (!pw.isEmpty()) {
                        QString sudoCmd = QString("echo '%1' | sudo -S sh -c '%2'").arg(pw, compressCmd.replace("'", "'\\''"));
                        exitCode = -1;
                        out = m_client->executeSync(sudoCmd, 60000, &exitCode);
                        if (exitCode != 0) errStr = tr("Sudo ile sıkıştırma başarısız: ") + out.trimmed();
                    } else {
                        errStr = tr("Uzak klasör sıkıştırılamadı: ") + out.trimmed();
                    }
                } else if (exitCode != 0) {
                    errStr = tr("Windows uzak klasör sıkıştırılamadı: ") + out.trimmed();
                }

                if (exitCode == 0) {
                    bool dlOk = m_client->downloadFile(remoteTar, localTar, [this](qint64 s, qint64 total) {
                        QMetaObject::invokeMethod(this, [this, s, total]() {
                            updateProgress(s, total, tr("Sıkıştırılmış Klasör İndiriliyor"));
                        }, Qt::QueuedConnection);
                    });

                    // cleanup remote tar
                    if (m_isRemoteWindows) {
                        m_client->executeSync(QString("cmd.exe /c del /f /q \"%1\"").arg(QDir::toNativeSeparators(remoteTar)), 5000);
                    } else {
                        QString pw;
                        QMetaObject::invokeMethod(this, [this, &pw]() { pw = m_sudoPassword; }, Qt::BlockingQueuedConnection);
                        if (!pw.isEmpty()) {
                            m_client->executeSync(QString("echo '%1' | sudo -S rm -f %2").arg(pw, remoteTar), 5000);
                        } else {
                            m_client->executeSync(QString("rm -f %1").arg(remoteTar), 5000);
                        }
                    }

                    if (dlOk) {
                        if (t.resolution == TransferConflictDialog::Resolution::Overwrite) {
                            QDir d(t.localPath);
                            d.removeRecursively();
                        }

                        QProcess proc;
                        proc.setWorkingDirectory(m_localCurrentPath);
                        if (t.filename != QFileInfo(t.localPath).fileName()) {
                            QString tmpExt = QDir::temp().absoluteFilePath("okidia_ext_" + uuid);
                            QDir().mkpath(tmpExt);
                            proc.setWorkingDirectory(tmpExt);
                            proc.start("tar", {"-xzf", localTar});
                            proc.waitForFinished(-1);
                            
                            QDir().rename(QDir(tmpExt).absoluteFilePath(folderName), t.localPath);
                            QDir(tmpExt).removeRecursively();
                            ok = true;
                        } else {
                            proc.start("tar", {"-xzf", localTar});
                            proc.waitForFinished(-1);
                            if (proc.exitCode() == 0) ok = true;
                            else errStr = tr("Yerel arşiv açılamadı.");
                        }
                    } else {
                        errStr = m_client->lastError();
                    }
                    QFile::remove(localTar);
                }
            } else {
                ok = m_client->downloadFile(t.remotePath, t.localPath, [this](qint64 s, qint64 total) {
                    QMetaObject::invokeMethod(this, [this, s, total]() { updateProgress(s, total, tr("İndiriliyor")); }, Qt::QueuedConnection);
                });
                
                if (!ok) {
                    errStr = m_client->lastError();
                    if (errStr.contains("Permission", Qt::CaseInsensitive) || errStr.contains("denied", Qt::CaseInsensitive) || errStr.isEmpty()) {
                        QString pw;
                        QMetaObject::invokeMethod(this, [this, &pw]() { pw = m_sudoPassword; }, Qt::BlockingQueuedConnection);
                        if (pw.isEmpty()) {
                            bool userOk = false;
                            QMetaObject::invokeMethod(this, [this, &pw, &userOk]() {
                                pw = QInputDialog::getText(this, tr("Yönetici Parolası"), tr("Dosya indirme başarısız. Yönetici parolası ile tekrar denenecek:"), QLineEdit::Password, "", &userOk);
                            }, Qt::BlockingQueuedConnection);
                            if (userOk && !pw.isEmpty()) {
                                QMetaObject::invokeMethod(this, [this, pw]() { m_sudoPassword = pw; }, Qt::BlockingQueuedConnection);
                            }
                        }
                        if (!pw.isEmpty()) {
                            QString tmpPath = "/tmp/okidia_dl_" + uuid;
                            QString cmd = QString("echo '%1' | sudo -S cp '%2' '%3' && echo '%1' | sudo -S chmod 666 '%3'").arg(pw, t.remotePath, tmpPath);
                            int exitCode = -1;
                            QString out = m_client->executeSync(cmd, 10000, &exitCode);
                            if (exitCode == 0) {
                                bool dlOk = m_client->downloadFile(tmpPath, t.localPath, [this](qint64 s, qint64 total) {
                                    QMetaObject::invokeMethod(this, [this, s, total]() { updateProgress(s, total, tr("Sudo İndiriliyor")); }, Qt::QueuedConnection);
                                });
                                m_client->executeSync(QString("echo '%1' | sudo -S rm -f '%2'").arg(pw, tmpPath), 5000);
                                if (dlOk) ok = true;
                                else errStr = tr("Sudo geçici dosya indirme başarısız: ") + m_client->lastError();
                            } else {
                                errStr = tr("Sudo hazırlık başarısız: ") + out.trimmed();
                            }
                        }
                    }
                }
            }

            if (!ok) {
                allOk = false;
                lastErr = errStr;
                break; // stop loop on error
            }
        }

        QMetaObject::invokeMethod(this, [this, allOk, lastErr]() {
            m_progressBar->setVisible(false);
            m_progressLbl->clear();
            setUiEnabled(true);
            if (allOk) {
                Logger::info(QObject::tr("Tüm indirmeler tamamlandı."));
                loadLocalDirectory(m_localCurrentPath);
            } else {
                QMessageBox::critical(this, tr("Hata"), tr("İndirme sırasında hata oluştu:\n") + lastErr);
            }
        }, Qt::QueuedConnection);
    });
}
