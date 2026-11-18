#!/bin/bash
# okidia-agent-recover.sh
# OKIDIA Agent servisinin çökme döngüsüne girmesi halinde çalışan otonom kurtarma ve geri alma (Zero-Touch Self-Healing) betiği.

set -e

LOG_FILE="/var/log/okidia/okidia_recover.log"
mkdir -p /var/log/okidia 2>/dev/null || true

log() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] [RECOVER] $1"
    echo "$msg"
    echo "$msg" >> "$LOG_FILE" 2>/dev/null || true
}

log "okidia-agent.service çöküşü tespit edildi. Kurtarma süreci başlatılıyor..."

ROLLBACK_DEB="/var/lib/okidia/rollback/okidia-agent-stable.deb"
LOCK_FILE="/var/run/okidia_rollback_in_progress"

# Sonsuz döngü koruması: Son 5 dakika içinde zaten kurtarma denenmişse tekrar tekrar deneme
if [ -f "$LOCK_FILE" ]; then
    LAST_TRY=$(stat -c %Y "$LOCK_FILE" 2>/dev/null || echo 0)
    NOW=$(date +%s)
    DIFF=$((NOW - LAST_TRY))
    if [ "$DIFF" -lt 300 ]; then
        log "UYARI: Son 5 dakika ($DIFF sn) içinde zaten kurtarma işlemi denenmiş! Sonsuz döngüyü önlemek için durduruluyor."
        exit 0
    fi
fi

touch "$LOCK_FILE"

# 1. Kilitli veya asılı süreçleri temizle
log "Mevcut asılı süreçler ve soketler temizleniyor..."
pkill -9 -x okidia-agent 2>/dev/null || true
pkill -9 -x okidia-tray 2>/dev/null || true
sleep 1

# 2. Rollback paketi mevcut mu?
if [ -f "$ROLLBACK_DEB" ]; then
    log "Yerel kararlı geri alma paketi bulundu: $ROLLBACK_DEB"
    log "Kararlı sürüm sessizce geri yükleniyor (dpkg -i)..."

    export DEBIAN_FRONTEND=noninteractive
    if dpkg -i "$ROLLBACK_DEB" >> "$LOG_FILE" 2>&1; then
        log "Geri alma paketi başarıyla kuruldu."
    else
        log "UYARI: dpkg kurulumu hata verdi, kırık bağımlılıklar onarılıyor (apt-get install -f)..."
        apt-get install -f -y >> "$LOG_FILE" 2>&1 || true
        dpkg -i "$ROLLBACK_DEB" >> "$LOG_FILE" 2>&1 || true
    fi
else
    log "BİLGİ: Yerel geri alma paketi bulunamadı ($ROLLBACK_DEB). Sistemdeki mevcut paket konfigürasyonu onarılıyor..."
    export DEBIAN_FRONTEND=noninteractive
    apt-get install -f -y >> "$LOG_FILE" 2>&1 || true
    dpkg --configure -a >> "$LOG_FILE" 2>&1 || true
fi

# 3. systemd servis durumunu sıfırla ve servisi yeniden başlat
log "systemd okidia-agent.service durumu sıfırlanıyor ve başlatılıyor..."
systemctl reset-failed okidia-agent.service || true
sleep 2

if systemctl start okidia-agent.service; then
    log "BAŞARILI: okidia-agent.service kurtarıldı ve başarıyla başlatıldı."
else
    log "HATA: okidia-agent.service başlatılamadı!"
fi

rm -f "$LOCK_FILE"
exit 0
