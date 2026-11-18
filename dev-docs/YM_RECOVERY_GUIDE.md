# OKIDIA Yönetici Master (YM) Kurtarma Kılavuzu
*(Herkesin Anlayabileceği Adım Adım Rehber)*

Bu kılavuz; Okidia sistemindeki en yetkili kullanıcı olan **Yönetici Master (YM)** hesabının şifresi unutulduğunda, bilgisayarı bozulduğunda, kurtarma kodu kaybolduğunda veya görev devri yapılması gerektiğinde **sistemin nasıl kurtarılacağını** tane tane açıklar.

---

## 1. YM (Yönetici Master) Nedir ve Neden Hayatidir?

Okidia sisteminde güvenlik gereği **Tek Yönetici Kuralı (Single YM Rule)** geçerlidir:
- Sistemde aynı anda sadece **1 adet resmi YM** bulunabilir.
- YM; okuldaki tüm akıllı tahtaların kaldırma şifresini bilir, öğretmen hesaplarını açar/kapatır, sunucunun yedeğini alır ve sistemi geri yükleyebilir.
- YM hesabının kilitlenmesi veya erişilemez hale gelmesi durumunda sistemin durmaması için **4 kademeli kurtarma zırhı** tasarlanmıştır.

---

## 2. Kurtarma Senaryoları (Hangisi Başınıza Geldi?)

Aşağıdaki durumlardan hangisini yaşıyorsanız ilgili başlığa gidin:

| Senaryo | Yaşanan Problem | Çözüm Yolu |
| :--- | :--- | :--- |
| **Senaryo 1** | Şifremi unuttum, elimde Kurtarma Kodu veya Kurulum Kodu var. | Giriş Ekranından Şifre Sıfırlama |
| **Senaryo 2** | Hem şifremi hem de Kurtarma Kodunu unuttum. | Sunucu Terminalinden `reset-admin` veya `show-setup-code` |
| **Senaryo 3** | YM bilgisayarı çöktü/formatlandı, yeni bilgisayara geçtim. | Yeni Master Kurulumu ve Giriş |
| **Senaryo 4** | Yöneticiliği başka bir öğretmene devretmek istiyorum. | Güvenli Yetki Devri (Handover) |
| **Senaryo 5** | Hem Sunucu hem YM bilgisayarı yok oldu (Felaket). | Harici `.okbak` ile Tam Kurtarma |

---

## 3. Senaryo 1: Şifremi Unuttum (Kurtarma Kodu veya Kurulum Kodu Elimde Var)

Sistem ilk kurulduğunda YM'ye ekranda gösterilen **16 haneli Kurtarma Kodu** (Örn: `A1B2-C3D4-E5F6-7890`) veya sunucunun **Kurulum Kodu** (Örn: `OKIDIA-A1B2-C3D4`) elinizdeyse:

1. Okidia Master programını açın.
2. Giriş ekranında (Login Penceresi) şifre kutusunun altındaki **"Şifremi Unuttum"** bağlantısına tıklayın.
3. Açılan pencereye 16 haneli **Kurtarma Kodunuzu** veya **Kurulum Kodunuzu** girin ve **"Kodu Doğrula"** butonuna basın.
4. Kod doğruysa sizden **Yeni Şifre** belirlemeniz istenecektir.
5. Yeni şifrenizi iki kez yazıp **"Şifreyi Kaydet"** deyin.
6. **Sonuç:** Hesabınızın kilidi anında açılır ve yeni şifrenizle giriş yapabilirsiniz.

---

## 4. Senaryo 2: Hem Şifremi Hem Kurtarma Kodunu Unuttum

> **"Kodu bir kağıda yazmıştım ama bulamıyorum, şifremi de hatırlamıyorum. Master'a giremiyorum!"**

Bu durumda Broker sunucusunun fiziksel olarak başına geçip terminalden şifreyi sıfırlayabilirsiniz.

### Adım Adım Terminalden YM Şifresini Sıfırlama:

#### 1. Adım: Sunucuda Terminali / Komut Satırını Açın
- **Linux:** Terminali açın.
- **Windows:** Komut İstemi'ni (CMD) veya PowerShell'i **Yönetici Olarak** açın.

#### 2. Adım: Sıfırlama Komutunu Çalıştırın
`okidia-broker` komutuna `--reset-admin` parametresini ve istediğiniz **yeni şifreyi** verin:

- **Linux:**
  ```bash
  okidia-broker --reset-admin "YeniSifrem2026!"
  ```
- **Windows:**
  ```cmd
  okidia-broker.exe --reset-admin "YeniSifrem2026!"
  ```

> ⚠️ **Şifre Kuralı:** Belirleyeceğiniz yeni şifre en az **8 karakter** olmalıdır.

#### 3. Adım: Ekranda Başarı Mesajını Görün
Terminalde şu mesaj çıkacaktır:
```text
Başarılı: YM şifresi sıfırlandı.
```

#### 4. Adım: Master Bilgisayarına Dönün
Master programını açın, YM kullanıcı adınızı ve az önce belirlediğiniz yeni şifrenizi yazıp giriş yapın. Sisteminiz açılacaktır!

