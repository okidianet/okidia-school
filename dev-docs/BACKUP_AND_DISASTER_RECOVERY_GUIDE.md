# OKIDIA Veritabanı Yedekleme ve Felaket Kurtarma Kılavuzu
*(Herkesin Anlayabileceği Adım Adım Rehber)*

Bu kılavuz; Okidia sisteminde veritabanı yedeklerinin **nerede saklandığını**, **nasıl şifrelendiğini**, sunucu çöktüğünde veya bakım gerektiğinde **terminalden (konsoldan)** ve **Master arayüzünden** nasıl kurtarılacağını en sade dille açıklar.

---

## 1. Genel Bakış ve Temel Mantık

Okidia'da sıradan bir "veritabanı kopyalama" yapılmaz. Okidia yedekleme sistemi bir **Felaket Kurtarma (Disaster Recovery)** mimarisidir:
- Yedek dosyalarının uzantısı **`.okbak`** şeklindedir (Örn: `okidia_20261001_130110.okbak`).
- Bu tek `.okbak` dosyasının içinde sadece veritabanı değil; sistemin beyni ve kimliği olan **Kök CA sertifikaları**, **özel anahtarlar**, **adli loglar** ve **kullanıcılar** bir arada paketlenir.
- Böylece sunucu yansa, çalınsa veya tamamen silinse dahi, elinizdeki tek bir `.okbak` dosyasıyla tüm okulu 1 dakikada sıfır ayar kaybıyla ayağa kaldırabilirsiniz.

---

## 2. Broker (Sunucu) Tarafında Yedekleme Nasıl Çalışır?

### Soru 1: Broker tarafında otomatik yedek alınıyor mu?
**Evet, tamamen otomatiktir.**
1. **Her Gece 03:00:** Broker sunucusu her gece saat 03:00'te sistem boşken sessizce tam yedek alır.
2. **Akıllı Telafi Modeli (Anacron):** Eğer sunucu gece kapalıysa (örneğin okulda elektrikler kesikse veya bilgisayar kapatılmışsa), sunucu sabah ilk açıldığında bunu fark eder ve açılıştan **5 dakika sonra** kaçırılan gece yedeğini otomatik olarak alır.

