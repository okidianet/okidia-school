// master/ssh/SshFileManagerRemote.cpp
#include "SshFileManagerWidget.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"

#include <QApplication>
#include <QMessageBox>
#include <QInputDialog>
#include <QMenu>
#include <QHeaderView>
#include <QDesktopServices>
#include <QUrl>
#include <QSettings>
#include <QCheckBox>
#include <QtConcurrent>
#include <QRegularExpression>
#include <QStyle>
#include <algorithm>

// ----------------------------------------------------------------------------
// REMOTE SLOTS & FUNCTIONS
// ----------------------------------------------------------------------------

void SshFileManagerWidget::onRemoteUpClicked() {
    QString current = OkidiaUtils::fromRemotePathSeparators(m_remoteCurrentPath);
    if (current.isEmpty()) return;

    if (m_isRemoteWindows) {
        if (current == tr("Bilgisayarım") || current == "Bilgisayarım") {
            return;
        }
        if (current.size() <= 3 && current.contains(":")) {
            loadRemoteDirectory(tr("Bilgisayarım"));
            return; // Sürücü kökündeyken bir üste basınca "Bilgisayarım" sürücü listesine geç
        }
        while (current.endsWith('/') && current.size() > 3) {
            current.chop(1);
        }
        int idx = current.lastIndexOf('/');
        if (idx < 0) {
            loadRemoteDirectory(tr("Bilgisayarım"));
            return;
        }
        QString parent = current.left(idx);
        if (parent.endsWith(':')) {
            parent += "/";
        }
        if (parent.isEmpty()) parent = tr("Bilgisayarım");
        loadRemoteDirectory(parent);
    } else {
        if (current == "/") return;
        while (current.endsWith('/') && current.size() > 1) {
            current.chop(1);
        }
        int idx = current.lastIndexOf('/');
        if (idx < 0) return;
        QString parent = current.left(idx);
        if (parent.isEmpty()) parent = "/";
        loadRemoteDirectory(parent);
    }
}

void SshFileManagerWidget::onRemoteRefreshClicked() {
    loadRemoteDirectory(m_remoteCurrentPath);
}

