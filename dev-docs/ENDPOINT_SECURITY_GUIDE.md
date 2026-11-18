# 🛡️ Uç Nokta ve Sunucu Fiziksel Güvenlik Kılavuzu (Endpoint & Server Physical Security Guide)

Bu rehber, **OKIDIA (Okul İçi Denetim ve İletişim Ağı)** altyapısının kurulduğu okul ve kurumlarda uç nokta bilgisayarlarının (Agent), yönetim konsollarının (Master) ve ana sunucunun (Broker) fiziksel müdahalelere karşı güvenliğini sağlamak için uygulanması gereken temel adımları içerir.

---

## 📌 Giriş ve Tehdit Modeli

Ağ seviyesinde TLS/mTLS, şifreli veritabanı ve yetkilendirme katmanları ne kadar güçlü olursa olsun; cihazlara doğrudan fiziksel erişimi olan yetkisiz kişiler (öğrenciler, üçüncü şahıslar vb.) şu yöntemlerle sisteme müdahale edebilir:
1. **Canlı USB / Canlı Linux Başlatma (Live Boot):**
   - Bilgisayarı harici bir USB bellekten açarak ana işletim sisteminin şifresiz dosya sistemine doğrudan erişmek,
   - `/etc/okidia` veya `C:\ProgramData\Okidia` altındaki anahtarları ve sertifikaları kopyalamak,
   - Dosya sistemini veya ajan servis dosyalarını silmek ya da değiştirmek.
2. **Diskin Fiziksel Olarak Sökülmesi veya Çalınması:**
   - Broker sunucusunun veya Master bilgisayarının diski sökülerek başka bir cihaza takıldığında açık metin SQLite veritabanlarına (`okidia.db`, `okidia_audit.db`) ve özel anahtarlara (`broker.key`, `ca.key`) erişilmesi.
3. **BIOS/UEFI Ayarlarını Değiştirme:**
   - Güvenli Önyükleme'yi (Secure Boot) kapatarak imzalanmamış çekirdek veya rootkit sürücüleri yüklemek.

---

## 🔑 1. BIOS / UEFI Fiziksel Sertleştirmesi (Tüm Cihazlar İçin)

Tüm istemci bilgisayarları, akıllı tahtalar ve sunucular için aşağıdaki BIOS/UEFI yapılandırmaları **kesinlikle uygulanmalıdır**:

### 1.1 BIOS / UEFI Yönetici Parolası (Administrator Password)
* BIOS ayarlarına girişi güçlü bir parola ile kilitleyin.
* Kullanıcıların `F2`, `F12`, `Del` veya `Esc` tuşlarıyla BIOS'a girerek önyükleme sırasını değiştirmesini engelleyin.

### 1.2 Harici Ortamdan Başlatmayı (USB / Optical Boot) Devre Dışı Bırakma
* BIOS ayarlarından **USB Boot**, **External Media Boot** ve **Network PXE Boot** (eğer merkezi dağıtım kullanılmıyorsa) seçeneklerini `Disabled` yapın.
* Birincil önyükleme aygıtı olarak yalnızca dahili sabit diski / SSD'yi (`Internal NVMe / SSD / HDD`) sabitleyin.

### 1.3 Secure Boot (Güvenli Önyükleme) Aktivasyonu
* BIOS ayarlarından **Secure Boot** özelliğini `Enabled` konumuna getirin.
* Secure Boot, işletim sistemi çekirdeğinin dijital olarak imzalanmış olduğunu doğrular; yetkisiz çekirdek modüllerinin veya üçüncü taraf bootloader'ların çalışmasını engeller.
* *Not:* OKIDIA Agent, sistemin Secure Boot durumunu (`SystemInfo::isSecureBootActive()`) düzenli olarak denetler.

### 1.4 Kasa Kilidi ve Fiziksel Port Güvenliği
* Broker sunucusu kilitli bir sistem odasında veya kilitli bir sunucu kabininde (rack kabinet) barındırılmalıdır.
* Açık alanlardaki bilgisayarlar için kasa kilitleri veya USB port engelleyici fiziksel kilitler kullanılabilir.

---

## 💾 2. Tam Disk Şifreleme (Full Disk Encryption - FDE)

