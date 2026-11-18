# 📂 OKIDIA Kod Tabanı Kataloğu (Codebase Catalog)

Bu döküman; Okidia projesindeki kaynak kod dosyalarını, C++ sınıflarını ve dizin yapılarını işlevsel sorumluluklarına göre kataloglayarak yeni geliştiricilerin sistemi hızlıca anlamasını ve doğru dosyalarda değişiklik yapmasını sağlar.

---

## 1. Ortak Bileşenler (`common/`)

Tüm platform modülleri (Sunucu, İstemci ve Yönetici) tarafından paylaşılan temel kütüphaneler, veri yapıları ve arayüz bileşenleri bu dizindedir.

### 1.1 `common/db/` (Veri Depolama)
*   [DbRecords.h](../common/db/DbRecords.h): Veritabanındaki satırları modelleyen ve ağda taşınan C++ yapıları (structs) (`AgentRecord`, `UserRecord`, `ChatMessageRecord`, `AuditRecord`, `TeacherRecord`, `SshCredentialRecord` vb.).
*   [OkidiaDatabase.h](../common/db/OkidiaDatabase.h) / [.cpp](../common/db/OkidiaDatabase.cpp): SQLite veritabanı işlemlerini, veri yazma/okuma sorgularını ve şema yönetimini yürüten thread-safe Singleton sınıfı. Yalnızca operasyonel verileri (`okidia.db`: kullanıcılar, sınıflar, ajanlar, tokenlar, join kodları, sohbet kilitleri ve yerel SSH kimlik kasası) yönetir. Master, Agent ve Broker tarafından paylaşılır.
*   [OkidiaAuditDatabase.h](../common/db/OkidiaAuditDatabase.h) / [.cpp](../common/db/OkidiaAuditDatabase.cpp): Yalnızca Broker üzerinde (`okidia_audit.db`) çalışan adli denetim izi (audit log) veritabanı Singleton sınıfı. SQLite seviyesinde WORM (Write Once Read Many) tetikleyicileri ile log silinmesini/değiştirilmesini engeller, dinamik retention ve haftalık dönen `.db.gz` arşivlemesini yönetir. KVKK/5651 gereği Master veya Agent üzerinde asla başlatılmaz veya depolanmaz.
*   [OkidiaChatDatabase.h](../common/db/OkidiaChatDatabase.h) / [.cpp](../common/db/OkidiaChatDatabase.cpp): Yalnızca Broker üzerinde (`okidia_chat.db`) çalışan sohbet odaları ve mesaj geçmişi veritabanı Singleton sınıfı. Dinamik retention (saklama süresi), otomatik temizlik ve haftalık dönen `.db.gz` sıkıştırılmış arşivleme mekanizmalarını yönetir. KVKK veri minimizasyonu gereği yalnızca sunucuda tutulur.

### 1.2 `common/crypto/` (Kriptografi ve Güvenlik)
*   [KeyManager.h](../common/crypto/KeyManager.h) / [.cpp](../common/crypto/KeyManager.cpp): OpenSSL tabanlı RSA/Ed25519 anahtar çifti üretimi, X.509 CSR üretimi, challenge imzalama ve doğrulama, sertifika geçerlilik süresi tespiti (`notBefore`/`notAfter`), kalan gün takibi (`getCertificateDaysRemaining`), ECDH (X25519) anahtar türetme, CSR ile X.509 açık anahtar eşleştirme doğrulaması (`matchCsrWithCertificate`) ve AES-256-GCM şifreleme/çözme motoru.
*   [KeyManagerTest.h](../common/crypto/KeyManagerTest.h): OpenSSL anahtar üretimi, imzalama, challenge doğrulama ve parmak izi işlevlerini sınayan birim test paketi.
*   [AesGcm.h](../common/crypto/AesGcm.h) / [.cpp](../common/crypto/AesGcm.cpp): AES-256-GCM kimlik doğrulamalı simetrik şifreleme yardımcı sınıfı.
*   [BackupPackage.h](../common/crypto/BackupPackage.h) / [.cpp](../common/crypto/BackupPackage.cpp): Tek birleşik şifreli sistem yedeği (`.okbak`) paketi oluşturma, doğrulama ve çözme motoru. PBKDF2-SHA256 anahtar türetimi ve AES-256-GCM ile veritabanını (`okidia.db`) ve Kök CA/Broker güvenlik sertifikalarını tek dosyada şifreler.

### 1.3 `common/network/` (Ağ ve Güvenlik)
*   [Connection.h](../common/network/Connection.h) / [.cpp](../common/network/Connection.cpp): Protokol zarfını (`Envelope`) TCP/SSL üzerinden gönderip alan temel soket sarmalayıcısı. 25 MB soket tampon birikme taşma koruması (`MAX_BUFFER_SIZE`), 10 MB azami paket sınır denetimi, UI unblocking (`kMaxMessagesPerRead = 8` + `singleShot(0)`) ve `TCP_NODELAY` düşük gecikme optimizasyonu sağlar.
*   [TlsClient.h](../common/network/TlsClient.h) / [.cpp](../common/network/TlsClient.cpp): Çift taraflı mTLS şifrelemeyi, sertifika yüklemeyi ve Broker sunucusuyla TLS soket bağlantılarını yöneten istemci sınıfı. TLS 1.2+ zorunluluğu (`TlsV1_2OrLater`), süresi dolmuş veya geçersiz sertifikaların fatal kabul edilerek engellenmesi yeteneklerine sahiptir.
*   [TlsServer.h](../common/network/TlsServer.h) / [.cpp](../common/network/TlsServer.cpp): Broker tarafında güvenli TLS bağlantılarını kabul eden, azami 500 eşzamanlı soket sınırı (`MAX_CONCURRENT_CONNECTIONS`), 10 saniyelik TLS handshake zaman aşımı (`HANDSHAKE_TIMEOUT_MS`) ve IP başına hız sınırlayıcısı ile DoS korumalı sunucu soketi.
*   [LocalConnection.h](../common/network/LocalConnection.h) / [.cpp](../common/network/LocalConnection.cpp): Agent servisi ve Tray arayüzü arasında Unix Domain Socket (`agent.sock`) veya Windows Named Pipe üzerinden IPC (Süreçler arası İletişim) sağlayan soket ve sunucu (`LocalServer`) sınıfı.
*   [MdnsManager.h](../common/network/MdnsManager.h) / [.cpp](../common/network/MdnsManager.cpp): mDNS servisini yayınlayan (Broker) ve tarayıp bulan (Agent/Master) çoklu yayın (multicast) yöneticisi. Linux'ta Avahi, Windows'ta yerel Qt QUdpSocket (224.0.0.251:5353) motoru kullanır. Multihomed host (birden çok ağ kartı ve sonradan takılan USB adaptörleri algılama), isim çakışması koruması (collision recovery) ve CA SHA-256 parmak izi (`cafp`) TXT anons desteği içerir.

### 1.4 `common/protocol/` (Mesaj Tanımları)
*   [messages.proto](../common/protocol/messages.proto): Serileştirme ve ağ aktarımı için tüm mesaj yapılarını, paket tiplerini (enum), zaman senkronizasyonu (`TimeSyncRequest`/`Response`), telemetri (`ClientHardwareAlert`), talep tabanlı akış (`SCREEN_STREAM_PAUSE`, `METRICS_STREAM_START`/`STOP`) ve zarf taşıyıcısını barındıran Google Protobuf şema dosyası.

### 1.5 `common/ui/` (Ortak Arayüz ve Sohbet Bileşenleri)
*   [ThemeManager.h](../common/ui/ThemeManager.h) / [.cpp](../common/ui/ThemeManager.cpp): Açık ve koyu tema (Light/Dark QSS) yönetimini, SVG ikon renk/palet dinamik dönüşümünü (`ThemeManager::icon(...)`) ve tema geçiş sinyallerini yöneten merkezi tema yöneticisi. `resources/ui/` ve `resources/ui_dark/` dizinlerindeki SVG vektör setini (özellikle akış kontrolü için `pause.svg`, `play.svg` vb.) yönetir.
*   `common/ui/chat/`:
    *   [ChatMessage.h](../common/ui/chat/ChatMessage.h): Sohbet mesajı veri modeli.
    *   [ChatMessageDelegate.h](../common/ui/chat/ChatMessageDelegate.h) / [.cpp](../common/ui/chat/ChatMessageDelegate.cpp): Qt `QStyledItemDelegate` tabanlı, modern sohbet baloncuklarını (bubble layout, rol rozetleri, zaman damgası) çizen görünüm delegesi.
    *   [ChatModel.h](../common/ui/chat/ChatModel.h) / [.cpp](../common/ui/chat/ChatModel.cpp): Qt `QAbstractListModel` tabanlı reaktif sohbet mesaj listesi modeli.
    *   [ChatWidget.h](../common/ui/chat/ChatWidget.h) / [.cpp](../common/ui/chat/ChatWidget.cpp): Master ve Agent tarafından paylaşılan, mesaj listesi ve giriş alanını barındıran zengin sohbet bileşeni.