void SshFileManagerWidget::loadRemoteDirectory(const QString& path) {
    if (!isConnected() || m_loadingRemote) return;

    QString cleanPath = OkidiaUtils::fromRemotePathSeparators(path);
    if (m_isRemoteWindows) {
        if (cleanPath == tr("Bilgisayarım") || cleanPath == "Bilgisayarım" || cleanPath == "/") {
            cleanPath = tr("Bilgisayarım");
        } else if (cleanPath.endsWith(':')) {
            cleanPath += "/";
        } else if (cleanPath.isEmpty()) {
            cleanPath = tr("Bilgisayarım");
        }
    } else {
        if (cleanPath.isEmpty()) cleanPath = "/";
    }

    if (!m_remoteHistorySkip) {
        if (m_remoteHistoryIdx >= 0 && m_remoteHistoryIdx < m_remoteHistory.size() - 1) {
            m_remoteHistory.erase(m_remoteHistory.begin() + m_remoteHistoryIdx + 1, m_remoteHistory.end());
        }
        if (m_remoteHistory.isEmpty() || m_remoteHistory.last() != cleanPath) {
            m_remoteHistory.append(cleanPath);
            m_remoteHistoryIdx = m_remoteHistory.size() - 1;
        }
    }
    m_remoteHistorySkip = false;

    m_loadingRemote = true;
    m_progressLbl->setText(tr("Uzak klasör yükleniyor..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);

    bool showHidden = m_remoteShowHidden->isChecked();

    std::ignore = QtConcurrent::run([this, cleanPath, showHidden]() {
        QList<FileInfo> filesList;

        if (m_isRemoteWindows && (cleanPath == tr("Bilgisayarım") || cleanPath == "Bilgisayarım")) {
            QString drivesOut = m_client->executeSync("cmd.exe /c fsutil fsinfo drives", 3000).trimmed();
            QStringList driveList;
            if (drivesOut.contains("Drives:", Qt::CaseInsensitive)) {
                drivesOut.remove("Drives:", Qt::CaseInsensitive);
                for (const QString& d : drivesOut.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)) {
                    QString dClean = OkidiaUtils::fromRemotePathSeparators(d);
                    if (!dClean.endsWith("/")) dClean += "/";
                    if (!driveList.contains(dClean, Qt::CaseInsensitive)) driveList.append(dClean);
                }
            }
            if (driveList.isEmpty()) {
                QString psOut = m_client->executeSync("powershell -NoProfile -Command \"(Get-PSDrive -PSProvider FileSystem).Root\"", 4000).trimmed();
                for (const QString& d : psOut.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts)) {
                    QString dClean = OkidiaUtils::fromRemotePathSeparators(d);
                    if (!dClean.endsWith("/")) dClean += "/";
                    if (!driveList.contains(dClean, Qt::CaseInsensitive)) driveList.append(dClean);
                }
            }
            if (driveList.isEmpty()) {
                driveList.append("C:/");
            }
            for (const QString& drv : driveList) {
                FileInfo info;
                info.name = drv;
                info.size = 0;
                info.isDir = true;
                info.permissions = "-";
                filesList.append(info);
            }
        } else {
            QList<SshClient::RemoteFileInfo> sftpFiles;
            bool sftpOk = m_client->listDirectory(cleanPath, sftpFiles);
            if (sftpOk) {
                for (const auto& sf : sftpFiles) {
                    if (!showHidden && sf.name.startsWith(".")) continue;
                    FileInfo info;
                    info.name = sf.name;
                    info.size = sf.size;
                    info.isDir = sf.isDir;
                    info.permissions = sf.permissions;
                    info.mtime = sf.mtime;
                    filesList.append(info);
                }
            } else {
                // SFTP başarısız olursa OS'a göre fallback shell komutu çalıştır
                if (m_isRemoteWindows) {
                    // Windows PowerShell Fallback
                    QString psCmd = QString("powershell -NoProfile -Command \""
                                            "Get-ChildItem -LiteralPath '%1' %2 | ForEach-Object { "
                                            "  $type = if ($_.PSIsContainer) { 'd' } else { '-' }; "
                                            "  $len = if ($_.PSIsContainer) { 0 } else { $_.Length }; "
                                            "  $time = [DateTimeOffset]::new($_.LastWriteTime).ToUnixTimeSeconds(); "
                                            "  Write-Output ($type + '|' + $len + '|0755|' + $time + '|' + $_.Name) "
                                            "}\"")
                                    .arg(cleanPath, showHidden ? "-Force" : "");
                    QString output = m_client->executeSync(psCmd, 10000);
                    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
                    for (const QString& line : lines) {
                        QStringList parts = line.trimmed().split('|');
                        if (parts.size() < 5) continue;
                        QString type = parts[0];
                        qint64 size = parts[1].toLongLong();
                        QString permissions = parts[2];
                        double timestamp = parts[3].toDouble();
                        QString name = parts.mid(4).join('|').trimmed();
                        if (name.isEmpty() || name == "." || name == "..") continue;

                        FileInfo info;
                        info.name = name;
                        info.size = size;
                        info.isDir = (type == "d");
                        info.permissions = permissions;
                        info.mtime = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(timestamp));
                        filesList.append(info);
                    }
                } else {
                    // Linux find Fallback
                    QString escPath = cleanPath;
                    escPath.replace("\"", "\\\"");
                    QString hiddenFilter = showHidden ? "" : "! -name \".*\"";
                    QString cmd = QString("cd \"%1\" && find . -maxdepth 1 %2 -printf \"%y|%s|%m|%T@|%f\\n\" 2>/dev/null")
                                  .arg(escPath, hiddenFilter);
                    QString output = m_client->executeSync(cmd, 10000);
                    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
                    for (const QString& line : lines) {
                        QStringList parts = line.split('|');
                        if (parts.size() < 5) continue;
                        QString type = parts[0];
                        qint64 size = parts[1].toLongLong();
                        QString permissions = parts[2];
                        double timestamp = parts[3].toDouble();
                        QString name = parts.mid(4).join('|').trimmed();
                        if (name == "." || name == "./" || name.isEmpty()) continue;
                        if (name.startsWith("./")) name = name.mid(2);

                        FileInfo info;
                        info.name = name;
                        info.size = size;
                        info.isDir = (type == "d");
                        info.permissions = permissions;
                        info.mtime = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(timestamp));
                        filesList.append(info);
                    }
                }
            }
        }

        // Klasörler ve sürücüler üste, değiştirilme tarihi en yeni en üstte
        std::sort(filesList.begin(), filesList.end(), [](const FileInfo& a, const FileInfo& b) {
            if (a.isDir != b.isDir) {
                return a.isDir > b.isDir;
            }
            if (a.mtime != b.mtime) {
                return a.mtime > b.mtime;
            }
            return OkidiaUtils::naturalCompare(a.name, b.name) < 0;
        });

        QMetaObject::invokeMethod(this, [this, filesList, cleanPath]() {
            m_remoteCurrentPath = cleanPath;
            m_remoteFiles = filesList;
            renderRemoteFiles();
            m_progressLbl->clear();
            QApplication::restoreOverrideCursor();
            m_loadingRemote = false;
        }, Qt::QueuedConnection);
    });
}