> 💡 **Alternatif Pratik Yol (Kurulum Kodu ile Uzaktan Sıfırlama):**
> Sunucu terminalinde `okidia-broker --show-setup-code` komutunu çalıştırarak aktif kurulum kodunu öğrenebilir, ardından sunucunun başından kalkıp kendi Master bilgisayarınızın giriş ekranındaki *"Şifremi Unuttum"* alanına bu kodu yazarak şifrenizi doğrudan kendi masanızdan da sıfırlayabilirsiniz.

---

## 5. Senaryo 3: YM Bilgisayarı Çöktü / Formatlandı

YM'nin kullandığı dizüstü veya masaüstü bilgisayar arızalandıysa, çalındıysa ya da formatlandıysa:

1. Yeni bir bilgisayara **Okidia Master** kurulumunu yapın.
2. Bilgisayarın okul ağına (kablolu veya Wi-Fi) bağlı olduğundan emin olun.
3. Master programını başlatın.
4. Okidia'nın sıfır yapılandırma (Zero-Config / mDNS) motoru ağdaki Broker sunucusunu otomatik olarak bulacaktır.
5. Giriş ekranında YM kullanıcı adınızı ve parolanızı yazıp **Giriş Yap** butonuna basın.
6. Sunucu parolanızı doğrular, bu yeni bilgisayar için yeni ve güvenli bir **mTLS istemci sertifikası** üretir ve bağlantıyı onaylar.
7. Tüm sınıflar, ayarlar ve tahtalar yeni bilgisayarınızın ekranına saniyeler içinde geri yüklenir.

---

## 6. Senaryo 4: Yöneticiliği Başka Bir Öğretmene Devretme (Yetki Devri)

Okulda görev değişikliği olduğunda veya yöneticiliği başka bir öğretmene devretmek istediğinizde:

1. Kendi bilgisayarınızda Master'ı açıp YM olarak giriş yapın.
2. Üst menüden **"Yönetici ve Öğretmen Paneli"** (Kullanıcı Yönetimi) penceresini açın.
3. **Kullanıcılar** sekmesine gelin.
4. Yeni yönetici olacak öğretmeni seçip **"Düzenle"** deyin (veya yeni kullanıcı olarak ekleyin).
5. Rol kısmını **"Yönetici (YM)"** olarak seçin.
6. Ekrana kırmızı bir güvenlik uyarısı gelecektir:
   > **"DİKKAT: Yöneticilik Yetkisi Devrediliyor! Bu kullanıcıyı Yönetici (YM) yaptığınızda sistemin tek resmi Yöneticisi bu kullanıcı olacaktır. Kendi yetkiniz oturum kapandığında Gözlemci (GM) seviyesine düşürülecektir."**
7. Bu pencerede yer alan:
   - `[x] Bu bilgisayarda saklanan yerel sistem yedeklerini güvenle temizle (Önerilen)`
   kutusunu işaretli bırakın (Böylece devrettiğiniz eski bilgisayarınızda okulun veritabanı yedekleri kalmaz).
8. **"Onaylıyorum ve Devret"** butonuna basın.
9. Devir tamamlanır. Yeni yönetici kendi bilgisayarından girdiğinde tam yetkili YM olur.

---

## 7. Senaryo 5: Hem Sunucu Hem Master Bilgisayarı Yok Oldu (Büyük Felaket)

Yangın, hırsızlık veya elektrik voltajı sebebiyle hem Broker sunucusu hem de YM bilgisayarı aynı anda kullanılamaz hale geldiyse:

1. Yeni bir bilgisayara **Okidia Broker** kurun.
2. Yeni sunucunun ekranında çıkan veya `/var/lib/okidia/setup_code.txt` içindeki **Kurulum Kodunu** alın.
3. Başka bir bilgisayara **Okidia Master** kurup açın.
4. İlk açılış sihirbazında **"Yedekten Sistemi Kurtar (Disaster Recovery)"** seçeneğini seçin.
5. Önceden harici bir belleğe veya buluta aldığınız şifreli **`.okbak`** yedek dosyasını gösterip şifresini yazın.
6. Yeni sunucunun Kurulum Kodunu girin ve tamamlayın.
7. Eski sunucunun veritabanı, adli logları ve orijinal Kök CA kimliği yeni makineye aktarılır.
8. Siz de eski YM kullanıcı adınız ve şifrenizle içeri girip sistemi yönetmeye devam edersiniz.

---

## 8. Güvenlik ve Kurtarma İçin Altın Kurallar

1. **16 Haneli Kurtarma Kodunu Güvenli Bir Yere Kaydedin:**
   Master kurulumu tamamlandığında ekranda verilen Kurtarma Kodunu okul kasasında veya güvenli bir şifre yöneticisinde saklayın.
2. **Düzenli Olarak Harici Diske Yedek Çıkarın:**
   Master arayüzünden (`Ayarlar` -> `Yedekleme`) ayda en az bir kez manuel yedek alıp flash belleğe veya okulun harici yedekleme diskine kopyalayın.
3. **Özel Yedekleme Parolası Belirleyin:**
   Yedek alırken her zaman hatırlayacağınız güçlü bir şifre belirleyin; böylece sunucu fiziksel olarak yansa dahi yedeği istediğiniz yeni bilgisayarda açabilirsiniz.