### 1.6 `common/utils/` (Yardımcı Araçlar)
*   [Logger.h](../common/utils/Logger.h) / [.cpp](../common/utils/Logger.cpp): Uygulama loglarını dosyaya (`okidia-<bileşen>.log`) ve konsola yazan, günlük tarih ve 10 MB boyuta göre parçalayan, 28 günden eski logları temizleyen, hata tekrar bastırma (error throttling) ve seviye filtreli eşzamanlı loglama motoru.
*   [PlatformService.h](../common/utils/PlatformService.h) / [.cpp](../common/utils/PlatformService.cpp): Linux (systemd `sd_notify`, POSIX sinyalleri, `clock_settime`, `executeElevated` yetki yükseltme) ve Windows (SCM servis yaşam döngüsü, Session 0 token ile kullanıcı oturumunda tepsi başlatma, `SetSystemTime`, UAC `runas` yetki yükseltme) yaşam döngüsünü soyutlayan merkezi platform servisi.
*   [OkidiaUtils.h](../common/utils/OkidiaUtils.h): Merkezi yardımcı fonksiyonlar (doğal sıralama `naturalCompare`, `nowSecs`, OS tespiti, UUID, SHA256, disk şifreleme ve Secure Boot denetimleri, dosya boyutu/tarih formatlama ve `installTranslator` i18n yükleyicisi).
*   [OkidiaConfig.h](../common/OkidiaConfig.h): Sistem ve çalışma zamanı dosya yollarını (`runDir`, `certDir`, `logDir`, `dbDir`, `assetDir`, `rollbackDir`) Linux ve Windows platformlarına göre sağlayan merkezi konfigürasyon başlığı.
*   [QrCodeGenerator.h](../common/utils/QrCodeGenerator.h) / [.cpp](../common/utils/QrCodeGenerator.cpp) & [qrcodegen.cpp](../common/utils/qrcodegen.cpp): Üçüncü parti bağımlılık olmaksızın QR Code matrisi hesaplayan ve `QPixmap` olarak rasterize eden vektörel QR görsel üretici motoru.

### 1.7 `common/version/` (İzole Sürüm Yönetimi)
*   [OkidiaVersion.h](../common/version/OkidiaVersion.h) / [.cpp](../common/version/OkidiaVersion.cpp): Uygulama ve paket sürüm bilgisini (`PROJECT_VERSION`) ve hedef işletim sistemi etiketini (`OS_TAG`) tek bir kaynak dosyada izole eden statik kütüphane (`okidia_version`). Git commit sayısı her arttığında yüzlerce C++ dosyasının yeniden derlenmesini engeller.

---

## 2. Sunucu Katmanı (`broker/`)

Tüm ağ trafiğini yönlendiren, yetkilendirmeyi denetleyen, verileri kalıcılaştıran ve Pub/Sub fan-out çoğaltması yapan arka plan servisidir.

*   [main.cpp](../broker/main.cpp): Broker uygulamasının giriş noktası. Linux'ta daemon, Windows'ta `PlatformService` ile `OkidiaBroker` Windows Servisi olarak çalışır. TCP/TLS soket sunucularını başlatır ve alt sistemleri koordine eder.
*   [main_tray.cpp](../broker/main_tray.cpp): `okidia-broker-tray` masaüstü sistem tepsisi (System Tray) uygulamasının giriş noktası.
*   [BrokerIpcServer.h](../broker/BrokerIpcServer.h) / [.cpp](../broker/BrokerIpcServer.cpp): Broker servisi ile `okidia-broker-tray` GUI uygulaması arasında yerel soket IPC (`broker.sock`) üzerinden haberleşmeyi sağlayan sunucu.
*   [ClientRegistry.h](../broker/ClientRegistry.h) / [.cpp](../broker/ClientRegistry.cpp): Ağdaki aktif Master ve Agent soketlerini, online/offline durumlarını, ekran ve telemetri abonelik listelerini (`m_screenSubscribers`, `m_metricsSubscribers`) tutan ve Master koptuğunda izleyicisi kalmayan ajanlara otomatik `SCREEN_STREAM_PAUSE` komutu üreten istemci kayıt defteri.
*   [CertSigner.h](../broker/CertSigner.h) / [.cpp](../broker/CertSigner.cpp): Ajan ve Master CSR isteklerini Kök CA anahtarıyla imzalayan OpenSSL tabanlı X.509 sertifika imzalama motoru.
*   [MulticastManager.h](../broker/MulticastManager.h) / [.cpp](../broker/MulticastManager.cpp): Tek bir paketi sınıf veya grup odasındaki tüm soketlere çoğaltarak dağıtan fan-out yayın mekanizması.
*   [BrokerTransferManager.h](../broker/BrokerTransferManager.h) / [.cpp](../broker/BrokerTransferManager.cpp): Master'dan gönderilen OTA paketlerini saklayan ve eski sürümlü ajanlara aktaran modül.
*   [BellScheduler.h](../broker/BellScheduler.h) / [.cpp](../broker/BellScheduler.cpp): Okulun ders giriş/çıkış saatlerini takip eden, yerel ses çalma komutu (`AUDIO_PLAY_LOCAL`) gönderen, teneffüs saatinde Tip 3 Akıllı Tahtalara nezaket süreli (`grace_period_sec = 30`) kilit komutu (`SCREEN_LOCK`) fırlatan, sabah ilk ders öncesi veya teneffüs/devre arası ortasında yeni açılan istemcileri nezaket süresiz (`grace_period_sec = 0`) anında kilitleyen (`reconcileClientBellState`), son çıkış zilinde Kurs/Etüt hibrit kilit politikasını uygulayan ve mesai bitiminde otomatik kapatma (`broadcastAutoShutdown`) yürüten dijital zil zamanlayıcısı.
*   [EnrollmentManager.h](../broker/EnrollmentManager.h) / [.cpp](../broker/EnrollmentManager.cpp): Ajanlardan gelen keşif (`DISCOVERY_REQUEST`), CSR imzalama ve kayıt (`ENROLL_REQUEST`) isteklerini tek bir merkezde yalıtılmış olarak işleyen zero-config kayıt yöneticisi.
*   [BrokerTray.h](../broker/BrokerTray.h) / [.cpp](../broker/BrokerTray.cpp): Broker sunucusunun durumunu, IP/Port bilgilerini ve ayarlar menüsünü görselleştiren sistem tepsisi kontrol arayüzü.
*   `handlers/` (Mesaj Yönlendirme ve İş Mantığı):
    *   [MessageHandler.h](../broker/handlers/MessageHandler.h) / [.cpp](../broker/handlers/MessageHandler.cpp): Gelen tüm protokol zarflarını (`Envelope`) karşılayan, yetkilendirmeyi denetleyen, ekran ve metrik paketlerini (`SCREEN_FRAME`, `SCREEN_TILE_UPDATE`, `SYSTEM_METRICS_REPORT`) izleyen Master'lara Pub/Sub Fan-Out ile çoğaltan ana yönlendirici.
    *   [AuthHandler.h](../broker/handlers/AuthHandler.h) / [.cpp](../broker/handlers/AuthHandler.cpp): Kurulum kodu doğrulama, Master oturum açma (`LOGIN_REQUEST`), IP hız sınırlama, hesap kilitleme, WORM güvenlik logları ve kullanıcı yönetimi işlemlerini yürüten kimlik doğrulama işleyicisi.
    *   [ClientHandler.h](../broker/handlers/ClientHandler.h) / [.cpp](../broker/handlers/ClientHandler.cpp): Ajan kimlik bilgisi (`CLIENT_INFO`), mDNS keşif reddetme/yoksayma, kara liste (`status = 2`), ajan tipi belirleme ve kaldırma parolası mesajlarını yöneten işleyici.
    *   [ClassHandler.h](../broker/handlers/ClassHandler.h) / [.cpp](../broker/handlers/ClassHandler.cpp): Sınıf ekleme, silme, adlandırma ve ajan atama işlemlerini yöneten sınıf işleyicisi.
    *   [ChatHandler.h](../broker/handlers/ChatHandler.h) / [.cpp](../broker/handlers/ChatHandler.cpp): Sınıf sohbet kilitleri, oda üyelik hareketleri, genel/özel sohbet mesajları ve sohbet geçmişi sorgularını yöneten işleyici.
    *   [SystemHandler.h](../broker/handlers/SystemHandler.h) / [.cpp](../broker/handlers/SystemHandler.cpp): OTA güncelleme transferleri, Broker ad/port değişimi, denetim kaydı ekleme/sorgulama, SSH kimlik kasası, veritabanı yedekleme ve uzaktan log izleme (`LOG_TAIL_*`) operasyonlarını yöneten sistem işleyicisi.
    *   [BrokerContext.h](../broker/handlers/BrokerContext.h): İşleyiciler arasında paylaşılan bağlam veri yapısı.

