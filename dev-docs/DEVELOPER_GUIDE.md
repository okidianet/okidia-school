# 💻 OKIDIA Kurulum ve Geliştirici Kılavuzu

Bu rehber; Okidia platformunu derlemek, test etmek, sisteme yeni özellikler/protobuf mesajları eklemek ve üç farklı istemci (Agent) modunun teknik davranışlarını anlamak isteyen geliştiriciler için hazırlanmıştır.

---

## 1. Derleme ve Yapılandırma adımları

Okidia, esnek ve modüler bir CMake derleme sistemine sahiptir. Modern C++20 özellikleri kullanıldığı için güncel bir derleyici (GCC 11+ veya Clang 13+) gereklidir.

### 1.1 Bağımlılıkların Kurulması (Ubuntu/Debian)
```bash
sudo apt update
sudo apt install build-essential cmake qt6-base-dev qt6-multimedia-dev qt6-tools-dev \
                 libssl-dev libssh2-1-dev protobuf-compiler libprotobuf-dev \
                 libsystemd-dev openssl libxkbcommon-dev libavahi-client-dev \
                 libxtst-dev libxfixes-dev libqt6svg6 qt6-translations-l10n
```

### 1.2 Derleme Parametreleri
*   **Release Modu (Önerilen):** Gecikmeleri ve CPU tüketimini azaltmak için varsayılan olarak `Release` derleme türü seçilir.
*   **Compile Commands:** Clangd dil sunucusu (LSP) desteği için `compile_commands.json` derleme sırasında otomatik olarak üretilir.

### 1.3 Derleme ve Paketleme Akışı
Projeyi `Debug` veya `Release` modunda yapılandırıp derlemek için:
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j$(nproc)
```

Derlenen agent paketini Debian (`.deb`) formatında paketlemek için:
```bash
cpack --config CPackConfig.cmake -G DEB -B packages
```

---

## 2. Üç Farklı İstemci Modu (Agent Types)

Okidia'da tek bir Agent kurulum paketi bulunur. Ancak cihazın tipi Broker veritabanındaki `agents.agent_type` alanına göre (Master veya Kurulum arayüzünden) değiştirildiğinde, Agent tamamen farklı iş mantıkları ve gizlilik sınırları sergiler:

```
[İstemci Paketi (okidia-agent)]
       ├── Tip 1: Öğrenci PC      ──> Tam Yönetim, Ekran/Giriş Kilidi, Sınav & Ekran/Ses Yayını Aktif
       ├── Tip 2: İdari PC        ──> Maksimum Gizlilik, Ekran Maskesi, Onay Diyalogları, Anons/Duyuru Aktif
       └── Tip 3: Akıllı Tahta    ──> Açık Sınıf Ekranı, Uzak Kontrol, Ekran Yayını & Sesli Anons Aktif
