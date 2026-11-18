# 🎨 OKIDIA İkon ve Görsel Varlık Mimarisi (Icon Architecture)

Bu belge; Okidia platformunun tüm alt modüllerinde (`okidia-master`, `okidia-tray` ve `okidia-broker-tray`) kullanılan görsel varlıkların, ikonların, bunların disk üzerindeki yollarının, piksel çözünürlüklerinin, tema modlarının (Açık/Koyu) ve Qt kaynak sistemi (`qrc`) üzerindeki eşleşmelerinin resmi dokümantasyonudur.

---

## 📂 1. SVG Formatındaki Vektörel Arayüz İkonları (`resources/ui/` & `resources/ui_dark/`)

Okidia, açık (Light) ve koyu (Dark) tema desteği için iki adet senkronize edilmiş SVG kaynak dizini barındırır:
- `resources/ui/`: Açık tema uyumlu SVG ikonları (`#2c3e50`, `#333333` veya renkli durum aksanları).
- `resources/ui_dark/`: Koyu tema uyumlu SVG ikonları (`#ffffff`, `#cdd6f4` veya parlak renkli durum aksanları).

`ThemeManager::icon(":/ui/<ikon_adi>.svg")` çağrısı yapıldığında, aktif temaya göre çalışma zamanında şeffaf bir şekilde uygun ikon dosyası çözümlenir (`:/ui/` -> `:/ui_dark/`).