void SshFileManagerWidget::renderRemoteFiles() {
    m_remoteFileTable->setSortingEnabled(false);
    m_remoteFileTable->setRowCount(0);
    m_remoteIconList->clear();
    m_remotePathEdit->setText(m_remoteCurrentPath == tr("Bilgisayarım") ? tr("Bilgisayarım") : OkidiaUtils::normalizeRemotePath(m_remoteCurrentPath, m_isRemoteWindows, m_isRemoteWindows));

    for (const auto& file : m_remoteFiles) {
        QIcon icon;
        if (file.isDir) {
            if (file.name.contains(":") || file.name == tr("Bilgisayarım")) {
                icon = style()->standardIcon(QStyle::SP_DriveHDIcon);
            } else {
                icon = style()->standardIcon(QStyle::SP_DirIcon);
            }
        } else {
            icon = style()->standardIcon(QStyle::SP_FileIcon);
        }
        
        if (m_viewMode == Details) {
            int r = m_remoteFileTable->rowCount();
            m_remoteFileTable->insertRow(r);

            auto* nameItem = new OkidiaTableItem(file.name);
            nameItem->setIcon(icon);
            nameItem->setData(Qt::UserRole, file.isDir ? 0 : 1);
            nameItem->setData(Qt::UserRole + 2, file.isDir);
            m_remoteFileTable->setItem(r, 0, nameItem);

            QString sizeStr = "-";
            if (!file.isDir) {
                sizeStr = OkidiaUtils::formatSize(file.size);
            }
            auto* sizeItem = new OkidiaTableItem(sizeStr);
            sizeItem->setData(Qt::UserRole, file.isDir ? 0 : 1);
            sizeItem->setData(Qt::UserRole + 1, file.isDir ? (qulonglong)0 : (qulonglong)file.size);
            sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_remoteFileTable->setItem(r, 1, sizeItem);

            QString typeStr;
            if (file.isDir) {
                typeStr = file.name.contains(":") ? tr("Sürücü") : tr("Klasör");
            } else {
                QFileInfo fi(file.name);
                QString ext = fi.suffix().toUpper();
                typeStr = ext.isEmpty() ? tr("Dosya") : QString("%1 %2").arg(ext, tr("Dosyası"));
            }
            auto* typeItem = new OkidiaTableItem(typeStr);
            typeItem->setData(Qt::UserRole, file.isDir ? 0 : 1);
            m_remoteFileTable->setItem(r, 2, typeItem);

            auto* permItem = new OkidiaTableItem(file.permissions.isEmpty() ? "-" : file.permissions);
            permItem->setData(Qt::UserRole, file.isDir ? 0 : 1);
            permItem->setTextAlignment(Qt::AlignCenter);
            m_remoteFileTable->setItem(r, 3, permItem);

            QString dateStr = file.mtime.isValid() ? file.mtime.toString("yyyy-MM-dd HH:mm:ss") : "-";
            auto* mtimeItem = new OkidiaTableItem(dateStr);
            mtimeItem->setData(Qt::UserRole, file.isDir ? 0 : 1);
            mtimeItem->setData(Qt::UserRole + 1, file.mtime.isValid() ? (qulonglong)file.mtime.toSecsSinceEpoch() : (qulonglong)0);
            mtimeItem->setTextAlignment(Qt::AlignCenter);
            m_remoteFileTable->setItem(r, 4, mtimeItem);
        } else {
            auto* item = new QListWidgetItem(icon, file.name, m_remoteIconList);
            item->setData(Qt::UserRole, file.isDir ? 0 : 1);
            item->setData(Qt::UserRole + 2, file.isDir);
            item->setToolTip(file.name);
        }
    }

    if (m_viewMode == Details) {
        int sortCol = m_remoteFileTable->horizontalHeader()->sortIndicatorSection();
        Qt::SortOrder sortOrd = m_remoteFileTable->horizontalHeader()->sortIndicatorOrder();
        if (sortCol < 0) {
            sortCol = 4; // Değiştirilme Tarihi
            sortOrd = Qt::DescendingOrder;
        }
        m_remoteFileTable->setSortingEnabled(true);
        m_remoteFileTable->sortByColumn(sortCol, sortOrd);
        m_remoteFileTable->horizontalHeader()->setSortIndicator(sortCol, sortOrd);
    }
}