### Soru 2: Yedekler Broker'da hangi klasöre kaydedilir?
Broker bilgisayarında yedekler işletim sistemine göre şu klasörde toplanır:
- **Linux:** `/var/lib/okidia/backups/` *(veya `~/.local/share/okidia/backups/`)*
- **Windows:** `C:\ProgramData\okidia\backups\` *(veya `AppData\Local\okidia\backups\`)*

### Soru 3: Yedekler açık mı yoksa şifreli mi?
**KESİNLİKLE ŞİFRELİDİR.**
- Dosya asla açık SQLite veritabanı veya açık metin olarak diske yazılmaz.
- **Şifreleme Standardı:** Askeri düzeyde **AES-256-GCM** ve **PBKDF2-HMAC-SHA256** anahtar türetimi kullanılır. Dosyanın içeriği kurcalanamaz ve çalınsa dahi şifresi olmadan açılamaz.
- **Otomatik Yedek Parolası:** Master Ayarlar -> "Yedekleme ve Geri Yükleme" sekmesinde belirlenen **"Otomatik Sistem Yedeği Parolası"** kullanılır.
  * Eğer yönetici özel bir parola belirlememişse; sunucu yansa veya taşınsa dahi yedeğin harici makinede kurtarılabilmesi için sunucunun kalıcı **Kurulum Kodu (`setup_code`)** veya YM Kurtarma Kodu varsayılan kurtarma parolası olarak kullanılır.
  * Geriye dönük uyumluluk kapsamında yerel makinedeki donanımsal güvenlik anahtarı (`SHA-256(broker.key)`) da alternatif olarak kabul edilir.
- **Disk Alanı Koruması (Storage Safety):** Yedekleme öncesinde hedef diskte en az 150 MB veya veritabanı boyutunun 3 katı boş alan denetlenir. Yetersiz alanda disk kilitlenmelerini önlemek için acil durum temizliği (Emergency Prune) yapılır ve Master konsollarına telemetri uyarısı iletilir.

### Soru 4: Eski yedekler diski doldurur mu?
Hayır. Broker varsayılan olarak **son 7 yedeği** tutar (`retention = 7`). 8. yedek alındığında en eski yedek otomatik olarak silinir.

---

## 3. Master Olmadan, Broker Bilgisayarında Terminalden Kurtarma

Eğer elinizde Master bilgisayarı yoksa veya Master arayüzü açılmıyorsa; doğrudan Broker'ın kurulu olduğu sunucu makinesinde terminalden sistemi kurtarabilirsiniz.

### Adım Adım Terminalden Kurtarma (Linux & Windows):

#### Adım 1: Broker Servisini Durdurun
Veritabanı dosyaları kilitli kalmasın diye önce servisi durdurun:
- **Linux:**
  ```bash
  sudo systemctl stop okidia-broker.service
  ```
- **Windows (Yönetici CMD / PowerShell):**
  ```cmd
  sc stop OkidiaBroker
  ```

#### Adım 2: Kurtarma Komutunu Çalıştırın
`okidia-broker` komutuna `--restore` parametresiyle `.okbak` dosyasının tam yolunu verin:
- **Linux:**
  ```bash
  okidia-broker --restore /var/lib/okidia/backups/okidia_20261001_130110.okbak
  ```
- **Windows:**
  ```cmd
  okidia-broker.exe --restore "C:\ProgramData\okidia\backups\okidia_20261001_130110.okbak"
  ```

> **İpucu:** Parolayı komut içinde doğrudan vermek isterseniz:
> `--password "yedek_sifreniz"` parametresini ekleyebilirsiniz.
> Parola vermezseniz terminal size güvenli şekilde `Yedekleme Parolasını Girin:` diye soracaktır.
> 
> *Otomatik Parola Esnekliği:* Sunucu kurulum kodunu (`OKIDIA-XXXX-YYYY`), tireli/tiresiz veya önekli/öneksiz girdiğinizde sistem otomatik varyasyonları deneyerek paketi çözer.
> Sunucunun aktif kurulum kodunu öğrenmek için: `okidia-broker --show-setup-code` komutunu çalıştırabilirsiniz.

#### Adım 3: Ekranda Başarı Mesajını Görün
Terminalde şu çıktıları göreceksiniz:
```text
  • Adli Denetim veritabanı yüklendi (okidia_audit.db)
  • Sertifika yüklendi: ca.crt
  • Sertifika yüklendi: ca.key
  • Sertifika yüklendi: broker.crt
  • Sertifika yüklendi: broker.key