| Dosya Adı | Qt Kaynak Yolu (QRC Path) | Kullanım Alanı / Açıklama |
| :--- | :--- | :--- |
| `add.svg` | `:/ui/add.svg` | Yeni eleman/sınıf/öğrenci ekleme butonları |
| `admin.svg` | `:/ui/admin.svg` | Yönetici (YM) simgeleri, admin hesabı görseli |
| `arrow_left.svg` | `:/ui/arrow_left.svg` | SFTP/Dosya yöneticisi önceki klasör, yetki geri alma butonları |
| `arrow_right.svg` | `:/ui/arrow_right.svg` | SFTP/Dosya yöneticisi sonraki klasör, yetki atama butonları |
| `arrow_up.svg` | `:/ui/arrow_up.svg` | SFTP/Dosya yöneticisi bir üst klasör butonu |
| `broadcast.svg` | `:/ui/broadcast.svg` | Ekran yayını ve akış başlatma/durdurma işlemleri |
| `chart.svg` | `:/ui/chart.svg` | Raporlama, log analizleri ve grafik panelleri |
| `chat.svg` | `:/ui/chat.svg` | Genel sınıf sohbeti ve öğrenciler arası özel sohbetler |
| `checked.svg` | `:/ui/checked.svg` | Toplu seçim veya onaylanmış durumlar |
| `check_circle.svg` | `:/ui/check_circle.svg` | Başarılı kurulum, hazır durum ve onay göstergeleri |
| `clear.svg` | `:/ui/clear.svg` | Girdileri ve seçimleri temizleme/sıfırlama |
| `clock.svg` | `:/ui/clock.svg` | Zamanlanmış görevler, ders saati ve takvim |
| `crown.svg` | `:/ui/crown.svg` | Yönetici (YM) rolü ve oda yetkisi |
| `discovery.svg` | `:/ui/discovery.svg` | Ağdaki broker'ları ve aktif ajanları keşfetme |
| `edit.svg` | `:/ui/edit.svg` | Yeniden adlandırma ve düzenleme kalem simgesi |
| `error_circle.svg` | `:/ui/error_circle.svg` | Başarısız işlem, bağlantı hatası göstergesi |
| `false.svg` | `:/ui/false.svg` | Başarısız, engellenmiş veya hatalı durum bildirimleri |
| `file.svg` | `:/ui/file.svg` | Dosya gönderme ve dosya alma işlemleri |
| `forbidden.svg` | `:/ui/forbidden.svg` | Desteklenmeyen aygıt, kısıtlı işlem veya paket çakışması |
| `fullscreen.svg` | `:/ui/fullscreen.svg` | Uzak masaüstü tam ekran ve pencere boyutu geçişi |
| `graduation.svg` | `:/ui/graduation.svg` | Öğretmen (OM) rol simgesi |
| `group.svg` | `:/ui/group.svg` | Laboratuvar alt grupları ve grup yönetimi |
| `hand.svg` | `:/ui/hand.svg` | Ajan tarafında söz isteme / parmak kaldırma simgesi |
| `home.svg` | `:/ui/home.svg` | SFTP dosya yöneticisi kullanıcı ana dizini |
| `hourglass.svg` | `:/ui/hourglass.svg` | SSH deneniyor / bekleme durumları |
| `key.svg` | `:/ui/key.svg` | Yetkilendirme, lisans anahtarları ve şifre kurtarma |
| `lock.svg` | `:/ui/lock.svg` | Ekran, klavye/fare kilitleme ve özel şifreli sohbet durumları |
| `monitor.svg` | `:/ui/monitor.svg` | Masaüstü izleme, kontrol ve sunum pencereleri |
| `mute.svg` | `:/ui/mute.svg` | Ses kapatma veya internet engelleme simgesi |
| `network.svg` | `:/ui/network.svg` | Ağ bağlantısı durumları ve detaylı IP/port ayarları |
| `offline.svg` | `:/ui/offline.svg` | Ağdan düşen veya çevrim dışı olan ajanlar |
| `online.svg` | `:/ui/online.svg` | Aktif ve çevrim içi olan ajanlar |
| `play.svg` | `:/ui/play.svg` | Ses kaydı dinleme ve genel oynatma / akış başlatma |
| `pause.svg` | `:/ui/pause.svg` | Ekran thumbnail akışını duraklatma (0 FPS) butonu |
| `play_cmd.svg` | `:/ui/play_cmd.svg` | Komut çalıştırma ve broadcast başlatma |
| `power.svg` | `:/ui/power.svg` | Uzaktan cihaz kapatma, yeniden başlatma ve çıkış |
| `record.svg` | `:/ui/record.svg` | Anons ses kaydı alma |
| `refresh.svg` | `:/ui/refresh.svg` | Yenileme, yeniden tarama ve paket kurulumu |
| `root_drive.svg` | `:/ui/root_drive.svg` | SFTP kök dosya sistemi (/) |
| `save.svg` | `:/ui/save.svg` | Dosya ve veritabanı yedeği kaydetme butonları |
| `save_template.svg` | `:/ui/save_template.svg` | SSH şablonu kaydetme butonu |
| `school.svg` | `:/ui/school.svg` | Sınıflar, laboratuvarlar ve okul geneli işlemler |
| `smartboard.svg` | `:/ui/smartboard.svg` | Akıllı tahta entegrasyonu ve kontrolü |
| `stop.svg` | `:/ui/stop.svg` | Devam eden yayını, kaydı veya kilit aksiyonunu durdurma |
| `student.svg` | `:/ui/student.svg` | İstemci listelerindeki öğrenci/ajan rolleri |
| `teacher.svg` | `:/ui/teacher.svg` | Kullanıcı yönetiminde öğretmen rolü |
| `template_settings.svg` | `:/ui/template_settings.svg` | SSH şablon ayarları |
| `transfer_left.svg` | `:/ui/transfer_left.svg` | SFTP dosya indirme / istemciden alma |
| `transfer_right.svg` | `:/ui/transfer_right.svg` | SFTP dosya yükleme / istemciye gönderme |
| `trash.svg` | `:/ui/trash.svg` | Çöp kutusu, silme ve kaldırma aksiyonları |
| `true.svg` | `:/ui/true.svg` | Başarılı, onaylanmış veya tamamlanmış durumlar |
| `unlock.svg` | `:/ui/unlock.svg` | Ekran ve girdi kilitlerini kaldırma butonları |
| `volume.svg` | `:/ui/volume.svg` | Ses açma, mikrofon ve hoparlör yayın durumları |
| `warning.svg` | `:/ui/warning.svg` | Uyarı ve tehlikeli işlem bildirimleri |

---

## 👩‍🏫 2. `resources/icons/master/` Klasörü (Master PNG Uygulama İkonları)

Yönetici/Öğretmen arayüzünün (`okidia-master`) ana uygulama simgeleridir. İşletim sistemi masaüstü kısayolları ve başlat menüsü entegrasyonu için farklı piksel boyutlarında PNG formatında olmalıdır.

*   **Disk Klasör Yolu:** `resources/icons/master/`
*   **Qt Kaynak Yolu (QRC Path):** `:/icons/master/<dosya_adi>`