void SshFileManagerWidget::onRemoteTableDoubleClicked(int row, int column) {
    Q_UNUSED(column)
    auto* item = m_remoteFileTable->item(row, 0);
    if (!item) return;

    bool isDir = (item->data(Qt::UserRole).toInt() == 0);
    if (isDir) {
        QString dirName = item->text();
        if (m_isRemoteWindows && (m_remoteCurrentPath == tr("Bilgisayarım") || m_remoteCurrentPath == "Bilgisayarım")) {
            loadRemoteDirectory(dirName);
        } else {
            QString nextPath = OkidiaUtils::fromRemotePathSeparators(m_remoteCurrentPath);
            if (!nextPath.endsWith("/")) nextPath += "/";
            nextPath += dirName;
            loadRemoteDirectory(nextPath);
        }
    } else {
        QString fileName = item->text();
        QString remotePath = OkidiaUtils::fromRemotePathSeparators(m_remoteCurrentPath);
        if (!remotePath.endsWith("/")) remotePath += "/";
        remotePath += fileName;
        downloadAndOpenRemoteFile(remotePath, fileName, false);
    }
}

void SshFileManagerWidget::onRemoteItemDoubleClicked(QListWidgetItem* item) {
    if (!item) return;
    bool isDir = (item->data(Qt::UserRole).toInt() == 0);
    if (isDir) {
        QString dirName = item->text();
        if (m_isRemoteWindows && (m_remoteCurrentPath == tr("Bilgisayarım") || m_remoteCurrentPath == "Bilgisayarım")) {
            loadRemoteDirectory(dirName);
        } else {
            QString nextPath = OkidiaUtils::fromRemotePathSeparators(m_remoteCurrentPath);
            if (!nextPath.endsWith("/")) nextPath += "/";
            nextPath += dirName;
            loadRemoteDirectory(nextPath);
        }
    } else {
        QString fileName = item->text();
        QString remotePath = OkidiaUtils::fromRemotePathSeparators(m_remoteCurrentPath);
        if (!remotePath.endsWith("/")) remotePath += "/";
        remotePath += fileName;
        downloadAndOpenRemoteFile(remotePath, fileName, false);
    }
}