```

### 📋 İstemci Tipleri Teknik Matrisi

| Özellik / Davranış | Tip 1: Öğrenci PC | Tip 2: İdari PC | Tip 3: Akıllı Tahta |
| :--- | :---: | :---: | :---: |
| **Görsel Gizlilik Maskesi** | Yok (Gerçek Ekran) | **Aktif (Emerald Green Box)** | Yok (Gerçek Ekran) |
| **Ekran İzleme / Kontrol** | Onaysız (Anında) | **Kullanıcı Onayına Tabi** | Onaysız (Anında) |
| **Zorunlu Güç / Kilit / Web** | İzin verilir | **Engellenir (Servis Seviyesinde)**| İzin verilir |
| **Sesli Anons & Pop-up Duyuru** | **İzin verilir** | **İzin verilir (Genel/Acil Anons)** | **İzin verilir** |
| **Ekran Yayını (Broadcast)**| **İzin verilir** | Görmezden gelinir | **İzin verilir** |
| **Sınav Modu (`EXAM_START`)**| **Yalnızca Tip 1 Alabilir** | Görmezden gelinir | Görmezden gelinir |
| **Tip Değişim Koruması** | Anında uygulanır | **Kullanıcı Onayı Gerekir** | Anında uygulanır |

### Modların Derinlemesine Teknik Davranışları:

1.  **Tip 1 — Öğrenci PC (`agent_type = 1`):**
    *   Sınıf içi kontrolün tam sağlandığı varsayılan moddur.
    *   *Eğitim Araçları:* Sınav Modu, Ekran Yayını (Broadcast), Sesli Anonslar ve Anlık Duyuru Mesajları aktiftir.
    *   *Yönetim:* Ekran kilidi, klavye/fare kilidi, web filtreleme ve güç yönetimi doğrudan uygulanır.

2.  **Tip 2 — İdari PC (`agent_type = 2`):**
    *   **Gizlilik Öncelikli Mod:** Okul müdürü, memurlar veya öğretmen kişisel bilgisayarları için tasarlanmıştır.
    *   *Ekran Koruma Maskesi:* Canlı ekran izleme esnasında Agent, gerçek ekran yerine Master'a zümrüt yeşili (`QColor(46, 204, 113)`) düz bir görsel maske gönderir. Yetkisiz kişilerin idari ekranları gizlice izlemesi engellenir.
    *   *Uzaktan Erişim Onayı:* Öğretmen uzak kontrol (`INPUT_TAKE_CONTROL`) veya ekran izleme (`SCREEN_SELECT`) başlattığında, Agent ekranında **5 dakikalık zaman aşımına sahip** bir onay kutusu çıkar (`QMessageBox`). Kullanıcı onaylamazsa komut reddedilir.
    *   *Komut Engelleme:* Giriş kilitleme, ekran kilitleme, web filtreleme veya zorunlu güç kapatma komutları servis seviyesinde filtrelenerek **kesinlikle uygulanmaz**.
    *   *İletişim:* Genel okul anonsları (`AUDIO_BROADCAST_...`) ve idari duyuru mesajları (`MSG_POPUP`, `MSG_BROADCAST`) personelin bilgilendirilmesi için iletilir.
    *   *Gizlilik Düşürme Koruması:* Master konsolundan bir idari PC'nin tipi Öğrenci PC'ye çevrilmek istendiğinde, Agent arayüzünde "Gizlilik durumunun değişeceğini bildiren" zorunlu bir onay diyaloğu gösterilir.

3.  **Tip 3 — Akıllı Tahta (`agent_type = 3`):**
    *   Sınıfta herkesin görebileceği tahtalar içindir.
    *   Öğretmen ekran yayını, sesli anonslar, ekran kilitleme, internet engelleme ve güç kapatma komutları doğrudan uygulanır.
    *   Bireysel öğrenciye özgü Sınav Modu (`EXAM_START`) ve Özel Sohbet komutları tahta üzerinde devre dışıdır.

---

## 3. Yeni Bir Protobuf Mesajı Ekleme Rehberi

İletişim protokolüne yeni bir komut veya veri tipi eklemek için aşağıdaki adımları sırasıyla uygulayın:

### Adım 1: Mesaj Tipini Tanımlayın (`common/protocol/messages.proto`)
`enum MessageType` içine yeni bir enum sabiti ekleyin:
```protobuf
enum MessageType {
    ...
    MY_NEW_COMMAND          = 250; // Master -> Agent: Yeni Komut
    MY_NEW_COMMAND_RESULT   = 251; // Agent -> Master: Komut Sonucu
}
```

### Adım 2: Mesaj Şemasını Oluşturun
Dosyanın altına asıl veri şemasını tanımlayın:
```protobuf
message MyNewCommand {
    string action_name = 1;
    uint32 delay_seconds = 2;
}

message MyNewCommandResult {
    bool success = 1;
    string error_msg = 2;
}
```

### Adım 3: Protobuf Kodlarını Yeniden Üretin
CMake derleme aşamasında `.proto` dosyasındaki değişiklikleri algılayıp `messages.pb.h` ve `messages.pb.cc` dosyalarını otomatik olarak üretecektir. Sadece projeyi build etmeniz yeterlidir:
```bash
cd build && cmake --build . -j$(nproc)
```

### Adım 4: Master Konsolundan Gönderin
```cpp
okidia::MyNewCommand cmd;
cmd.set_action_name("clean_temp");
cmd.set_delay_seconds(5);

okidia::Envelope env;
env.set_type(okidia::MessageType::MY_NEW_COMMAND);
env.set_payload(cmd.SerializeAsString());
env.set_target_id(targetAgentUuid.toStdString());

m_connection->sendEnvelope(env);
```

### Adım 5: Agent Tarafında Mesajı Karşılayın (`agent/main.cpp` veya `main_tray.cpp`)
```cpp
case okidia::MessageType::MY_NEW_COMMAND: {
    okidia::MyNewCommand cmd;
    if (cmd.ParseFromString(env.payload())) {
        Logger::info("Yeni komut alindi: " + QString::fromStdString(cmd.action_name()));
        // Komut işlemlerini çalıştır...
    }
    break;
}
```

---

## 4. Gelecek Yol Haritası (v2 Planları)

Beta sürümünün başarıyla tamamlanmasının ardından v2 kapsamında aşağıdaki özelliklerin eklenmesi planlanmaktadır:

*   🔒 **AC-AC Özel Sohbet Şifrelemesi:** Öğrencilerin kendi aralarındaki birebir sohbetlerin veritabanına kaydedilmeksizin, cihazlar arasında **ECDH (Elliptic Curve Diffie-Hellman)** anahtar değişimi ve **AES-256-GCM** ile uçtan uca şifrelenmesi.
*   📊 **Raporlar Paneli Geliştirmesi:** Challenge-Response tabanlı adli erişim doğrulaması ile zenginleştirilmiş geçmiş ağ, uygulama ve audit log görüntüleme paneli.
*   🔄 **Otomatik Güncelleme Sistemi:** Agent servislerinin ve tray uygulamalarının arka planda imzalı paket doğrulamasıyla kendini güncelleyebilmesi.
*   💳 **Lisans Yönetimi:** Okul ve akıllı tahta sayılarına göre dinamik lisanslama ve merkezi yetkilendirme modülü.