---

## 3. İstemci Katmanı (`agent/`)

Yönetilen bilgisayarlarda işletim sistemi seviyesinde çalışan modüldür.

### 3.1 Çekirdek Servis ve IPC
*   [main.cpp](../agent/main.cpp): `okidia-agent` sistem servisinin (Linux daemon / Windows Service) giriş noktası. Linux'ta root, Windows'ta `SYSTEM` altında Session 0'da çalışır.
*   `core/`:
    *   [AgentApp.h](../agent/core/AgentApp.h) / [.cpp](../agent/core/AgentApp.cpp): Sistem servisinin ana koordinatörü. Broker TLS bağlantısını, oturum değişimlerini (DM Tray ve kullanıcı oturumu geçişleri, canlı tray bekçisi / watchdog), `/run/okidia/ipc_token` kriptografik token yönetimini, talep tabanlı telemetri zamanlayıcısını (`m_metricsTimer`), otonom OTA paketlerini ve komut yönlendirmesini yürütür.
    *   [AgentIpcServer.h](../agent/core/AgentIpcServer.h) / [.cpp](../agent/core/AgentIpcServer.cpp): Masaüstü arayüzü (`okidia-tray`) ile `agent.sock` / Named Pipe üzerinden konuşan yerel soket IPC sunucusu. Aktif ekran kullanıcısını eşler, UI mesajlarını iletir ve Windows oturum geçişlerinde aktif akış ayarını (`m_lastFpsSetEnv`) yeni tepsiye otomatik yeniden oynatır (state replay).
    *   [AgentDiscoveryCoordinator.h](../agent/core/AgentDiscoveryCoordinator.h) / [.cpp](../agent/core/AgentDiscoveryCoordinator.cpp): mDNS ve L3/VLAN izolasyonuna karşı kademeli DNS fallback (`okidia-broker` / `broker.okidia.local`) ile Broker arama ve otomatik yeniden bağlanma koordinatörü.
    *   [LocalScheduleWatchdog.h](../agent/core/LocalScheduleWatchdog.h) / [.cpp](../agent/core/LocalScheduleWatchdog.cpp): Broker veya yerel ağ kesildiğinde akıllı tahtaların ve istemcilerin bağımsız (otonom) olarak zil çalmasını ve kilitlenmesini sağlayan yerel nöbetçi zamanlayıcı. Ders çizelgesini yerel diskte önbelleğe alır (`schedule.json`), Broker bağlantısı koptuğunda dakikalık periyotla yerel saat üzerinden zilleri ve kilit perdesini yönetir; BIOS pili bitik olan istemcileri güvenli moda (fail-safe) alarak dersin bölünmesini engeller.
*   [main_tray.cpp](../agent/main_tray.cpp): Kullanıcı oturumu açıldığında veya Winlogon ekranında otomatik çalışan `okidia-tray` GUI uygulamasının giriş noktasıdır. Ekran yakalamayı, talep tabanlı timer yönetimini (`screenTimer` start/stop), kilit ekranlarını ve sohbet penceresini barındırır.

### 3.2 `agent/network/` (Ağ ve Keşif)
*   [DiscoveryService.h](../agent/network/DiscoveryService.h) / [.cpp](../agent/network/DiscoveryService.cpp): Ajanın yerel ağda mDNS üzerinden Broker'ı arama, OpenSSL ile CSR üretimi ve ilk katılım isteklerini (`DISCOVERY_REQUEST`) yalıtılmış olarak yöneten servis.

### 3.3 `agent/capture/` (Ekran Yakalama)
*   [ScreenCapture.h](../agent/capture/ScreenCapture.h) / [.cpp](../agent/capture/ScreenCapture.cpp): Ekran görüntüsünü alıp sıkıştıran, yalnızca değişen alanları (**dirty tiles**) tespit edip gönderen performanslı ekran yakalama motoru. Linux X11/Qt ve Windows Win32 GDI nesne havuzuna (HDC/HBITMAP reuse) sahiptir. Akış yeniden başladığında tam anahtar kare garantisi (`resetPreviousFrame`) sağlar.

### 3.4 `agent/input/` (Girdi Kontrolü)
*   [InputController.h](../agent/input/InputController.h) / [.cpp](../agent/input/InputController.cpp): Master'dan gelen fare/klavye olaylarını sisteme simüle eden (Linux XTest, Windows `SendInput`), klavye ve fareyi kilitleyen (`INPUT_LOCK`) platform girdi motoru.

### 3.5 `agent/commands/` (Komut Yürütücüler)
*   [CommandHandler.h](../agent/commands/CommandHandler.h) / [.cpp](../agent/commands/CommandHandler.cpp): Gelen komut zarflarını okuyup alt yürütücülere delege eden ana koordinatör.
*   [CommandHandlerPower.cpp](../agent/commands/CommandHandlerPower.cpp): Kapatma, yeniden başlatma, oturum kapatma ve uykuya alma komutları.
*   [CommandHandlerAudio.cpp](../agent/commands/CommandHandlerAudio.cpp): Ses seviyesi ayarlama, sessize alma ve anons komutlarını kuyruğa alıp sırayla yürütme.
*   [CommandHandlerInput.cpp](../agent/commands/CommandHandlerInput.cpp): Aygıt kilitlerini ve Windows kilit ekranı giriş komutlarını yöneten arabirim.
*   [CommandHandlerScreen.cpp](../agent/commands/CommandHandlerScreen.cpp): Ekran kilitleme overlay'ini yöneten ve ekran FPS ayarlarını değiştiren arabirim.
*   [CommandHandlerApp.cpp](../agent/commands/CommandHandlerApp.cpp): Çalışan süreçleri listeleyen ve kara listedeki uygulamaları sonlandıran mekanizma.

### 3.6 `agent/audio/` (Ses ve Oynatma)
*   [AudioPlayer.h](../agent/audio/AudioPlayer.h) / [.cpp](../agent/audio/AudioPlayer.cpp): Canlı ses yayınlarını (`AUDIO_BROADCAST_LIVE`), dosya anonslarını (`AUDIO_BROADCAST_FILE`) ve yerel zil seslerini (`AUDIO_PLAY_LOCAL`) oynatan Qt6 Multimedia ses alıcısı.
*   [LocalPlaylistPlayer.h](../agent/audio/LocalPlaylistPlayer.h) / [.cpp](../agent/audio/LocalPlaylistPlayer.cpp): Yerel çalma listelerini döngüsel oynatan yardımcı ses oynatıcı.

### 3.7 `agent/broadcast/` (Ekran Yayını Alma)
*   [ScreenBroadcastReceiver.h](../agent/broadcast/ScreenBroadcastReceiver.h) / [.cpp](../agent/broadcast/ScreenBroadcastReceiver.cpp): Master'dan gelen ekran yayınlarını (`BROADCAST_FRAME`) alıp ekranda çizen alıcı. Latest-Frame-Only drop mekanizmasıyla ara kare birikmesini ve gecikmeleri önler.

### 3.8 `agent/chat/` (Canlı İletişim)
*   [ChatHandler.h](../agent/chat/ChatHandler.h) / [.cpp](../agent/chat/ChatHandler.cpp): İstemci sohbet akışını, oda davetlerini, özel sohbet isteklerini ve bekleme sürelerini yöneten iş mantığı sınıfı.

### 3.9 `agent/transfer/` (Dosya Transferi)
*   [FileReceiver.h](../agent/transfer/FileReceiver.h) / [.cpp](../agent/transfer/FileReceiver.cpp): Master'dan gelen dosya ve anons aktarımlarını `sequence_id` eşlemesiyle paralel olarak alan modül.

### 3.10 `agent/auth/` (Çevrimdışı Kimlik Doğrulama)
*   [OfflineAuthManager.h](../agent/auth/OfflineAuthManager.h) / [.cpp](../agent/auth/OfflineAuthManager.cpp): Akıllı tahtalarda (Tip 3) sıfır ağ temasıyla (Air-Gapped) öğretmen tohumları (`teachers.dat`), dinamik QR kod ve 6 haneli HMAC PIN doğrulamasını yürüten çevrimdışı kimlik doğrulama motoru.

### 3.11 `agent/ui/` (Kullanıcı Arayüzü)
*   `AgentChatWindow/`:
    *   [AgentChatWindow.h](../agent/ui/AgentChatWindow/AgentChatWindow.h) / [.cpp](../agent/ui/AgentChatWindow/AgentChatWindow.cpp): Öğrencinin katıldığı odaları ve özel sohbetleri tek birleşik sekme çubuğunda barındıran istemci sohbet penceresi.
*   `AgentTray/`:
    *   [AgentTray.h](../agent/ui/AgentTray/AgentTray.h) / [.cpp](../agent/ui/AgentTray/AgentTray.cpp): Sağ alttaki durum simgesi, el kaldırma butonu ve menüleri barındıran tepsi arayüzü.