void SshFileManagerWidget::showRemoteContextMenu(const QPoint& pos) {
    if (!isConnected()) return;

    QMenu menu(this);
    bool hasSelection = false;
    bool isDir = false;
    QString selectedName;

    if (m_viewMode == Details) {
        QTableWidgetItem* item = m_remoteFileTable->itemAt(pos);
        if (item) {
            hasSelection = true;
            int r = item->row();
            selectedName = m_remoteFileTable->item(r, 0)->text();
            isDir = (m_remoteFileTable->item(r, 0)->data(Qt::UserRole).toInt() == 0);
            if (isDir) {
                menu.addAction(tr("Aç"), this, [this, r]() { onRemoteTableDoubleClicked(r, 0); });
            } else {
                QString remotePath = m_remoteCurrentPath;
                if (!remotePath.endsWith("/")) remotePath += "/";
                remotePath += selectedName;
                menu.addAction(tr("İndir ve Aç"), this, [this, remotePath, selectedName]() {
                    downloadAndOpenRemoteFile(remotePath, selectedName, true);
                });
                menu.addAction(tr("İndir"), this, &SshFileManagerWidget::onTransferToLocalClicked);
            }
        }
    } else {
        QListWidgetItem* item = m_remoteIconList->itemAt(pos);
        if (item) {
            hasSelection = true;
            selectedName = item->text();
            isDir = (item->data(Qt::UserRole).toInt() == 0);
            if (isDir) {
                menu.addAction(tr("Aç"), this, [this, item]() { onRemoteItemDoubleClicked(item); });
            } else {
                QString remotePath = m_remoteCurrentPath;
                if (!remotePath.endsWith("/")) remotePath += "/";
                remotePath += selectedName;
                menu.addAction(tr("İndir ve Aç"), this, [this, remotePath, selectedName]() {
                    downloadAndOpenRemoteFile(remotePath, selectedName, true);
                });
                menu.addAction(tr("İndir"), this, &SshFileManagerWidget::onTransferToLocalClicked);
            }
        }
    }

    if (hasSelection) {
        menu.addAction(tr("Yeniden Adlandır..."), this, &SshFileManagerWidget::onRemoteRenameAction);
        menu.addAction(tr("Sil"), this, &SshFileManagerWidget::onRemoteDeleteAction);
        menu.addSeparator();
    }

    menu.addAction(tr("Yeni Klasör..."), this, &SshFileManagerWidget::onRemoteCreateFolderAction);
    
    if (m_viewMode == Details) {
        menu.exec(m_remoteFileTable->viewport()->mapToGlobal(pos));
    } else {
        menu.exec(m_remoteIconList->viewport()->mapToGlobal(pos));
    }
}