| Dosya Adı | Çözünürlük | Kullanım Amacı / Sistem Yolu |
| :--- | :--- | :--- |
| `okidia-master-16.png` | 16x16 px | Başlık çubuğu ve küçük listeler |
| `okidia-master-22.png` | 22x22 px | Menü çubukları ve tray özetleri |
| `okidia-master-32.png` | 32x32 px | Tray menüsü ve pencere simgeleri |
| `okidia-master-48.png` | 48x48 px | `/usr/share/icons` (Sistem uygulama listeleri) |
| `okidia-master-64.png` | 64x64 px | Büyük simge görünüm modları |
| `okidia-master-128.png` | 128x128 px | Freedesktop masaüstü başlatıcıları |
| `okidia-master-256.png` | 256x256 px | Yüksek çözünürlüklü uygulama ikonları |

---

## 🧑‍🎓 3. `resources/icons/agent/` Klasörü (Ajan PNG Durum İkonları)

Öğrenci bilgisayarlarında çalışan `okidia-tray` servisinin sistem tepsisi (System Tray) durum ikonları ve bağlantı arama animasyon kareleridir. Tamamı PNG formatında olmalıdır.

*   **Disk Klasör Yolu:** `resources/icons/agent/`
*   **Qt Kaynak Yolu (QRC Path):** `:/icons/agent/<dosya_adi>`

### 3.1 Aktif Bağlantı İkonları (Connected State)
Ajan başarıyla Broker'a bağlandığında sistem tepsisinde görüntülenecek sabit durum ikonlarıdır.
*   `okidia-agent-connected-16.png` (16x16 px)
*   `okidia-agent-connected-22.png` (22x22 px)
*   `okidia-agent-connected-32.png` (32x32 px)

### 3.2 Arama Animasyonu İkonları (Search State Animation)
Ajan ağda Broker ararken sistem tepsisinde sırayla dönerek animasyon oluşturacak 3 aşamalı (3 frame) ikon setidir.
*   **Aşama 1 (Search 1):**
    *   `okidia-agent-search1-16.png` (16x16 px)
    *   `okidia-agent-search1-22.png` (22x22 px)
    *   `okidia-agent-search1-32.png` (32x32 px)
*   **Aşama 2 (Search 2):**
    *   `okidia-agent-search2-16.png` (16x16 px)
    *   `okidia-agent-search2-22.png` (22x22 px)
    *   `okidia-agent-search2-32.png` (32x32 px)
*   **Aşama 3 (Search 3):**
    *   `okidia-agent-search3-16.png` (16x16 px)
    *   `okidia-agent-search3-22.png` (22x22 px)
    *   `okidia-agent-search3-32.png` (32x32 px)

---

## 🖥️ 4. `resources/icons/broker/` Klasörü (Broker PNG İkonları)

Broker sistem tepsisi arayüzünün (`okidia-broker-tray`) durum ve tepsi ikonlarıdır. PNG formatında olmalıdır.

*   **Disk Klasör Yolu:** `resources/icons/broker/`
*   **Qt Kaynak Yolu (QRC Path):** `:/icons/broker/<dosya_adi>`

1.  `okidia-broker-16.png` (16x16 px)
2.  `okidia-broker-22.png` (22x22 px)
3.  `okidia-broker-32.png` (32x32 px)

---

## 🛠️ 5. CMake Paketleme ve Otomatik Derleme Bilgisi

Okidia yapı sisteminde, yukarıda listelenen tüm dosyalar ilgili modüllerin `CMakeLists.txt` dosyaları içerisindeki `qt6_add_resources` makrosuna kaydedilmiştir.

*   **Master Kaynak Entegrasyonu:** [master/CMakeLists.txt](file:///home/onurtamer/Projects/okidia/master/CMakeLists.txt#L120-L230)
*   **Ajan Kaynak Entegrasyonu:** [agent/CMakeLists.txt](file:///home/onurtamer/Projects/okidia/agent/CMakeLists.txt#L138-L245)
*   **Broker Kaynak Entegrasyonu:** [broker/CMakeLists.txt](file:///home/onurtamer/Projects/okidia/broker/CMakeLists.txt#L38-L46)

Yeni bir ikon ürettiğinizde veya mevcut bir ikonu değiştirdiğinizde, tek yapmanız gereken dosyayı yukarıda belirtilen disk yoluna (hem `resources/ui/` hem de `resources/ui_dark/` altına) aynı isimle kaydetmektir. Bir sonraki `cmake --build` aşamasında Qt ve CMake bu dosyaları otomatik olarak tarayıp ikili (binary) uygulamanın içerisine derleyecektir.
