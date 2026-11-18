# 🏛️ OKIDIA Yayınlama, Lisanslama, Gelir Modeli ve Yasal Güvence Rehberi
*(Devlet Memuru / Öğretmenler İçin Sıfır Hata ve Sıfır Risk Kılavuzu)*

Bu rehber; Okidia projesini küresel standartlarda açık kaynaklı bir ürün olarak yayınlarken; **hukuki (657 DMK & FSEK), mali (vergi mevzuatı), teknik (kod imzalama) ve gelir (bağış/sponsorluk)** adımlarını hata payı bırakmayacak şekilde baştan sona belgeler.

---

## 📑 İÇİNDEKİLER
1. [Hukuki Zırh: 657 Sayılı Kanun ve Telif İstisnası](#1-hukuki-zırh-657-sayılı-kanun-ve-telif-istisnası)
2. [Açık Kaynak Lisansı: Neden ve Nasıl GNU GPL v3?](#2-açık-kaynak-lisansı-neden-ve-nasıl-gnu-gpl-v3)
3. [Windows Defender & Güvenlik: Kod İmzalama (Code Signing)](#3-windows-defender--güvenlik-kod-imzalama-code-signing)
4. [Gelir Kanalları ve Para Transfer Mekanizması](#4-gelir-kanalları-ve-para-transfer-mekanizması)
5. [Maliye ve Vergi Güvencesi: Paranın Hesaba Girişi](#5-maliye-ve-vergi-güvencesi-paranın-hesaba-girişi)
6. [Adım Adım Uygulama Yol Haritası (Checklist)](#6-adım-adım-uygulama-yol-haritası-checklist)

---

## 1. Hukuki Zırh: 657 Sayılı Kanun ve Telif İstisnası

Bir devlet memuru olarak en büyük çekincenin *"Ticaret yasağına takılır mıyım? Disiplin soruşturması açılır mı?"* olduğunu biliyorum. Cevap: **Doğru yoldan gidersen KESİNLİKLE HAYIR.**

### Yasal Dayanaklar:
1. **657 Sayılı DMK Madde 28:** Memurların tacir veya esnaf sayılmalarını gerektirecek bir faaliyette bulunmasını yasaklar. **Ancak bilimsel, edebi ve sanatsal eser üretmeyi YASAKLAMAZ.**
2. **5846 Sayılı Fikir ve Sanat Eserleri Kanunu (FSEK):**
   * Madde 2/1 uyarınca **"Bilgisayar programları (yazılımlar)"**, kanunen birer **İlim ve Edebiyat Eseri**dir (roman yazmak veya beste yapmakla hukuki statüsü birebir aynıdır).
   * Danıştay ve Devlet Personel Başkanlığı'nın (DPB) yerleşik içtihatlarına göre: *Memurlar yazdıkları eserlerin telif hakkını devredebilir, telif geliri elde edebilir ve eserlerinden doğan haklarını kullanabilir.*
3. **Altın Kural:**
   * Bir şey "satmıyorsun" (e-ticaret/dükkan yok).
   * Fatura kesmiyorsun (şahıs şirketi kurmuyorsun).
   * Açık kaynak bir esere dünya çapından **"Gönüllü Telif / Bağış / Sponsorluk"** kabul ediyorsun.

---

## 2. Açık Kaynak Lisansı: Neden ve Nasıl GNU GPL v3?

Projenin ticari şirketler tarafından çalınmasını engellerken aynı zamanda gelir elde edebilmen için en doğru lisans **GNU General Public License v3 (GPL-3.0)**'tür.

### GPL v3 Ne Sağlar?
* **Kodun Çalınmasını Engeller (Copyleft):** Bir firma veya kişi Okidia'nın kodlarını alıp kapalı kaynaklı ticari bir ürüne dönüştüremez. Okidia'yı kullanan herkes kaynak kodlarını açık tutmak zorundadır.
* **Gelir Hakkını Korur:** GPL v3, yazılımdan gelir elde edilmesini (bağış toplama, sponsorluk alma, ücretli kurulum/destek verme) **açıkça serbest bırakır**.
* **Sorumluluk Reddi (No Warranty):** Yazılımın bir okulda yaratabileceği olası aksaklıklardan dolayı senin şahsi olarak maddi tazminatla sorumlu tutulmanı engeller.

### Nasıl Uygulanır?
Proje ana dizinindeki [LICENSE.txt](file:///home/onurtamer/Projects/okidia/LICENSE.txt) dosyası standart GPL-3.0 metni ile güncellenir ve GitHub reposuna basılır.

---

## 3. Windows Defender & Güvenlik: Kod İmzalama (Code Signing)

Windows kullanıcılarının `.exe` dosyasını indirirken *"Windows kişisel bilgisayarınızı korudu (Bilinmeyen Yayıncı)"* mavi ekranını görmemesi için iki aşamalı bir strateji izlenir:

### Aşama 1: Ücretsiz İtibar Kazanma (Reputation Building)
1. **Inno Setup Meta Verileri:**
   * Paketleme dosyalarında (`okidia-master-setup.iss` vb.) `AppPublisher=Onur Tamer`, `AppVersion=0.1.x`, `AppId` alanları eksiksiz tutulur.
2. **Microsoft Defender Whitelist Gönderimi (Submission):**
   * Her yeni sürüm çıktığında, üretilen `.exe` dosyaları [Microsoft Security Intelligence Submission](https://www.microsoft.com/en-us/wdsi/filesubmission) portalına yüklenir.
   * Kategori: *"Software Developer / False Positive"*, Açıklama: *"Open source educational classroom management software"*.
   * Microsoft 2-4 saat içinde dosyayı inceler ve Defender veritabanında "Güvenli (Clean)" olarak etiketler.

### Aşama 2: Ücretsiz Kurumsal Kod İmzalama (SignPath.io)
* Açık kaynak projelere dünya genelinde ücretsiz kod imzalama sertifikası sağlayan **SignPath Foundation** vakfı bulunur.
* Proje GitHub'da GPLv3 lisansıyla yayınlandığında, SignPath'e ücretsiz açık kaynak başvurusu yapılır.
* GitHub Actions ile her release aldığında, SignPath senin `.exe` dosyalarını resmi global sertifikayla dijital olarak imzalar.

---

## 4. Gelir Kanalları ve Para Transfer Mekanizması

Türkiye'deki bankacılık (BDDK) ve PayPal yasağı göz önüne alındığında, **paranın Türkiye'ye sorunsuz akacağı 2 garanti kanal** vardır:

### Kanal A: GitHub Sponsors (Öncelikli & En Kurumsal Kanal)
* **Nasıl Çalışır?**
  1. GitHub profilinden `github.com/sponsors` sayfasına girip başvuru yaparsın.
  2. Başvuru sırasında ülke olarak **Turkey** seçilir.
  3. GitHub, seni kendi kurumsal köprüsü olan **Stripe Connect** arayüzüne yönlendirir.
  4. Türkiye'deki banka hesabının **IBAN** ve T.C. kimlik bilgilerini girersin.
* **Avantajı:** GitHub hiçbir komisyon kesmez (%0 fee). Yabancı veya yerli destekçiler kredi kartıyla sana aylık veya tek seferlik sponsor olabilir. Para doğrudan Türkiye'deki banka hesabına yatar.

### Kanal B: Patreon + Payoneer (Bireysel Destekçiler İçin)
* **Nasıl Çalışır?**
  1. Bir Patreon sayfası açarsın (Örn: `patreon.com/okidia`).
  2. Patreon ödeme alma yöntemi olarak **Payoneer** seçilir.
  3. Ücretsiz bir Payoneer hesabı açılıp Patreon'a bağlanır.
  4. Dünyadan Patreon'a gelen bağışlar Payoneer'e geçer; Payoneer'den de tek tıkla Türk banka hesabına (Ziraat, İş Bankası vb.) TL veya USD olarak çekilir.

---

## 5. Maliye ve Vergi Güvencesi: Paranın Hesaba Girişi

Banka hesabına para girmeye başladığında, memuriyetinin tehlikeye girmemesi için devletin sunduğu yasal zırhları kullanacaksın:

### Yöntem 1: FSEK Telif İstisnası (GVK Madde 18) — Başlangıç Modeli
* Açık kaynak yazılım bir ilim eseridir. GitHub Sponsors üzerinden gelen bağışlar "telif ödülü/bağışı" niteliğindedir.
* Bu gelirler yıllık belirli bir istisna tutarına kadar (2024-2026 bandında yıllık yaklaşık 3-4 Milyon TL) gelir vergisinden muaftır ve fatura kesilmesini gerektirmez.

### Yöntem 2: "Sosyal İçerik Üreticiliği ve Mobil Uygulama İstisnası" (GVK Mükerrer 20/B) — En Sağlam Zırh
Eğer mobil uygulamadan AdMob veya uygulama içi destek geliri alacaksan:
1. İkamet ettiğin yerdeki Vergi Dairesine gidip (veya İnteraktif Vergi Dairesi üzerinden): *"Mobil Uygulama Geliştiriciliği İstisna Belgesi"* talep ediyorsun. (Şirket kurmak gerekmez, defter tutulmaz).
2. Bu belge ile kamu bankasında (Ziraat/Vakıfbank) özel bir **"İstisna Hesabı"** açtırıyorsun.
3. Mobil reklam veya sponsor gelirleri bu IBAN'a yatar.
4. Banka gelen paradan **otomatik %15 stopaj kesip devlete öder**, kalan %85 sana kalır.
5. Bu işlem Gelir İdaresi Başkanlığı nezdinde tamamen kayıtlı ve yasaldır; hiçbir müfettiş veya denetmen "Sen şirket kurmuşsun" diyemez.

---

## 6. Adım Adım Uygulama Yol Haritası (Checklist)

### 1. Hazırlık & Lisanslama (Hemen Şimdi)
- [ ] Projenin ana dizinine GNU GPL v3 lisans metnini (`LICENSE`) ekle.
- [ ] README.md dosyasına projenin misyonunu, açık kaynak olduğunu ve ekran görüntülerini ekle.

### 2. Finansal Köprülerin Kurulması
- [ ] **GitHub Sponsors** başvurusu yap (Stripe Connect üzerinden Türk IBAN'ını bağla).
- [ ] Bir **Payoneer** hesabı oluştur (Türkiye banka hesabını tanıt).
- [ ] Destekçiler için sade bir **Patreon** sayfası aç ve Payoneer'i bağla.

### 3. Arayüz ve Web Entegrasyonu (Saygın & Zarif İletişim)
- [ ] `okidia.net` web sitesine bir *"Sponsor Ol / Destekle"* bölümü ekle (GitHub Sponsors ve Patreon butonları).
- [ ] Master uygulamasının "Hakkında" (About) penceresine zarif bir `☕ Projeyi Destekle` butonu ekle (tıklandığında GitHub Sponsors sayfasını açsın).
- [ ] Mobil uygulamada (Expo/React Native) akıllı tahta açma ekranının altına rahatsız etmeyen küçük bir banner veya "Geliştiriciye Destek Ol" alanı koy.

### 4. Büyüme & Kurumsallaşma (Gelir Düzenli Hale Geldiğinde)
- [ ] İlk ciddi ve düzenli gelir akışı başladığında vergi dairesinden GVK 20/B İstisna Belgesi al.
- [ ] SignPath.io vakfına başvurarak ücretsiz resmi Kod İmzalama Sertifikasını (Code Signing) entegre et.