Tam disk şifreleme, cihaz kapatıldığında veya diski söküldüğünde tüm işletim sistemini, takas alanını (swap), logları ve OKIDIA verilerini okunamaz hale getirir.

### 2.1 Linux / Pardus / Ubuntu Üzerinde LUKS Kurulumu
Broker sunucusu ve Linux istemciler kurulurken tam disk şifreleme seçilmelidir:

1. **Kurulum Sırasında LUKS Seçimi:**
   - Pardus / Debian / Ubuntu kurulum medyasında disk bölümleme adımında **"Diski Sil ve LVM ile Şifreli Kurulum Yap" (Guided - use entire disk and set up encrypted LVM)** seçeneğini işaretleyin.
   - Güçlü bir disk şifreleme parolası (Passphrase) belirleyin.
2. **Mevcut Kurulumda Durum Kontrolü:**
   - Sistem diskinizin şifreli olup olmadığını terminalden kontrol edin:
     ```bash
     lsblk -o NAME,TYPE,MOUNTPOINT
     ```
     Kök dizin (`/`) veya sistem bölümleri altında `crypt` tipi görünüyorsa disk şifreleme aktiftir.
3. **Açılışta Otomatik Kilit Açma (TPM 2.0 Entegrasyonu):**
   - Sunucunun her yeniden başlatmada klavyeden parola istemeden güvenle açılması için `systemd-cryptenroll` veya `clevis` ile TPM 2.0 çipine bağlanabilir:
     ```bash
     systemd-cryptenroll --tpm2-device=auto --tpm2-pcrs=0+2+7 /dev/nvme0n1p3
     ```
     *(Bu işlem yalnızca TPM çipine ve BIOS Secure Boot bütünlüğüne sahip ana donanım üzerinde diskin açılmasını sağlar; disk söküldüğünde kilitlenir).*

### 2.2 Windows 10 / 11 Üzerinde BitLocker Kurulumu
Windows Master ve Agent bilgisayarlarında BitLocker'ı etkinleştirmek için:

1. **Gereksinim:** TPM 2.0 çipi ve Windows 10/11 Pro veya Enterprise sürümü.
2. **BitLocker'ı Başlatma:**
   - `Denetim Masası` > `Sistem ve Güvenlik` > `BitLocker Sürücü Şifrelemesi` bölümünü açın.
   - İşletim sistemi sürücüsü (`C:`) için **"BitLocker'ı Aç" (Turn on BitLocker)** seçeneğine tıklayın.
   - Kurtarma anahtarını güvenli bir yere (okul kasası veya kurumsal yedek) kaydedin.
3. **Komut Satırından Hızlı Doğrulama:**
   - Yönetici PowerShell veya CMD terminalinde:
     ```cmd
     manage-bde -status C:
     ```
     `Protection Status: Protection On` ve `Conversion Status: Fully Encrypted` çıktısı görülmelidir.

---

## 📋 3. Denetim Listesi (Quick Hardening Checklist)

| Güvenlik Adımı | Uygulanacak Cihazlar | Öncelik | Durum Kontrolü |
|---|---|---|---|
| **BIOS Yönetici Parolası** | Sunucu, Yönetici, Tüm Ajanlar | 🔴 Çok Yüksek | BIOS Menüsünde Parola Sorulması |
| **USB Boot Engeli** | Sınıf İstemcileri, Akıllı Tahtalar | 🔴 Çok Yüksek | Açılışta USB Seçeneğinin Engellenmesi |
| **Secure Boot Açık** | Tüm Cihazlar | 🟡 Yüksek | `mokutil --sb-state` / `Confirm-SecureBootUEFI` |
| **Tam Disk Şifreleme (LUKS/BitLocker)** | Broker Sunucusu, Master | 🔴 Kritik | OKIDIA SetupWizard & `lsblk` / `manage-bde` |
| **Fiziksel Sunucu Odası Kilidi** | Broker Sunucusu | 🔴 Kritik | Yetkisiz Girişe Kapalı Kilitli Kabinet |

---

*Bu kılavuz, OKIDIA Hardening Dokümantasyonu (Madde 18) kapsamında hazırlanmıştır.*