void SshFileManagerWidget::onRemoteCreateFolderAction() {
    bool ok;
    QString folderName = QInputDialog::getText(this, tr("Yeni Klasör"), tr("Klasör Adı:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || folderName.trimmed().isEmpty()) return;

    QString baseDir = OkidiaUtils::fromRemotePathSeparators(m_remoteCurrentPath);
    QString newDirPath = baseDir.endsWith("/") ? (baseDir + folderName.trimmed()) : (baseDir + "/" + folderName.trimmed());

    m_progressBar->setVisible(true);
    m_progressLbl->setText(tr("Yeni Klasör yapılıyor..."));
    m_progressBar->setValue(0);
    setUiEnabled(false);

    std::ignore = QtConcurrent::run([this, newDirPath]() {
        // 1. Önce doğrudan SFTP mkdir dene
        if (m_client->createDirectory(newDirPath)) {
            QMetaObject::invokeMethod(this, [this]() {
                m_progressBar->setVisible(false);
                m_progressLbl->clear();
                setUiEnabled(true);
                loadRemoteDirectory(m_remoteCurrentPath);
            }, Qt::QueuedConnection);
            return;
        }

        // 2. SFTP başarısız olursa kabuk komutuna (ve Linux'ta sudo'ya) düş
        QMetaObject::invokeMethod(this, [this, newDirPath]() {
            QString cmd = OkidiaUtils::remoteMkdirCmd(newDirPath, m_isRemoteWindows);
            QString sudoTpl = m_isRemoteWindows ? "" : QString("echo '%%1' | sudo -S %1").arg(cmd);
            runRemoteCommandWithSudo(cmd, sudoTpl, tr("Yeni Klasör"));
        }, Qt::QueuedConnection);
    });
}

void SshFileManagerWidget::onRemoteRenameAction() {
    QString oldName;
    if (m_viewMode == Details) {
        int r = m_remoteFileTable->currentRow();
        if (r < 0) return;
        oldName = m_remoteFileTable->item(r, 0)->text();
    } else {
        auto items = m_remoteIconList->selectedItems();
        if (items.isEmpty()) return;
        oldName = items.first()->text();
    }

    bool ok;
    QString newName = QInputDialog::getText(this, tr("Yeniden Adlandır"), tr("Yeni İsim:"), QLineEdit::Normal, oldName, &ok);
    if (!ok || newName.trimmed().isEmpty() || newName.trimmed() == oldName) return;

    QString baseDir = OkidiaUtils::fromRemotePathSeparators(m_remoteCurrentPath);
    QString oldPath = baseDir.endsWith("/") ? (baseDir + oldName) : (baseDir + "/" + oldName);
    QString newPath = baseDir.endsWith("/") ? (baseDir + newName.trimmed()) : (baseDir + "/" + newName.trimmed());

    m_progressBar->setVisible(true);
    m_progressLbl->setText(tr("Yeniden Adlandır yapılıyor..."));
    m_progressBar->setValue(0);
    setUiEnabled(false);

    std::ignore = QtConcurrent::run([this, oldPath, newPath]() {
        // 1. Önce doğrudan SFTP rename dene
        if (m_client->renameRemoteFile(oldPath, newPath)) {
            QMetaObject::invokeMethod(this, [this]() {
                m_progressBar->setVisible(false);
                m_progressLbl->clear();
                setUiEnabled(true);
                loadRemoteDirectory(m_remoteCurrentPath);
            }, Qt::QueuedConnection);
            return;
        }

        // 2. SFTP başarısız olursa kabuk komutuna düş
        QMetaObject::invokeMethod(this, [this, oldPath, newPath]() {
            QString cmd = OkidiaUtils::remoteMoveCmd(oldPath, newPath, m_isRemoteWindows);
            QString sudoTpl = m_isRemoteWindows ? "" : QString("echo '%%1' | sudo -S %1").arg(cmd);
            runRemoteCommandWithSudo(cmd, sudoTpl, tr("Yeniden Adlandır"));
        }, Qt::QueuedConnection);
    });
}

void SshFileManagerWidget::onRemoteDeleteAction() {
    struct DeleteItem {
        QString path;
        bool isDir;
    };
    QList<DeleteItem> itemsToDelete;
    QString baseDir = OkidiaUtils::fromRemotePathSeparators(m_remoteCurrentPath);

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
            QString path = baseDir.endsWith("/") ? (baseDir + name) : (baseDir + "/" + name);
            itemsToDelete.append({path, isDir});
        }
    } else {
        auto selectedItems = m_remoteIconList->selectedItems();
        for (auto* item : selectedItems) {
            QString name = item->text();
            bool isDir = item->data(Qt::UserRole + 2).toBool();
            QString path = baseDir.endsWith("/") ? (baseDir + name) : (baseDir + "/" + name);
            itemsToDelete.append({path, isDir});
        }
    }

    if (itemsToDelete.isEmpty()) return;

    auto res = QMessageBox::question(this, tr("Sil"), QString(tr("%1 adet öğeyi silmek istediğinizden emin misiniz?")).arg(itemsToDelete.size()),
                                     QMessageBox::Yes | QMessageBox::No);
    if (res != QMessageBox::Yes) return;

    m_progressBar->setVisible(true);
    m_progressLbl->setText(tr("Sil yapılıyor..."));
    m_progressBar->setValue(0);
    setUiEnabled(false);

    std::ignore = QtConcurrent::run([this, itemsToDelete]() {
        QStringList fallbackCmds;
        for (const auto& item : itemsToDelete) {
            bool sftpDeleted = item.isDir
                ? m_client->removeRemoteDirectory(item.path)
                : m_client->removeRemoteFile(item.path);
            if (!sftpDeleted) {
                fallbackCmds.append(OkidiaUtils::remoteDeleteCmd(item.path, item.isDir, m_isRemoteWindows));
            }
        }

        if (fallbackCmds.isEmpty()) {
            QMetaObject::invokeMethod(this, [this]() {
                m_progressBar->setVisible(false);
                m_progressLbl->clear();
                setUiEnabled(true);
                loadRemoteDirectory(m_remoteCurrentPath);
            }, Qt::QueuedConnection);
            return;
        }

        QMetaObject::invokeMethod(this, [this, fallbackCmds]() {
            QString sep = m_isRemoteWindows ? " & " : " && ";
            QString fullCmd = fallbackCmds.join(sep);
            QString escapedForSudo = fullCmd;
            escapedForSudo.replace("'", "'\\''");
            QString sudoTpl = m_isRemoteWindows ? "" : QString("echo '%%1' | sudo -S sh -c '%2'").arg(escapedForSudo);
            runRemoteCommandWithSudo(fullCmd, sudoTpl, tr("Sil"));
        }, Qt::QueuedConnection);
    });
}

