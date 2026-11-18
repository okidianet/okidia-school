// master/ssh/SshFileManagerLocal.cpp
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
#include <QtConcurrent>
#include <QStyle>
#include <algorithm>

// ----------------------------------------------------------------------------
// LOCAL SLOTS & FUNCTIONS
// ----------------------------------------------------------------------------

void SshFileManagerWidget::onLocalUpClicked() {
    QString current = QDir::fromNativeSeparators(m_localCurrentPath).trimmed();
    if (current.isEmpty()) return;

    if (m_isLocalWindows) {
        if (current == tr("Bilgisayarım") || current == "Bilgisayarım") {
            return;
        }
        if (current.size() <= 3 && current.contains(":")) {
            loadLocalDirectory(tr("Bilgisayarım"));
            return;
        }
        QDir dir(current);
        if (dir.cdUp()) {
            QString upPath = QDir::fromNativeSeparators(dir.absolutePath());
            if (upPath == current || upPath.isEmpty()) {
                loadLocalDirectory(tr("Bilgisayarım"));
            } else {
                loadLocalDirectory(upPath);
            }
        } else {
            loadLocalDirectory(tr("Bilgisayarım"));
        }
    } else {
        if (current == "/") return;
        QDir dir(current);
        if (dir.cdUp()) {
            loadLocalDirectory(dir.absolutePath());
        }
    }
}

void SshFileManagerWidget::onLocalRefreshClicked() {
    loadLocalDirectory(m_localCurrentPath);
}