*   `ScreenLockOverlay/`:
    *   [ScreenLockOverlay.h](../agent/ui/ScreenLockOverlay/ScreenLockOverlay.h) / [.cpp](../agent/ui/ScreenLockOverlay/ScreenLockOverlay.cpp): Ekran kilitlendiğinde tüm pencerelerin üstüne tam ekran açılan ve klavye/fareyi bloke eden kilit ekranı.
*   `SmartboardLockOverlay/`:
    *   [SmartboardLockOverlay.h](../agent/ui/SmartboardLockOverlay/SmartboardLockOverlay.h) / [.cpp](../agent/ui/SmartboardLockOverlay/SmartboardLockOverlay.cpp): Tip 3 (Akıllı Tahta) kilitlendiğinde tam ekran açılan dokunmatik Numpad ve QR kod kilit ekranı.
*   `RecessDismissDialog/`:
    *   [RecessDismissDialog.h](../agent/ui/RecessDismissDialog/RecessDismissDialog.h) / [.cpp](../agent/ui/RecessDismissDialog/RecessDismissDialog.cpp): Ders bitiminde öğretmene 30 saniyelik nezaket süresi (Grace Period) tanıyan sağ-alt açılır pencere.
*   `PopupMessage/`:
    *   [PopupMessage.h](../agent/ui/PopupMessage/PopupMessage.h) / [.cpp](../agent/ui/PopupMessage/PopupMessage.cpp): Öğretmenden gelen anons ve popupları ekranda gösteren mesaj penceresi.
*   `BellToastBanner/`:
    *   [BellToastBanner.h](../agent/ui/BellToastBanner/BellToastBanner.h) / [.cpp](../agent/ui/BellToastBanner/BellToastBanner.cpp): Ders ve teneffüs zili çaldığında (veya aktif anons nedeniyle zil sesi bastırıldığında) ekranın sağ üst köşesinde zarif, koyu temalı, odak çalmayan ve otomatik kaybolan görsel zil bildirimini (`WA_ShowWithoutActivating`, `WA_TranslucentBackground`) sunan banner bileşeni.

### 3.12 `agent/system/` (Sistem ve Durum Yönetimi)
*   [SystemInfo.h](../agent/system/SystemInfo.h) / [.cpp](../agent/system/SystemInfo.cpp): İşletim sistemi bilgilerini, aktif kullanıcıyı, kilit ekranı durumunu (`isWorkstationLocked`) ve donanım kimliklerini okuyan; `launchProcessInConsoleSession` ile konsol oturumunda süreç başlatan sistem yöneticisi.
*   [PersistentStateManager.h](../agent/system/PersistentStateManager.h) / [.cpp](../agent/system/PersistentStateManager.cpp): Ekran, klavye/fare ve web kısıtlamalarını JSON dosyasına yazarak yeniden başlatmalarda kısıtlamaların 3 saatlik TTL ile kalıcı olmasını sağlayan durum yöneticisi.
*   [SessionSwitchManager.h](../agent/system/SessionSwitchManager.h) / [.cpp](../agent/system/SessionSwitchManager.cpp): Akıllı tahtalarda PIN ile kilit açıldığında hedef öğretmenin oturumuna güvenli geçişi sağlayan oturum yöneticisi.

### 3.13 `agent/web/` (İnternet Güvenlik Duvarı)
*   [WebFilter.h](../agent/web/WebFilter.h) / [.cpp](../agent/web/WebFilter.cpp): Windows Defender Firewall ve Linux iptables düzeyinde istemcinin internet erişimini deliksiz engelleyen (`WEB_BLOCK_ALL`) ve self-healing kural koruyuculuğu içeren güvenlik duvarı yöneticisi.

### 3.14 `agent/exam/` (Sınav Yönetimi)
*   [ExamHandler.h](../agent/exam/ExamHandler.h) / [.cpp](../agent/exam/ExamHandler.cpp): İstemci sınav modunu, sağ alttaki geri sayım panelini (`ExamOverlay`) ve sınav bittiğinde geçici dosyaları temizleyen `cleanupExamFiles()` mekanizmasını yöneten modül.

### 3.15 OTA Güncelleme ve Geri Alma (Rollback)
*   [OtaUpdater.h](../agent/OtaUpdater.h) / [.cpp](../agent/OtaUpdater.cpp): Linux'ta dpkg, Windows'ta Inno Setup ile sessiz güncelleme kuran ve yerel geri alma deposuna (`rollback/`) kararlı paketleri kopyalayarak ağ trafiği oluşturmadan tek tıkla geri alma (`ROLLBACK_TO_STABLE_REQ`) sağlayan modül.

---

## 4. Yönetici Katmanı (`master/`)

Öğretmenlerin ve yöneticilerin kullandığı zengin arayüzlü (Qt6 Widgets) masaüstü kontrol uygulamasıdır.

### 4.1 Çekirdek Yönetim ve Oturum
*   [main.cpp](../master/main.cpp): Master uygulamasının giriş noktası. `LoginDialog` ile Argon2id kimlik doğrulaması yapar, mTLS ile Broker'a bağlanır ve ana ekranı açar.
*   `session/`:
    *   [SessionManager.h](../master/session/SessionManager.h) / [.cpp](../master/session/SessionManager.cpp): İstemcilerden gelen canlı durum verilerini tutan, görünür istemcileri belirleyerek dinamik akış aboneliğini (`setVisibleClients`, debounce ve adaptif FPS) yöneten ve arayüze sinyaller üreten ana veri yöneticisi.
    *   [SessionManagerAudio.cpp](../master/session/SessionManagerAudio.cpp): Master mikrofon kaydı, yumuşak fade-in/out filtreleme, multicast anons aktarımı ve ses yayını komutlarını yöneten ses modülü.
    *   [SessionManagerCommands.cpp](../master/session/SessionManagerCommands.cpp): Kilit, güç, komut ve genel yönetim isteklerini paketleyip Broker'a gönderen komut işleyicisi.
    *   [SessionManagerDiscovery.cpp](../master/session/SessionManagerDiscovery.cpp): Yeni istemci onaylama/reddetme (`DISCOVERY_RESOLVED_BROADCAST`) ve kayıt akışlarını yöneten keşif işleyicisi.
    *   [SessionManagerHandlers.cpp](../master/session/SessionManagerHandlers.cpp): Broker'dan gelen asenkron yanıt zarflarını dinleyip arayüz modellerine dağıtan yanıt yönlendiricisi.
    *   [NetworkWatcher.h](../master/session/NetworkWatcher.h) / [.cpp](../master/session/NetworkWatcher.cpp): Master'ın ağ bağlantı durumunu, IP değişimlerini ve adaptör durumlarını izleyen ağ bekçisi.
*   [LicenseManager.h](../master/license/LicenseManager.h) / [.cpp](../master/license/LicenseManager.cpp): Okidia kurumsal lisans doğrulaması, cihaz kotası ve aktivasyon yönetimini yürüten modül.
*   [ScheduleManager.h](../master/schedule/ScheduleManager.h) / [.cpp](../master/schedule/ScheduleManager.cpp): Zamanlanmış görevleri (otomatik kapanma, periyodik anonslar vb.) yöneten zamanlayıcı.

### 4.2 `master/tree/` (Hiyerarşik Ağaç Görünümü)
*   [TreeManager.h](../master/tree/TreeManager.h): Sol taraftaki "Sınıf ──> Grup ──> İstemci" ve "Subnet" hiyerarşik listesini yöneten merkezi ağaç yöneticisi.
    *   [TreeManager.cpp](../master/tree/TreeManager.cpp): Çekirdek model koordinasyonu, sınıf/grup/istemci CRUD işlemleri, UUID yönetimi ve filtreleme.
    *   [TreeManagerUi.cpp](../master/tree/TreeManagerUi.cpp): `QTreeWidget` arayüz olayları, klavye kısayolları ve sürükle-bırak (`drag-drop`) akışları.
    *   [TreeContextMenu.cpp](../master/tree/TreeContextMenu.cpp): Ağaç öğeleri ve sınıflar için dinamik sağ tık hiyerarşik taşıma/bağlam menüleri.

### 4.3 `master/chat/` (Sohbet Yöneticileri)
*   [RoomManager.h](../master/chat/RoomManager.h) / [.cpp](../master/chat/RoomManager.cpp): Master tarafındaki genel, sınıf ve grup sohbet odalarının oluşturulmasını, üye durumlarının güncellenmesini ve YM ziyaretçi giriş/çıkış akışlarını kontrol eden modül.
*   [PrivateChatManager.h](../master/chat/PrivateChatManager.h) / [.cpp](../master/chat/PrivateChatManager.cpp): Birebir özel sohbet sekmelerinin durumlarını ve asenkron denetim kayıtlarını yöneten sınıf.