void SshFileManagerWidget::runRemoteCommandWithSudo(const QString& normalCmd, const QString& sudoCmdTpl, const QString& opName) {
    m_progressBar->setVisible(true);
    m_progressLbl->setText(opName + tr(" yapılıyor..."));
    m_progressBar->setValue(0);
    setUiEnabled(false);

    std::ignore = QtConcurrent::run([this, normalCmd, sudoCmdTpl, opName]() {
        int exitCode = -1;
        QString out = m_client->executeSync(normalCmd, 8000, &exitCode);
        
        if (exitCode == 0) {
            QMetaObject::invokeMethod(this, [this]() {
                m_progressBar->setVisible(false);
                m_progressLbl->clear();
                setUiEnabled(true);
                loadRemoteDirectory(m_remoteCurrentPath);
            }, Qt::QueuedConnection);
            return;
        }

        if (sudoCmdTpl.isEmpty()) {
            QMetaObject::invokeMethod(this, [this, opName, out]() {
                m_progressBar->setVisible(false);
                m_progressLbl->clear();
                setUiEnabled(true);
                QMessageBox::critical(this, tr("Hata"), opName + tr(" işlemi başarısız: ") + out.trimmed());
            }, Qt::QueuedConnection);
            return;
        }

        QString pw;
        QMetaObject::invokeMethod(this, [this, &pw]() {
            pw = m_sudoPassword;
        }, Qt::BlockingQueuedConnection);

        if (pw.isEmpty()) {
            bool userOk = false;
            QMetaObject::invokeMethod(this, [this, opName, &pw, &userOk]() {
                pw = QInputDialog::getText(this, tr("Yönetici Parolası"), 
                    QString(tr("%1 işlemi başarısız oldu. Yönetici parolası ile tekrar denenecek:\n(Parola oturum boyunca saklanacaktır)")).arg(opName), 
                    QLineEdit::Password, "", &userOk);
            }, Qt::BlockingQueuedConnection);
            
            if (!userOk || pw.isEmpty()) {
                QMetaObject::invokeMethod(this, [this, opName, out]() {
                    m_progressBar->setVisible(false);
                    m_progressLbl->clear();
                    setUiEnabled(true);
                    QMessageBox::critical(this, tr("Hata"), opName + tr(" işlemi başarısız: ") + out.trimmed());
                }, Qt::QueuedConnection);
                return;
            }
        }

        QString sudoCmd = sudoCmdTpl.arg(pw);
        exitCode = -1;
        out = m_client->executeSync(sudoCmd, 8000, &exitCode);
        bool finalOk = (exitCode == 0);
        QString errStr = out.trimmed();
        if (!finalOk && errStr.isEmpty()) errStr = tr("Yanlış parola veya yetersiz izin");

        QMetaObject::invokeMethod(this, [this, finalOk, errStr, opName, pw]() {
            m_progressBar->setVisible(false);
            m_progressLbl->clear();
            setUiEnabled(true);
            if (finalOk) {
                m_sudoPassword = pw;
                loadRemoteDirectory(m_remoteCurrentPath);
            } else {
                m_sudoPassword.clear();
                QMessageBox::critical(this, tr("Hata"), opName + tr(" sudo işlemi başarısız: ") + errStr);
            }
        }, Qt::QueuedConnection);
    });
}