void SshFileManagerWidget::loadLocalDirectory(const QString& path) {
    if (m_loadingLocal) return;

    QString cleanPath = QDir::fromNativeSeparators(path.trimmed());
    if (m_isLocalWindows) {
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

    if (!m_localHistorySkip) {
        if (m_localHistoryIdx >= 0 && m_localHistoryIdx < m_localHistory.size() - 1) {
            m_localHistory.erase(m_localHistory.begin() + m_localHistoryIdx + 1, m_localHistory.end());
        }
        if (m_localHistory.isEmpty() || m_localHistory.last() != cleanPath) {
            m_localHistory.append(cleanPath);
            m_localHistoryIdx = m_localHistory.size() - 1;
        }
    }
    m_localHistorySkip = false;

    m_loadingLocal = true;
    m_progressLbl->setText(tr("Yerel klasör yükleniyor..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);

    bool showHidden = m_localShowHidden->isChecked();

    std::ignore = QtConcurrent::run([this, cleanPath, showHidden]() {
        QList<FileInfo> filesList;

        if (m_isLocalWindows && (cleanPath == tr("Bilgisayarım") || cleanPath == "Bilgisayarım")) {
            const auto drives = QDir::drives();
            for (const auto& d : drives) {
                FileInfo info;
                info.name = QDir::fromNativeSeparators(d.absoluteFilePath());
                if (!info.name.endsWith("/")) info.name += "/";
                info.size = 0;
                info.isDir = true;
                info.permissions = "-";
                filesList.append(info);
            }
        } else {
            QDir dir(cleanPath);
            QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
            if (showHidden) filters |= QDir::Hidden;
            QFileInfoList list = dir.entryInfoList(filters, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);

            for (const QFileInfo& fi : list) {
                FileInfo info;
                info.name = fi.fileName();
                info.size = fi.size();
                info.isDir = fi.isDir();
                
                QFile::Permissions perms = fi.permissions();
                int octal = 0;
                if (perms & QFile::ReadUser)   octal |= 0400;
                if (perms & QFile::WriteUser)  octal |= 0200;
                if (perms & QFile::ExeUser)    octal |= 0100;
                if (perms & QFile::ReadGroup)  octal |= 0040;
                if (perms & QFile::WriteGroup) octal |= 0020;
                if (perms & QFile::ExeGroup)   octal |= 0010;
                if (perms & QFile::ReadOther)  octal |= 0004;
                if (perms & QFile::WriteOther) octal |= 0002;
                if (perms & QFile::ExeOther)   octal |= 0001;

                info.permissions = QString("%1").arg(octal, 3, 8, QChar('0'));
                info.mtime = fi.lastModified();
                filesList.append(info);
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
            m_localCurrentPath = cleanPath;
            if (!m_syncingFromOtherTab) {
                OkidiaUtils::setLastDirectory("ssh_sftp_local", cleanPath);
                emit localPathChanged(cleanPath);
            }
            m_syncingFromOtherTab = false;
            m_localFiles = filesList;
            renderLocalFiles();
            m_progressLbl->clear();
            QApplication::restoreOverrideCursor();
            m_loadingLocal = false;
        }, Qt::QueuedConnection);
    });
}

void SshFileManagerWidget::renderLocalFiles() {
    m_localFileTable->setSortingEnabled(false);
    m_localFileTable->setRowCount(0);
    m_localIconList->clear();
    m_localPathEdit->setText(m_localCurrentPath == tr("Bilgisayarım") ? tr("Bilgisayarım") : QDir::toNativeSeparators(m_localCurrentPath));

    for (const auto& file : m_localFiles) {
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
            int r = m_localFileTable->rowCount();
            m_localFileTable->insertRow(r);

            auto* nameItem = new OkidiaTableItem(file.name);
            nameItem->setIcon(icon);
            nameItem->setData(Qt::UserRole, file.isDir ? 0 : 1);
            nameItem->setData(Qt::UserRole + 2, file.isDir);
            m_localFileTable->setItem(r, 0, nameItem);

            QString sizeStr = "-";
            if (!file.isDir) {
                sizeStr = OkidiaUtils::formatSize(file.size);
            }
            auto* sizeItem = new OkidiaTableItem(sizeStr);
            sizeItem->setData(Qt::UserRole, file.isDir ? 0 : 1);
            sizeItem->setData(Qt::UserRole + 1, file.isDir ? (qulonglong)0 : (qulonglong)file.size);
            sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_localFileTable->setItem(r, 1, sizeItem);

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
            m_localFileTable->setItem(r, 2, typeItem);

            auto* permItem = new OkidiaTableItem(file.permissions.isEmpty() ? "-" : file.permissions);
            permItem->setData(Qt::UserRole, file.isDir ? 0 : 1);
            permItem->setTextAlignment(Qt::AlignCenter);
            m_localFileTable->setItem(r, 3, permItem);

            QString dateStr = file.mtime.isValid() ? file.mtime.toString("yyyy-MM-dd HH:mm:ss") : "-";
            auto* mtimeItem = new OkidiaTableItem(dateStr);
            mtimeItem->setData(Qt::UserRole, file.isDir ? 0 : 1);
            mtimeItem->setData(Qt::UserRole + 1, file.mtime.isValid() ? (qulonglong)file.mtime.toSecsSinceEpoch() : (qulonglong)0);
            mtimeItem->setTextAlignment(Qt::AlignCenter);
            m_localFileTable->setItem(r, 4, mtimeItem);
        } else {
            auto* item = new QListWidgetItem(icon, file.name, m_localIconList);
            item->setData(Qt::UserRole, file.isDir ? 0 : 1);
            item->setData(Qt::UserRole + 2, file.isDir);
            item->setToolTip(file.name);
        }
    }

    if (m_viewMode == Details) {
        int sortCol = m_localFileTable->horizontalHeader()->sortIndicatorSection();
        Qt::SortOrder sortOrd = m_localFileTable->horizontalHeader()->sortIndicatorOrder();
        if (sortCol < 0) {
            sortCol = 4; // Değiştirilme Tarihi
            sortOrd = Qt::DescendingOrder;
        }
        m_localFileTable->setSortingEnabled(true);
        m_localFileTable->sortByColumn(sortCol, sortOrd);
        m_localFileTable->horizontalHeader()->setSortIndicator(sortCol, sortOrd);
    }
}

void SshFileManagerWidget::onLocalTableDoubleClicked(int row, int column) {
    Q_UNUSED(column)
    auto* item = m_localFileTable->item(row, 0);
    if (!item) return;

    bool isDir = (item->data(Qt::UserRole).toInt() == 0);
    if (isDir) {
        QString dirName = item->text();
        if (m_isLocalWindows && (m_localCurrentPath == tr("Bilgisayarım") || m_localCurrentPath == "Bilgisayarım")) {
            loadLocalDirectory(dirName);
        } else {
            QDir dir(m_localCurrentPath);
            if (dir.cd(dirName)) {
                loadLocalDirectory(dir.absolutePath());
            }
        }
    } else {
        QString fileName = item->text();
        QString fullPath = QDir(m_localCurrentPath).filePath(fileName);
        openLocalFile(fullPath);
    }
}

void SshFileManagerWidget::onLocalItemDoubleClicked(QListWidgetItem* item) {
    if (!item) return;
    bool isDir = (item->data(Qt::UserRole).toInt() == 0);
    if (isDir) {
        QString dirName = item->text();
        if (m_isLocalWindows && (m_localCurrentPath == tr("Bilgisayarım") || m_localCurrentPath == "Bilgisayarım")) {
            loadLocalDirectory(dirName);
        } else {
            QDir dir(m_localCurrentPath);
            if (dir.cd(dirName)) {
                loadLocalDirectory(dir.absolutePath());
            }
        }
    } else {
        QString fileName = item->text();
        QString fullPath = QDir(m_localCurrentPath).filePath(fileName);
        openLocalFile(fullPath);
    }
}

void SshFileManagerWidget::showLocalContextMenu(const QPoint& pos) {
    QMenu menu(this);
    bool hasSelection = false;
    bool isDir = false;
    QString selectedName;

    if (m_viewMode == Details) {
        QTableWidgetItem* item = m_localFileTable->itemAt(pos);
        if (item) {
            hasSelection = true;
            int r = item->row();
            selectedName = m_localFileTable->item(r, 0)->text();
            isDir = (m_localFileTable->item(r, 0)->data(Qt::UserRole).toInt() == 0);
            if (isDir) {
                menu.addAction(tr("Aç"), this, [this, r]() { onLocalTableDoubleClicked(r, 0); });
            } else {
                QString fullPath = QDir(m_localCurrentPath).filePath(selectedName);
                menu.addAction(tr("Aç"), this, [this, fullPath]() { openLocalFile(fullPath); });
                menu.addAction(tr("Yükle"), this, &SshFileManagerWidget::onTransferToRemoteClicked);
            }
        }
    } else {
        QListWidgetItem* item = m_localIconList->itemAt(pos);
        if (item) {
            hasSelection = true;
            selectedName = item->text();
            isDir = (item->data(Qt::UserRole).toInt() == 0);
            if (isDir) {
                menu.addAction(tr("Aç"), this, [this, item]() { onLocalItemDoubleClicked(item); });
            } else {
                QString fullPath = QDir(m_localCurrentPath).filePath(selectedName);
                menu.addAction(tr("Aç"), this, [this, fullPath]() { openLocalFile(fullPath); });
                menu.addAction(tr("Yükle"), this, &SshFileManagerWidget::onTransferToRemoteClicked);
            }
        }
    }

    if (hasSelection) {
        menu.addAction(tr("Yeniden Adlandır..."), this, &SshFileManagerWidget::onLocalRenameAction);
        menu.addAction(tr("Sil"), this, &SshFileManagerWidget::onLocalDeleteAction);
        menu.addSeparator();
    }

    menu.addAction(tr("Yeni Klasör..."), this, &SshFileManagerWidget::onLocalCreateFolderAction);
    
    if (m_viewMode == Details) {
        menu.exec(m_localFileTable->viewport()->mapToGlobal(pos));
    } else {
        menu.exec(m_localIconList->viewport()->mapToGlobal(pos));
    }
}

void SshFileManagerWidget::onLocalCreateFolderAction() {
    bool ok;
    QString folderName = QInputDialog::getText(this, tr("Yeni Klasör"), tr("Klasör Adı:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || folderName.trimmed().isEmpty()) return;

    QDir dir(m_localCurrentPath);
    dir.mkdir(folderName.trimmed());
    loadLocalDirectory(m_localCurrentPath);
}

void SshFileManagerWidget::onLocalRenameAction() {
    QString oldName;
    if (m_viewMode == Details) {
        int r = m_localFileTable->currentRow();
        if (r < 0) return;
        oldName = m_localFileTable->item(r, 0)->text();
    } else {
        auto items = m_localIconList->selectedItems();
        if (items.isEmpty()) return;
        oldName = items.first()->text();
    }

    bool ok;
    QString newName = QInputDialog::getText(this, tr("Yeniden Adlandır"), tr("Yeni İsim:"), QLineEdit::Normal, oldName, &ok);
    if (!ok || newName.trimmed().isEmpty() || newName.trimmed() == oldName) return;

    QDir dir(m_localCurrentPath);
    dir.rename(oldName, newName.trimmed());
    loadLocalDirectory(m_localCurrentPath);
}

void SshFileManagerWidget::onLocalDeleteAction() {
    QList<QPair<QString, bool>> toDelete; // name, isDir

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
            toDelete.append({name, isDir});
        }
    } else {
        auto selectedItems = m_localIconList->selectedItems();
        for (auto* item : selectedItems) {
            QString name = item->text();
            bool isDir = item->data(Qt::UserRole + 2).toBool();
            toDelete.append({name, isDir});
        }
    }

    if (toDelete.isEmpty()) return;

    auto res = QMessageBox::question(this, tr("Sil"), QString(tr("%1 adet öğeyi silmek istediğinizden emin misiniz?")).arg(toDelete.size()),
                                     QMessageBox::Yes | QMessageBox::No);
    if (res != QMessageBox::Yes) return;

    QDir dir(m_localCurrentPath);
    for (const auto& item : toDelete) {
        QString fullPath = dir.filePath(item.first);
        if (item.second) { // isDir
            QDir sub(fullPath);
            if (!sub.removeRecursively()) {
                Logger::warning(QObject::tr("SshFileManager: Klasör silinemedi: ") + fullPath);
            }
        } else {
            QFile f(fullPath);
            if (!f.isWritable()) {
                f.setPermissions(f.permissions() | QFile::WriteOwner | QFile::WriteUser);
            }
            if (!f.remove()) {
                if (!dir.remove(item.first)) {
                    Logger::warning(QObject::tr("SshFileManager: Dosya silinemedi: ") + fullPath);
                }
            }
        }
    }
    
    loadLocalDirectory(m_localCurrentPath);
}

void SshFileManagerWidget::setLocalPath(const QString& path) {
    if (m_localCurrentPath != path) {
        m_syncingFromOtherTab = true;
        loadLocalDirectory(path);
    }
}

void SshFileManagerWidget::onLocalPrevClicked() {
    if (m_localHistoryIdx > 0) {
        m_localHistoryIdx--;
        m_localHistorySkip = true;
        loadLocalDirectory(m_localHistory[m_localHistoryIdx]);
    }
}

void SshFileManagerWidget::onLocalNextClicked() {
    if (m_localHistoryIdx >= 0 && m_localHistoryIdx < m_localHistory.size() - 1) {
        m_localHistoryIdx++;
        m_localHistorySkip = true;
        loadLocalDirectory(m_localHistory[m_localHistoryIdx]);
    }
}

void SshFileManagerWidget::onLocalHomeClicked() {
    loadLocalDirectory(QDir::homePath());
}

void SshFileManagerWidget::onLocalRootClicked() {
    loadLocalDirectory(m_isLocalWindows ? tr("Bilgisayarım") : "/");
}

void SshFileManagerWidget::openLocalFile(const QString& fullPath) {
    if (fullPath.isEmpty() || !QFile::exists(fullPath)) return;
    bool ok = QDesktopServices::openUrl(QUrl::fromLocalFile(fullPath));
    if (!ok) {
        QMessageBox::warning(this, tr("Dosya Açılamadı"),
                             QString(tr("'%1' dosyası varsayılan uygulama ile açılamadı.")).arg(QFileInfo(fullPath).fileName()));
    }
}