### 4.4 `master/ui/` (Zengin Arayüz Bileşenleri)
*   `MainWindow/`: Cihaz izleme ızgarasını (grid), sol hiyerarşik ağacı, üst toolbar komut düğmelerini barındıran ana dashboard.
    *   [MainWindow.h](../master/ui/MainWindow/MainWindow.h) / [MainWindow.cpp](../master/ui/MainWindow/MainWindow.cpp): Ana kurucu/yıkıcı, pencere durum eşitlemesi, dock kurulumları ve durum çubuğu kalıcı hedef rozeti.
    *   [MainWindowActions.cpp](../master/ui/MainWindow/MainWindowActions.cpp): Menü çubuğu, araç çubuğu butonları, kısayollar ve paneller menüsü entegrasyonu.
    *   [MainWindowEvents.cpp](../master/ui/MainWindow/MainWindowEvents.cpp): Sistem bildirimleri, donanım/saat uyarıları, OTA ve rol değişim diyalogları.
    *   [MainWindowLayout.cpp](../master/ui/MainWindow/MainWindowLayout.cpp): Thumbnail grup çerçeveleri, ızgara yerleşimi ve boyutlandırma olayları.
    *   [MainWindowCommands.cpp](../master/ui/MainWindow/MainWindowCommands.cpp): Kilit, güç, yayın, dosya ve anons toolbar komut tetikleyicileri.
    *   [MainWindowChat.cpp](../master/ui/MainWindow/MainWindowChat.cpp): Sohbet ve öğrenci yardım/el kaldırma bildirim slotları.
    *   [MainWindowDiscovery.cpp](../master/ui/MainWindow/MainWindowDiscovery.cpp): Keşfedilen yeni istemci onaylama/reddetme paneli.
    *   [MainWindowClientOps.cpp](../master/ui/MainWindow/MainWindowClientOps.cpp): İstemci bağlam menüsü (`buildClientMenu`), ekran/giriş kilidi ve istemci bazlı işlemler.
    *   [MainWindowExamAndFilter.cpp](../master/ui/MainWindow/MainWindowExamAndFilter.cpp): Sınav modu yönetimi ve sınıf/grup filtreleme mantığı.
    *   [MainWindowSettings.cpp](../master/ui/MainWindow/MainWindowSettings.cpp): Ayarlar diyaloğu entegrasyonu, rubber-band çoklu seçim ve arka plan denetleyicileri.
*   [LoginDialog.h](../master/ui/LoginDialog/LoginDialog.h) / [.cpp](../master/ui/LoginDialog/LoginDialog.cpp): Master konsoluna giriş için Argon2id parola doğrulamalı modern giriş ekranı.
*   [SettingsDialog.h](../master/ui/SettingsDialog/SettingsDialog.h) / [.cpp](../master/ui/SettingsDialog/SettingsDialog.cpp): Master ve Sunucu yapılandırmasını (Broker adı, portu, ağ parametreleri, okul/kurum ayarları) yöneten çok sekmeli ayarlar diyaloğu.
*   [ClientThumbnail.h](../master/ui/ClientThumbnail/ClientThumbnail.h) / [.cpp](../master/ui/ClientThumbnail/ClientThumbnail.cpp): İzleme ızgarasında her öğrencinin ekran görüntüsünü canlı differential tile'lar ile birleştirip çizen izleme kartı. RAM'deki kare tamponlarını temizleyen `clearFrameBuffer()` barındırır.
*   [ThumbnailGroupFrame.h](../master/ui/ThumbnailGroupFrame/ThumbnailGroupFrame.h) / [.cpp](../master/ui/ThumbnailGroupFrame/ThumbnailGroupFrame.cpp): İzleme ızgarasında sınıfları ve grupları görsel çerçevelerle gruplayan, daraltma/genişletme butonlu çerçeve bileşeni.
*   [LiveTooltip.h](../master/ui/LiveTooltip/LiveTooltip.h) / [.cpp](../master/ui/LiveTooltip/LiveTooltip.cpp): Thumbnail kartları üzerine gelindiğinde zengin sistem bilgisi (CPU, RAM, Aktif Pencere, IP vb.) gösteren animasyonlu canlı ipucu kartı.
*   [RemoteControlView.h](../master/ui/RemoteControlView/RemoteControlView.h) / [.cpp](../master/ui/RemoteControlView/RemoteControlView.cpp): Uzak masaüstü bağlantısı kurulduğunda öğrenci ekranını tam çözünürlüklü çizen ve fare/klavye olaylarını ileten kontrol penceresi.
*   [ChatWindow.h](../master/ui/ChatWindow/ChatWindow.h) / [.cpp](../master/ui/ChatWindow/ChatWindow.cpp): Öğretmenin katıldığı odaları ve özel sohbetleri tek birleşik sekme çubuğunda barındıran modern sohbet ekranı.
*   [RoomJoinRequestDialog.h](../master/ui/ChatWindow/RoomJoinRequestDialog.h) / [.cpp](../master/ui/ChatWindow/RoomJoinRequestDialog.cpp): Başka bir öğretmenin veya yöneticinin kilitli bir sohbet odasına katılma talebini onaylayan/reddeden onay diyaloğu.
*   [UserManagementPanel.h](../master/ui/UserManagementPanel/UserManagementPanel.h) / [.cpp](../master/ui/UserManagementPanel/UserManagementPanel.cpp): Yöneticiler, branş öğretmenleri ve sınıf yetkilendirmelerini yöneten bağımsız yönetim paneli.
    *   [UserManagementPanelUsers.cpp](../master/ui/UserManagementPanel/UserManagementPanelUsers.cpp): Yöneticiler sekmesi tablosu, CRUD diyaloğu, parola sıfırlama ve kurtarma kodu üretimi.
    *   [UserManagementPanelTeachers.cpp](../master/ui/UserManagementPanel/UserManagementPanelTeachers.cpp): Akıllı tahta branş öğretmenleri tablosu, mobil QR eşleştirme ve tahtalara anahtar eşitleme.
    *   [UserManagementPanelClasses.cpp](../master/ui/UserManagementPanel/UserManagementPanelClasses.cpp): Sınıf atama sekmesi ve yetki yönetimi.