Başarılı: Sistem veritabanı, adli loglar ve Kök CA güvenlik sertifikaları başarıyla geri yüklendi.
```

#### Adım 4: Servisi Tekrar Başlatın
- **Linux:**
  ```bash
  sudo systemctl start okidia-broker.service
  ```
- **Windows:**
  ```cmd
  sc start OkidiaBroker
  ```
Sisteminiz tamamen eski haline dönmüştür!

---

## 4. Master (Yönetici Bilgisayarı) Tarafında Yedekleme Nerede Tutulur?

Master tarafında 2 tür yedek bulunur:

### A) Otomatik İkincil Replika Yedekleri (Disaster Recovery):
Yönetici Master (`YM`) hesabıyla giriş yapıldığında, Broker'daki en son yedek arka planda Master bilgisayarına sessizce ve parça parça indirilir.
- **Linux:** `~/.local/share/okidia-master/backups/`
- **Windows:** `C:\Users\<Kullanıcı>\AppData\Roaming\okidia-master\backups\`
- Bu sayede sunucu aniden yansa bile, yöneticinin kendi dizüstü/masaüstü bilgisayarında hazır bir güncel kopya bulunur.

### B) Kullanıcının Aldığı Manuel Sistem Yedekleri:
Yöneticinin arayüzden "Yedek Al" dediği dosyalar buraya kaydedilir:
- **Varsayılan Konum:** Belgelerim klasöründeki `OKIDIA-Yedekler` dizini:
  - **Linux:** `~/Belgeler/OKIDIA-Yedekler/`
  - **Windows:** `C:\Users\<Kullanıcı>\Documents\OKIDIA-Yedekler\`

---

## 5. Master Arayüzünden Yedek Alma ve Kurtarma Nasıl Yapılır?

### Senaryo A: Sistem Çalışırken Manuel Yedek Alma
1. Okidia Master programını açın ve **YM (Yönetici Master)** olarak giriş yapın.
2. Üst menüden veya araç çubuğundan **Ayarlar (Çark Simgesi)** penceresini açın.
3. Sol menüden **"Yedekleme ve Geri Yükleme"** kategorisine tıklayın.
4. **"Şimdi Yedek Al"** butonuna basın.
5. Açılan pencerede:
   - Yedeğiniz için bir **Parola** belirleyin (En az 4 karakter).
   - Yedeklenecek veritabanlarını seçin:
     - [x] *Çekirdek Sistem (okidia.db)* — Zorunludur.
     - [x] *Adli Denetim Günlükleri (okidia_audit.db)* — Açık olması önerilir.
     - [ ] *Sohbet Geçmişi (okidia_chat.db)* — İsteğe bağlı.
6. **"Yedek Al"** butonuna tıklayın. Dosyanız saniyeler içinde `OKIDIA-Yedekler` klasörüne oluşturulacaktır.

---

### Senaryo B: Sistem Çalışırken Master Üzerinden Geri Yükleme
1. Master programında **Ayarlar** -> **Yedekleme ve Geri Yükleme** bölümüne gidin.
2. **"Yedekten Geri Yükle"** butonuna basın.
3. Açılan dosya seçim penceresinden kurtarmak istediğiniz `.okbak` dosyasını seçin.
4. Yedeği alırken koyduğunuz parolayı girin.
5. Karşınıza yedeğin tarihi, oluşturan kişi ve içerik özetini gösteren bir onay kutusu gelecektir. **"Evet"** deyin.
6. Master paketi Broker'a iletir; Broker sistemi eski haline getirip 2 saniye içinde kendini tazeler. Master otomatik olarak tekrar bağlanır.

---

### Senaryo C: Sunucu Tamamen Sıfırlandı / Yeni Sunucu Kuruldu (Felaket Kurtarma)
Eğer eski sunucu çöktü ve yeni bir makineye sıfırdan Okidia Broker kurulduysa:
1. Yeni sunucuda Broker kurulduğunda oluşan ekrandaki veya `setup_code.txt` içindeki **Kurulum Kodunu** (Örn: `A1B2-C3D4`) bir kenara not edin.
2. Kendi bilgisayarınızda Okidia Master'ı açın. Karşınıza **İlk Kurulum Sihirbazı** gelecektir.
3. 2. Sayfada ("Kurulum Tipi") şu 3 seçenek görünür:
   - *Yeni bir sistem kur (Yönetici Master - YM)*
   - *Mevcut bir sisteme katıl (Öğretmen veya Gözlemci)*
   - 🔘 **"Yedekten Sistemi Kurtar (Disaster Recovery)"** -> **Bunu seçin!**
4. İleri butonuna basın.
5. Elinizdeki `.okbak` yedek dosyasını seçin ve yedeğin parolasını yazın.
6. Bir sonraki ekranda yeni sunucunun **Kurulum Kodunu** girin.
7. Sihirbazı tamamlayın.
8. **Sonuç:** Yeni kurulan sunucu, eski sunucunun birebir aynısı olur. Eski şifreler, eski sınıflar, kullanıcılar ve en önemlisi eski Kök CA sertifikaları yüklenir. Okuldaki hiçbir akıllı tahta veya öğrenci bilgisayarına tek bir ayar dahi yapmanız gerekmez; tüm cihazlar yeni sunucuyu sanki hiç kesinti olmamış gibi anında tanır.

---

## 6. Özet Tablo

| Özellik | Broker Otomatik Yedek | Master Manuel Yedek | Master İkincil Replika |
| :--- | :--- | :--- | :--- |
| **Konum** | `/var/lib/okidia/backups/` | `~/Belgeler/OKIDIA-Yedekler/` | `~/.local/share/okidia-master/backups/` |
| **Format** | `.okbak` (v3 Birleşik Paket) | `.okbak` (v3 Birleşik Paket) | `.okbak` (v3 Birleşik Paket) |
| **Şifreleme** | AES-256-GCM + PBKDF2 | AES-256-GCM + PBKDF2 | AES-256-GCM + PBKDF2 |
| **Zamanlama** | Her gece 03:00 + Anacron (5 dk) | Kullanıcı isteğine bağlı | Her YM girişinde + yeni yedek anonsunda |
| **Saklama (Retention)** | Son 7 adet | Son 7 adet (ayarlanabilir) | Son 7 adet |
| **Ağ Tüketimi** | Yerel (0 bayt ağ) | Sunucudan Master'a çekilir | Ön kontrol (Check) ile sadece yeni varsa parça parça |
| **CLI Kurtarma** | `okidia-broker --restore` | `okidia-broker --restore` | `okidia-broker --restore` |