void SshFileManagerWidget::onRemotePrevClicked() {
    if (m_remoteHistoryIdx > 0) {
        m_remoteHistoryIdx--;
        m_remoteHistorySkip = true;
        loadRemoteDirectory(m_remoteHistory[m_remoteHistoryIdx]);
    }
}

void SshFileManagerWidget::onRemoteNextClicked() {
    if (m_remoteHistoryIdx >= 0 && m_remoteHistoryIdx < m_remoteHistory.size() - 1) {
        m_remoteHistoryIdx++;
        m_remoteHistorySkip = true;
        loadRemoteDirectory(m_remoteHistory[m_remoteHistoryIdx]);
    }
}

void SshFileManagerWidget::onRemoteHomeClicked() {
    loadRemoteDirectory(m_remoteHomePath.isEmpty() ? (m_isRemoteWindows ? "C:/Users" : "/home") : m_remoteHomePath);
}

void SshFileManagerWidget::onRemoteRootClicked() {
    loadRemoteDirectory(m_remoteRootPath.isEmpty() ? (m_isRemoteWindows ? tr("Bilgisayarım") : "/") : m_remoteRootPath);
}

void SshFileManagerWidget::downloadAndOpenRemoteFile(const QString& remotePath, const QString& fileName, bool force) {
    if (!isConnected()) return;

    QSettings settings("okidia", "master");
    bool autoOpen = settings.value("ssh/autoOpenDownloadedFiles", false).toBool();

    if (!force && !autoOpen) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Dosya Aç"));
        msgBox.setText(QString(tr("'%1' dosyası geçici klasöre indirilip varsayılan uygulama ile açılacak.\n\nDevam etmek istiyor musunuz?")).arg(fileName));
        msgBox.setIcon(QMessageBox::Question);
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        msgBox.setDefaultButton(QMessageBox::Yes);

        QCheckBox* rememberBox = new QCheckBox(tr("Bu seçimi hatırla ve bir daha sorma"), &msgBox);
        msgBox.setCheckBox(rememberBox);

        if (msgBox.exec() != QMessageBox::Yes) {
            return;
        }

        if (rememberBox->isChecked()) {
            settings.setValue("ssh/autoOpenDownloadedFiles", true);
        }
    }

    QString tempDir = QDir::tempPath() + "/okidia_preview";
    QDir().mkpath(tempDir);
    QString localTarget = tempDir + "/" + fileName;

    m_progressBar->setVisible(true);
    m_progressLbl->setText(QString(tr("İndiriliyor: %1")).arg(fileName));
    m_progressBar->setValue(0);
    setUiEnabled(false);

    std::ignore = QtConcurrent::run([this, remotePath, localTarget, fileName]() {
        bool ok = m_client->downloadFile(remotePath, localTarget, [this](qint64 s, qint64 total) {
            QMetaObject::invokeMethod(this, [this, s, total]() {
                updateProgress(s, total, tr("Önizleme İndiriliyor"));
            }, Qt::QueuedConnection);
        });

        QString lastErr = ok ? "" : m_client->lastError();

        QMetaObject::invokeMethod(this, [this, ok, localTarget, fileName, lastErr]() {
            m_progressBar->setVisible(false);
            m_progressLbl->clear();
            setUiEnabled(true);

            if (ok) {
                bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(localTarget));
                if (!opened) {
                    QMessageBox::warning(this, tr("Dosya Açılamadı"),
                                         QString(tr("'%1' dosyası indirildi ancak varsayılan uygulama ile açılamadı.\nKonum: %2")).arg(fileName, localTarget));
                }
            } else {
                QMessageBox::critical(this, tr("Hata"), QString(tr("'%1' dosyası indirilemedi:\n%2")).arg(fileName, lastErr));
            }
        }, Qt::QueuedConnection);
    });
}