*   [ExamDialog.h](../master/exam/ExamDialog.h) / [.cpp](../master/exam/ExamDialog.cpp): Sınav süresi, açıklaması ve hedef sınıfların belirlendiği sınav yönetim penceresi.
*   [SetupWizard.h](../master/ui/SetupWizard/SetupWizard.h) / [.cpp](../master/ui/SetupWizard/SetupWizard.cpp): Broker kurulduğunda ilk YM hesabını tanımlayan ilk kurulum sihirbazı.
*   [ReportPanel.h](../master/ui/ReportPanel/ReportPanel.h) / [.cpp](../master/ui/ReportPanel/ReportPanel.cpp): Kapsamlı denetim loglarını filtreleyen, sayfalayan, Excel uyumlu CSV, vektörel PDF ve HTML formatlarında dışa aktaran raporlama ekranı.
*   [BroadcastSourceDialog.h](../master/ui/BroadcastSourceDialog/BroadcastSourceDialog.h) / [.cpp](../master/ui/BroadcastSourceDialog/BroadcastSourceDialog.cpp): Ekran yayını başlatılırken spesifik monitör veya tüm monitörlerin panoramik olarak seçilmesini sağlayan kaynak seçim diyaloğu.
*   [BroadcastOverlayWidget.h](../master/ui/BroadcastOverlay/BroadcastOverlayWidget.h) / [.cpp](../master/ui/BroadcastOverlay/BroadcastOverlayWidget.cpp): Canlı ekran yayını sırasında öğretmenin ekranının üst-orta kısmında duran, fare üzerine geldiğinde aşağı sarkan (auto-hide/drop-down), anlık kalite slider'ı, sabitleme (pin) ve tek tıkla yayını durdurma butonlarını barındıran yüzen kontrol çubuğu.
*   [SmartboardConfirmDialog.h](../master/ui/SmartboardConfirmDialog/SmartboardConfirmDialog.h) / [.cpp](../master/ui/SmartboardConfirmDialog/SmartboardConfirmDialog.cpp): Yayın hedefi olarak Akıllı Tahta seçildiğinde yanlışlıkla ders akışını bozmamak için tahta listesini onaylatan güvenlik teyit diyaloğu.
*   [AnnounceRecordDialog.h](../master/ui/AnnounceRecordDialog/AnnounceRecordDialog.h) / [.cpp](../master/ui/AnnounceRecordDialog/AnnounceRecordDialog.cpp): Öğretmenin mikrofonla anlık anons kaydetmesini veya hazır ses dosyalarını seçili sınıf/hedef kapsamına, öğrenci ve akıllı tahta cihaz türü filtreleri ve güvenlik onay kalkanıyla yayınlamasını sağlayan gelişmiş ses ve anons arayüzü.
*   [LiveAnnounceDialog.h](../master/ui/LiveAnnounceDialog/LiveAnnounceDialog.h) / [.cpp](../master/ui/LiveAnnounceDialog/LiveAnnounceDialog.cpp): Yöneticinin mikrofondan doğrudan canlı ve anlık ses anonsu (Push-to-Talk) yapabilmesini sağlayan, mikrofon seçimi, süre sayacı ve tek tıkla canlı yayını başlatıp durduran hafif ve modern canlı anons arayüzü.
*   [ScreenBroadcaster.h](../master/broadcast/ScreenBroadcaster.h) / [.cpp](../master/broadcast/ScreenBroadcaster.cpp): Master ekranını yakalayıp JPEG olarak sıkıştıran, donanım fare imlecini (`drawCursor`) karenin üzerine çizen, seçilen kaliteye göre akıllı dinamik çözünürlük ölçeklemesi uygulayan ve backpressure (200 KB tampon kontrolü) ile yayınlayan ekran yayın motoru.
*   [AudioBroadcaster.h](../master/audio/AudioBroadcaster.h) / [.cpp](../master/audio/AudioBroadcaster.cpp): Master mikrofonundan gelen sesleri veya ses dosyalarını ağ üzerinden istemcilere canlı yayınlayan ses yayın motoru.
*   [LogViewerDialog.h](../master/ui/LogViewerDialog/LogViewerDialog.h) / [.cpp](../master/ui/LogViewerDialog/LogViewerDialog.cpp): Yerel Master, Broker sunucu ve uzaktaki online istemcilerin loglarını Protobuf (`LOG_TAIL_REQ`) üzerinden inceleyen modern log görüntüleyici.
*   [UninstallPasswordDialog.h](../master/ui/UninstallPasswordDialog/UninstallPasswordDialog.h) / [.cpp](../master/ui/UninstallPasswordDialog/UninstallPasswordDialog.cpp): Windows istemcilerde Okidia'nın yetkisiz kaldırılmasını önleyen kaldırma koruma parolasını yöneten iletişim kutusu.
*   [CustomProfilesDialog.h](../master/ui/CustomProfilesDialog/CustomProfilesDialog.h) / [.cpp](../master/ui/CustomProfilesDialog/CustomProfilesDialog.cpp): Zil/Amfi bilgisayarları, Özel Eğitim veya Anasınıfı gibi standart dışı makineler için kullanıcı tanımlı özel öğrenci profillerini (zil çalma, teneffüs kilidi muafiyeti, sohbet izni) yöneten arayüz bileşeni.
*   [AgentTypeSelectionDialog.h](../master/ui/AgentTypeSelectionDialog/AgentTypeSelectionDialog.h) / [.cpp](../master/ui/AgentTypeSelectionDialog/AgentTypeSelectionDialog.cpp): Onay bekleyen ajanlar için Agent Tipi (Öğrenci, İdari, Akıllı Tahta ve Tanımlı Özel Profiller) seçilmesini sağlayan onay iletişim kutusu.
*   [Widgets/MasterLockOverlay.h](../master/ui/Widgets/MasterLockOverlay.h) / [.cpp](../master/ui/Widgets/MasterLockOverlay.cpp): Hareketsizlik süresi dolduğunda veya `Ctrl+L` ile paneli kilitleyen güvenli kilit ekranı arayüz katmanı.
*   [Widgets/MasterUpdateDialog.h](../master/ui/Widgets/MasterUpdateDialog.h) / [.cpp](../master/ui/Widgets/MasterUpdateDialog.cpp): Master uygulamasının yeni sürümü çıktığında kullanıcıya bildiren ve OTA güncellemesini başlatan iletişim kutusu.
*   [ReportIssueDialog.h](../master/ui/ReportIssueDialog/ReportIssueDialog.h) / [.cpp](../master/ui/ReportIssueDialog/ReportIssueDialog.cpp): Öğretmen ve yöneticilerin karşılaştıkları teknik aksaklıkları 3 tıkla raporlamasını sağlayan kullanıcı arayüzü. Kategori seçimi, zaman penceresi, ekran görüntüsü iliştirme, yerel arşivi inceleme/masaüstüne kaydetme ve sunucu API'sine doğrudan arka plan yükleme desteği sunar.

### 4.5 `master/audit/` & `master/backup/` (Denetim ve Afet Kurtarma)
*   [AuditLog.h](../master/audit/AuditLog.h) / [.cpp](../master/audit/AuditLog.cpp): Kritik olayları toplayıp güvenli TLS oturumu üzerinden Broker adli denetim veritabanına (`audit_log`) ileten denetim yöneticisi.
*   [BackupManager.h](../master/backup/BackupManager.h) / [.cpp](../master/backup/BackupManager.cpp): Sistem yedeklerini (`.okbak`) yöneten, şifreli yedek alma, dışa/içe aktarma ve afet kurtarma (disaster recovery) döngülerini yürüten modül.

### 4.6 `master/transfer/` (Dosya ve Güncelleme Aktarımı)
*   [FileSender.h](../master/transfer/FileSender.h) / [.cpp](../master/transfer/FileSender.cpp): İstemcilere veya sınıflara toplu dosya gönderimini yöneten modül.
*   [OtaSender.h](../master/transfer/OtaSender.h) / [.cpp](../master/transfer/OtaSender.cpp): Ajan paketlerini Broker deposuna yükleyen ve YM yetkisiyle Broker'ın kendisini OTA üzerinden güncelleyen aktarım sınıfı.
*   [MasterUpdater.h](../master/transfer/MasterUpdater.h) / [.cpp](../master/transfer/MasterUpdater.cpp): Broker'daki güncel Master paketini indirip kurulumu tetikleyen güncelleme yöneticisi.

### 4.7 `master/diagnostics/` (Teşhis ve Olay Masası Motoru)
*   [DiagnosticCollector.h](../master/diagnostics/DiagnosticCollector.h) / [.cpp](../master/diagnostics/DiagnosticCollector.cpp): Sistem künyesi (OS, CPU mimarisi, çekirdek, Qt ve derleyici sürümü), küme durumu ve filtrelenmiş son hata/uyarı loglarını toplayıp şifre/token gibi hassas verileri regex ile maskeleyen (KVKK uyumlu), kurumsal schema 2.1.0 JSON manifesti ve zip arşivi üreten teşhis motoru.

### 4.8 `master/ssh/` (Gelişmiş Uzaktan Yönetim ve SSH)
*   [DeployPanel.h](../master/ssh/DeployPanel.h) / [DeployPanel.cpp](../master/ssh/DeployPanel.cpp) / [DeployPanelUi.cpp](../master/ssh/DeployPanelUi.cpp): Ağ tarama, SSH kimlik doğrulama, ajan paket dağıtımı ve tablo/grid görünüm geçişlerini yöneten ana dağıtım paneli.
    *   [DeployPanelCommon.h](../master/ssh/DeployPanelCommon.h) / [DeployPanelCommon.cpp](../master/ssh/DeployPanelCommon.cpp): Ortak durum sıralama enumu (`DeployStatusRank`), tablo durumu güncelleme ve hedef SSH yapılandırması çözümleyici.
    *   [DeployPanelHandlers.cpp](../master/ssh/DeployPanelHandlers.cpp): Ağ tarama olayları, toplu kimlik atama ve sağ tık menü koordinasyonu.
    *   [DeployPanelPrepare.cpp](../master/ssh/DeployPanelPrepare.cpp): Paket seçimleri ve hedef makinelerin asenkron SSH tespiti (`probeRowViaSsh`).
    *   [DeployPanelExecution.cpp](../master/ssh/DeployPanelExecution.cpp): Seçili cihazlara paralel SSH dağıtımını başlatma ve `DeployWorker` yaşam döngüsü takibi.
*   [DeployWorker.h](../master/ssh/DeployWorker.h) / [DeployWorker.cpp](../master/ssh/DeployWorker.cpp): Çoklu makinelerde paralel olarak SSH bağlantısı, SFTP dosya aktarımı, dpkg kurulumu ve servis doğrulamasını QtConcurrent üzerinde asenkron yürüten iş parçacığı yöneticisi.
*   [HostInstallCard.h](../master/ssh/HostInstallCard.h) / [HostInstallCard.cpp](../master/ssh/HostInstallCard.cpp): Paralel kurulum esnasında her bir hedef makinenin anlık durumunu, canlı yükleme yüzdesini ve hata çıktısı inceleme penceresini sunan kart bileşeni.
*   [DeployProgressGrid.h](../master/ssh/DeployProgressGrid.h) / [DeployProgressGrid.cpp](../master/ssh/DeployProgressGrid.cpp): Paralel kurulum kartlarını dinamik ızgara düzeninde gösteren, üst kısımda özet durum rozetleri ve filtreleme sunan görsel izleme paneli.
*   [SshManagerPanel.h](../master/ssh/SshManagerPanel.h) / [.cpp](../master/ssh/SshManagerPanel.cpp): Toplu komut ve aktif bağlantılar sekmelerini barındıran; tekil hedef ekleyen, çift tıklamayla terminale bağlanan ana SSH kontrol arayüzü koordinatörü.
    *   [SshManagerPanelUi.cpp](../master/ssh/SshManagerPanelUi.cpp): Sol hedef tablosu, sağ sekmeler, hız seçicisi (`m_cmdSpeedCombo`), nohup kutuları ve arayüz yerleşimi.
    *   [SshManagerPanelDiscovery.cpp](../master/ssh/SshManagerPanelDiscovery.cpp): Çevrimiçi istemcileri listeleme, kayıtlı SSH cihazlarını ekleme ve asenkron OS tespiti.
    *   [SshManagerPanelSessions.cpp](../master/ssh/SshManagerPanelSessions.cpp): Terminal/SFTP açma, çoklu sekme yönetimi ve aktif işlem korumalı güvenli kapatma (`safeDetach`).
    *   [SshManagerPanelCommands.cpp](../master/ssh/SshManagerPanelCommands.cpp): Asenkron toplu komut yürütme, hızlı TCP Probe, Wi-Fi `QSemaphore` concurrency kontrolü ve çıktı konsolu.
*   [SshTerminalWidget.h](../master/ssh/SshTerminalWidget.h) / [SshTerminalWidget.cpp](../master/ssh/SshTerminalWidget.cpp): QPainter tabanlı gerçek VT100/xterm terminal emülatörü (2D karakter ızgarası, alternate screen, scrollback, UTF-8, ConPTY, SIGWINCH dinamik satır hesaplama).
*   [SshDeviceSessionWidget.h](../master/ssh/SshDeviceSessionWidget.h) / [.cpp](../master/ssh/SshDeviceSessionWidget.cpp): Tek bir hedef cihaza ait tam ekran Terminal ve çift panelli Dosya Yöneticisi sekmelerini birleştiren oturum bileşeni.
*   [SshFileManagerWidget.h](../master/ssh/SshFileManagerWidget.h) / [SshFileManagerWidget.cpp](../master/ssh/SshFileManagerWidget.cpp): SSH/SFTP tabanlı iki panelli dosya yöneticisi arayüzü.
    *   [SshFileManagerRemote.cpp](../master/ssh/SshFileManagerRemote.cpp): Uzak SFTP dosya/dizin taraması, navigasyon, klasör oluşturma/silme ve önizleme indirmesi.
    *   [SshFileManagerLocal.cpp](../master/ssh/SshFileManagerLocal.cpp): Yerel dosya sistemi navigasyonu, sürücü listelemesi ve yerel dosya açma.
    *   [SshFileManagerTransfer.cpp](../master/ssh/SshFileManagerTransfer.cpp): Çift yönlü dosya/klasör aktarımı, Linux makineler arası `tar.gz` sıkıştırmalı transfer ve `TransferConflictDialog` çakışma çözümü.
*   [TransferConflictDialog.h](../master/ssh/TransferConflictDialog.h) / [.cpp](../master/ssh/TransferConflictDialog.cpp): Dosya transferlerinde hedefte aynı isimde öğe bulunduğunda üzerine yazma, atlama veya yeniden adlandırma seçeneklerini sunan diyalog.
*   [SshClient.h](../master/ssh/SshClient.h) / [.cpp](../master/ssh/SshClient.cpp): libssh2 kullanarak paralel ve bloke etmeyen bağlantı, kimlik doğrulama, komut çalıştırma ve SFTP operasyonlarını yürüten sınıf.
*   [SshTemplateDialog.h](../master/ssh/SshTemplateDialog.h) / [.cpp](../master/ssh/SshTemplateDialog.cpp): Toplu komutlar için kullanıcı tanımlı şablonları yöneten, `.oktpl` formatında dışa/içe aktaran şablon yöneticisi.
*   [SshCredentialDialog.h](../master/ssh/SshCredentialDialog.h) / [.cpp](../master/ssh/SshCredentialDialog.cpp): Yeni SSH kimliği tanımlama, düzenleme ve şablon oluşturma diyaloğu.
*   [SshCredSelectorWidget.h](../master/ssh/SshCredSelectorWidget.h) / [.cpp](../master/ssh/SshCredSelectorWidget.cpp): Panellerde kayıtlı SSH kimliklerini açılır kutuda listeleyen seçim bileşeni.
*   [SshCredStore.h](../master/ssh/SshCredStore.h) / [.cpp](../master/ssh/SshCredStore.cpp): SSH kimliklerini yerel veritabanında AES-256-GCM ile şifreli saklayan ve bellek hijyeni (`secureWipe`) sağlayan güvenli kimlik deposu.
*   [GitHubFetcher.h](../master/ssh/GitHubFetcher.h) / [.cpp](../master/ssh/GitHubFetcher.cpp): GitHub REST API üzerinden son `.deb` sürümlerini okuyup indiren ağ sınıfı.
*   [PackageRequirementSolver.h](../master/ssh/PackageRequirementSolver.h) / [.cpp](../master/ssh/PackageRequirementSolver.cpp): Ağ taraması ve OTA düğümlerinden toplanan hedef cihazların işletim sistemi ve mimari verilerini analiz eden paket çözümleyici.
*   [ReleaseManager.h](../master/network/ReleaseManager.h) / [.cpp](../master/network/ReleaseManager.cpp): Resmi OKIDIA Web Portalı (`https://okidia.net/api/releases.php`) REST API'si ile haberleşen sürüm servisi.
*   [DownloadManagerDialog.h](../master/ui/DownloadManagerDialog.h) / [.cpp](../master/ui/DownloadManagerDialog.cpp): Eksik paketleri kuyruk halinde indiren ve SHA-256 ile doğrulayan indirme yöneticisi.
*   [OtaUpdatePanel.h](../master/ssh/OtaUpdatePanel.h) / [.cpp](../master/ssh/OtaUpdatePanel.cpp): Ajanların TLS bağlantısı üzerinden Broker vasıtasıyla OTA güncellemelerini almasını sağlayan yönetim ve izleme paneli.
*   [BrokerUpdateManagerDialog.h](../master/ssh/BrokerUpdateManagerDialog.h) / [.cpp](../master/ssh/BrokerUpdateManagerDialog.cpp): Broker sunucusunun uzaktan tek tıkla güncellenmesini ve sürüm durumunun incelenmesini sağlayan yönetim diyaloğu.
*   [AccordionLogWidget.h](../master/ssh/AccordionLogWidget.h) / [.cpp](../master/ssh/AccordionLogWidget.cpp): Dağıtım ve komut konsollarında katlanabilir akordeon tarzı log görüntüleme aracı.

---

## 5. Geliştirici ve Yardımcı Araçlar (`tools/`)

*   [build_linux.sh](../tools/build_linux.sh): Linux ortamında `git pull`, otomatik commit sayacı ile versiyonlandırma, derleme ve DEB paketlerini `build/dist/linux/` altına üreten otomasyon betiği.
*   [build_win.ps1](../tools/build_win.ps1) / [build_win.bat](../tools/build_win.bat): Windows ortamında `git pull`, dinamik sürüm tespiti (`0.1.<count>`), tüm Windows hedeflerini (`okidia-agent`, `okidia-tray`, `okidia-master`, `okidia-broker`, `okidia-broker-tray`) derleyen, `windeployqt` ve OpenSSL staging klasörlerini hazırlayan ve Inno Setup ile kurulum paketlerini `build/dist/windows/` altına üreten birleşik Windows otomasyon betiği.
*   [broker_bridge.sh](../tools/broker_bridge.sh): Yerel sanal makinelerin (VirtualBox) veya yerel ağdaki ajanların `socat` TCP port tüneli ve `avahi-publish` mDNS yayını aracılığıyla doğrudan uzak Broker'a (Tailscale) bağlanmasını sağlayan köprü scripti.
*   [okidia-bridge.service](../tools/okidia-bridge.service): Köprü scriptini standart bir systemd sistem servisi olarak (`sudo systemctl start okidia-bridge`) isteğe bağlı çalıştırmaya yarayan servis tanım dosyası.
*   [sync_web_rpi.py](../tools/sync_web_rpi.py): Okidia web portalı (`okidia-net/`) dosyalarını yerel ağdaki Raspberry Pi 5 Apache web sunucusuna (`192.168.1.104:/var/www/html/okidia/`) `rsync` ile aktaran geliştirici senkronizasyon aracı.
*   [sync_web_vps.py](../tools/sync_web_vps.py): Okidia web portalı (`okidia-net/`) dosyalarını doğrudan canlı VPS prodüksiyon sunucusuna (`server.okidia.net:/var/www/html/onur/okidia/`) senkronize eden dağıtım otomasyon aracı.
*   [serve_mobile_https.py](../tools/serve_mobile_https.py): Mobil tarayıcıların (Chrome/Safari) WebRTC kamera akışını (`getUserMedia`) Secure Context (HTTPS) altında açması nedeniyle, `mobile-app/teacher` PWA uygulamasını yerel ağdaki telefonlar için self-signed SSL sertifikasıyla `https://<ip>:8443` portundan anında sunan test aracı.

---

## 6. Görsel Varlıklar ve Tema Kaynakları (`resources/`)

*   `resources/ui/`: Açık tema (Light Mode) için optimize edilmiş 55 adet modern SVG vektörel ikon (ayarlar, kullanıcılar, ağ, linux, windows vb.).
*   `resources/ui_dark/`: Koyu tema (Dark Mode) için optimize edilmiş 55 adet modern SVG vektörel ikon (`resources/ui/` ile birebir eşlenik).
*   `resources/icons/`: Master, Agent ve Broker masaüstü, sistem tepsisi (System Tray) durum ve animasyon PNG simgeleri ile Windows kurulum ve Program Ekle/Kaldır simge paketleri (`okidia-agent.ico`, `okidia-master.ico`, `okidia-broker.ico`).
*   `resources/styles/`: Açık (`default.qss`) ve koyu (`dark.qss`) Qt stil şablonları.
*   `resources/html/blocked.html`: Web filtresi veya alan adı kara listesi (Domain Blacklist) tetiklendiğinde öğrencinin tarayıcısında açılan, koyu ve açık temaya duyarlı yerel bilgilendirme sayfası.

---

## 7. Merkezi Çok Dilli Çeviri Havuzu (`translations/`)

Tüm platform bileşenleri (`master`, `agent`, `broker`, `common`) için tek merkezden yönetilen ve Qt ikili dosyalarına (`.qm`) derlenerek gömülen çok dilli yerelleştirme (i18n) dizini:

*   [okidia_tr.ts](../translations/okidia_tr.ts): Türkçe arayüz, mesaj ve komut dizgileri kaynak çeviri dosyası.
*   [okidia_en.ts](../translations/okidia_en.ts): İngilizce arayüz, mesaj ve komut dizgileri kaynak çeviri dosyası.

---

## 8. Lisans ve Yasal Belgeler (`LICENSE.txt`)

*   [LICENSE.txt](../LICENSE.txt): Okidia ticari yazılım lisans bildirimini ve kullanılan açık kaynak kütüphanelerin (Qt 6 LGPLv3, OpenSSL Apache 2.0, Google Protocol Buffers BSD 3-Clause, libssh2, libjpeg-turbo, SQLite ve Avahi) yasal atıflarını içeren merkezi lisans sözleşmesi metni.

---

## 9. Paketleme ve Kurulum (`packaging/windows/` & `packaging/debian/`)

*   [okidia-agent-setup.iss](../packaging/windows/okidia-agent-setup.iss): Inno Setup 6+ tabanlı Windows Agent kurulum betiği (`okidia-agent.exe`, `okidia-tray.exe`, Windows Servisi oluşturma, firewall kuralları, NTFS kısıtlamaları ve yönetici parola koruması).
*   [okidia-master-setup.iss](../packaging/windows/okidia-master-setup.iss): Inno Setup 6+ tabanlı Windows Master kurulum betiği (`okidia-master.exe`, DLL bağımlılıkları, masaüstü kısayolları ve profil koruma/temizleme seçenekleri).
*   [okidia-broker-setup.iss](../packaging/windows/okidia-broker-setup.iss): Inno Setup 6+ tabanlı Windows Broker kurulum betiği (`okidia-broker.exe` servisi, `okidia-broker-tray.exe` tepsisi ve `setup_code.txt` ilk kurulum aktivasyon kodu).
*   `packaging/debian/`: Linux Debian `.deb` paket konfigürasyonları, çift dilli maintainer betikleri (`preinst`, `postinst`, `prerm`, `postrm`), 3'lü veritabanı izinleri ve systemd servis tanımları.
    *   [usr.bin.okidia-broker](../packaging/debian/usr.bin.okidia-broker): Broker servisinin sertifika ve veritabanı dizinlerine erişimini kısıtlayan AppArmor güvenlik profili.
    *   [usr.bin.okidia-agent](../packaging/debian/usr.bin.okidia-agent): Agent servisinin anahtar ve sertifika klasörlerini yalıtan AppArmor güvenlik profili.

---

## 10. IDE ve Geliştirme Ortamı Konfigürasyonu (`.vscode/`)

*   [.vscode/settings.json](../.vscode/settings.json): VS Code ve Antigravity IDE ortamında Qt çeviri dosyalarının (`*.ts`) TypeScript yerine XML olarak tanınmasını sağlayarak sözdizimi hatalarını ve yanlış hata uyarılarını önleyen çalışma alanı ayar dosyası.

---

## 11. Mobil Uygulamalar (`mobile-app/`)

### 11.1 Öğretmen PIN Üretici (`mobile-app/teacher/`)
Öğretmenlerin akıllı tahtadaki ekran kilidini tamamen internetsiz (Air-Gapped) açabilmeleri için geliştirilen, Service Worker tabanlı bağımsız mobil web / PWA uygulaması.

*   `js/crypto.js`: Okidia C++ `KeyManager::generatePinFromSeed` algoritmasıyla birebir aynı HMAC-SHA256 ve RFC 4226 (HOTP) dinamik budama standardını uygulayan JavaScript kriptografi motoru.
*   `js/app.js`: Kamera vizör akışı, `jsQR` ile gerçek zamanlı QR kod tespiti, `localStorage` ile güvenli tohum saklama, 6 haneli PIN kartı ve 60 saniyelik dinamik geri sayım yöneticisi.
*   `sw.js`: Uygulamanın internetsiz / çevrimdışı yerel önbellekten çalışmasını sağlayan Service Worker motoru.
*   `css/app.css`: Okidia koyu tema paletine uyumlu mobil arayüz stili.
*   `index.html`: Responsive mobil viewport ve manifest tanımlarını içeren giriş sayfası.

---

## 12. Geliştirici ve Mimari Kılavuzları (`dev-docs/`)

*   [ARCHITECTURE.md](ARCHITECTURE.md): Okidia'nın çok katmanlı mikro-servis mimarisini, kuyruk mekanizmalarını ve ağ protokollerini açıklayan temel mimari rehber.
*   [ROADMAP.md](ROADMAP.md): Projenin tamamlanan, ertelenen ve gelecekte geliştirilmesi planlanan özelliklerini tutan merkezi yol haritası dökümanı.
*   [CODEBASE_CATALOG.md](CODEBASE_CATALOG.md): Projedeki tüm kaynak kod dosyalarının, C++ sınıflarının ve dizin yapılarının işlevsel envanteri.
*   [DATABASE_SCHEMA.md](DATABASE_SCHEMA.md): Okidia 3'lü bağımsız SQLite veritabanı (`okidia.db`, `okidia_audit.db`, `okidia_chat.db`) mimarisini, tüm tablo DDL'lerini, WORM tetikleyicilerini, indeksleri ve C++ singleton API'lerini belgeleyen teknik şema kılavuzu.
*   [CHANGELOG.md](CHANGELOG.md): Sürümler arası detaylı değişiklik günlükleri, yeni özellikler ve hata düzeltmeleri listesi.
*   [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md): Projeye yeni katılan geliştiriciler için ortam kurulumu, CMake derleme adımları, kodlama ve güvenlik standartları kılavuzu.
*   [BACKUP_AND_DISASTER_RECOVERY_GUIDE.md](BACKUP_AND_DISASTER_RECOVERY_GUIDE.md): Veritabanı yedekleme ve felaket kurtarma senaryolarını, CLI ve UI geri yükleme adımlarını açıklayan kullanıcı ve yönetici rehberi.
*   [YM_RECOVERY_GUIDE.md](YM_RECOVERY_GUIDE.md): Yönetici Master (YM) şifre sıfırlama, kurtarma kodu ve acil durum kurtarma senaryoları rehberi.
*   [ENDPOINT_SECURITY_GUIDE.md](ENDPOINT_SECURITY_GUIDE.md): MEB/Fatih ve kurumsal okul ağlarında sunucu ve uç nokta cihazlarının fiziksel güvenliğini sağlama adımlarını (BIOS parolası, USB Boot engelleme, Secure Boot zorunluluğu, Linux LUKS ve Windows BitLocker tam disk şifreleme) içeren kılavuz.
*   [LEGAL_MONETIZATION_GUIDE.md](LEGAL_MONETIZATION_GUIDE.md): Açık kaynak çekirdek, çift lisanslama, premium modüller ve kurumsal ticarileştirme strateji rehberi.
*   [ICON_ARCHITECTURE.md](ICON_ARCHITECTURE.md): Okidia SVG vektörel arayüz ikonları ve PNG uygulama/tepsi simgelerinin mimarisi ve Qt kaynak sistemi (`qrc`) eşleme kılavuzu.
