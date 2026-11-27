// master/session/SessionManagerAudio.cpp
// Ses kaydı, anons işleme/temizleme (DC Blocker + AGC), ses dosyası WAV normalizasyonu,
// canlı mikrofon yayını ve ses komutları metodlarını içerir.

#include "SessionManager.h"
#include "../common/utils/Logger.h"
#include "../common/db/OkidiaDatabase.h"
#include "../transfer/FileSender.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QProcess>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <QUrl>
#include <QtEndian>
#include <QDataStream>
#include <QAudioSource>
#include <QAudioFormat>
#include <QAudioDecoder>
#include <QAudioBuffer>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QStandardPaths>
#include <QUuid>
#include <cmath>
#include <algorithm>
#include <vector>

namespace {

constexpr int kTargetSampleRate = 24000;

// Herhangi bir QAudioFormat içindeki ham PCM bloğunu Mono Float [-1.0, 1.0] dizisine çevirir
static std::vector<float> convertRawPcmToMonoFloat(const char* rawData, int byteSize, const QAudioFormat& fmt) {
    const int channels = qMax(1, fmt.channelCount());
    const QAudioFormat::SampleFormat sfmt = fmt.sampleFormat();
    int bytesPerSample = 2;
    switch (sfmt) {
    case QAudioFormat::UInt8: bytesPerSample = 1; break;
    case QAudioFormat::Int16: bytesPerSample = 2; break;
    case QAudioFormat::Int32:
    case QAudioFormat::Float: bytesPerSample = 4; break;
    default: bytesPerSample = 2; break;
    }

    const int frameBytes = channels * bytesPerSample;
    if (frameBytes <= 0 || byteSize < frameBytes) return {};

    const int frameCount = byteSize / frameBytes;
    std::vector<float> out(frameCount, 0.0f);
    const uchar* ptr = reinterpret_cast<const uchar*>(rawData);

    for (int f = 0; f < frameCount; ++f) {
        float sum = 0.0f;
        for (int c = 0; c < channels; ++c) {
            const uchar* sPtr = ptr + (f * channels + c) * bytesPerSample;
            float v = 0.0f;
            if (sfmt == QAudioFormat::Int16) {
                qint16 s = qFromLittleEndian<qint16>(sPtr);
                v = static_cast<float>(s) / 32768.0f;
            } else if (sfmt == QAudioFormat::Int32) {
                qint32 s = qFromLittleEndian<qint32>(sPtr);
                v = static_cast<float>(static_cast<double>(s) / 2147483648.0);
            } else if (sfmt == QAudioFormat::Float) {
                float s = qFromLittleEndian<float>(sPtr);
                if (std::isnan(s) || std::isinf(s)) s = 0.0f;
                v = qBound(-1.0f, s, 1.0f);
            } else if (sfmt == QAudioFormat::UInt8) {
                v = (static_cast<int>(*sPtr) - 128) / 128.0f;
            }
            sum += v;
        }
        out[f] = sum / static_cast<float>(channels);
    }
    return out;
}

// Doğrusal enterpolasyonlu örnekleme hızı dönüştürücü (Resampler)
static std::vector<float> resampleMonoFloat(const std::vector<float>& in, int inRate, int outRate) {
    if (in.empty() || inRate <= 0 || outRate <= 0) return {};
    if (inRate == outRate) return in;

    const double ratio = static_cast<double>(inRate) / static_cast<double>(outRate);
    const size_t outCount = static_cast<size_t>(std::max(1.0, std::floor(in.size() / ratio)));
    std::vector<float> out(outCount);

    for (size_t i = 0; i < outCount; ++i) {
        const double srcPos = i * ratio;
        const size_t idx0 = static_cast<size_t>(srcPos);
        const size_t idx1 = std::min(idx0 + 1, in.size() - 1);
        const float frac = static_cast<float>(srcPos - idx0);
        out[i] = in[idx0] * (1.0f - frac) + in[idx1] * frac;
    }
    return out;
}

// Gerçek zamanlı DC Blocker + Adaptif AGC + Yumuşak Limiter + VU Seviye Hesaplayıcı
// AMD ACP dijital mikrofonlardaki -8179 DC offset ve -32 dBFS düşük kazanç sorununu çözer.
static QByteArray processLiveMicChunk(const char* rawData,
                                      int byteSize,
                                      const QAudioFormat& inFmt,
                                      int targetRate,
                                      float& dcPrevIn,
                                      float& dcPrevOut,
                                      float& agcGain,
                                      int* outLevelPercent) {
    std::vector<float> mono = convertRawPcmToMonoFloat(rawData, byteSize, inFmt);
    if (mono.empty()) {
        if (outLevelPercent) *outLevelPercent = 0;
        return {};
    }

    // 1. High-Pass DC Blocker: y[n] = x[n] - x[n-1] + R * y[n-1] (fc ≈ 18 Hz)
    const float R = 0.995f;
    float peakAbs = 0.0f;
    double sumSq = 0.0;

    for (size_t i = 0; i < mono.size(); ++i) {
        const float x = mono[i];
        const float y = x - dcPrevIn + R * dcPrevOut;
        dcPrevIn = x;
        dcPrevOut = y;
        mono[i] = y;
        const float ay = std::fabs(y);
        if (ay > peakAbs) peakAbs = ay;
        sumSq += static_cast<double>(y) * static_cast<double>(y);
    }

    const float rms = static_cast<float>(std::sqrt(sumSq / mono.size()));

    // 2. Adaptif AGC (Konuşma varken hedef RMS ≈ 0.20 / Peak ≈ 0.70; gürültü tabanı < 0.002 iken kazancı sabitle)
    if (rms > 0.0025f && peakAbs > 0.004f) {
        const float desiredByRms = 0.22f / rms;
        const float maxByPeak    = 0.85f / peakAbs;
        const float targetGain   = qBound(1.0f, std::min(desiredByRms, maxByPeak), 18.0f);
        // Hızlı attack (clipping önleme), yumuşak release
        const float alpha = (targetGain < agcGain) ? 0.35f : 0.08f;
        agcGain = agcGain * (1.0f - alpha) + targetGain * alpha;
    }

    // 3. Resample → targetRate (24000 Hz)
    std::vector<float> resampled = resampleMonoFloat(mono, inFmt.sampleRate(), targetRate);

    // 4. Kazanç + Yumuşak Limiter + Int16 Little-Endian Paketleme
    QByteArray outBytes(static_cast<int>(resampled.size() * sizeof(qint16)), Qt::Uninitialized);
    uchar* dst = reinterpret_cast<uchar*>(outBytes.data());
    double outSumSq = 0.0;

    for (size_t i = 0; i < resampled.size(); ++i) {
        float v = resampled[i] * agcGain;
        // Yumuşak doygunluk (soft knee limiter)
        if (v > 0.85f) {
            v = 0.85f + 0.15f * std::tanh((v - 0.85f) / 0.15f);
        } else if (v < -0.85f) {
            v = -0.85f - 0.15f * std::tanh((-v - 0.85f) / 0.15f);
        }
        v = qBound(-0.99f, v, 0.99f);
        outSumSq += static_cast<double>(v) * static_cast<double>(v);
        const qint16 s16 = static_cast<qint16>(std::lrint(v * 32767.0f));
        qToLittleEndian<qint16>(s16, dst + i * sizeof(qint16));
    }

    if (outLevelPercent && !resampled.empty()) {
        const float outRms = static_cast<float>(std::sqrt(outSumSq / resampled.size()));
        // Logaritmik VU ölçeği (-45 dBFS → %0, -3 dBFS → %100)
        if (outRms <= 0.003f) {
            *outLevelPercent = 0;
        } else {
            const float db = 20.0f * std::log10(outRms);
            const int pct = qRound(((db + 45.0f) / 42.0f) * 100.0f);
            *outLevelPercent = qBound(0, pct, 100);
        }
    }

    return outBytes;
}

static bool writeStandardMono16Wav(const QString& outPath, const QByteArray& pcmData, int sampleRate) {
    QFile file(outPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    const quint32 dataSize = static_cast<quint32>(pcmData.size());
    const quint32 sr = static_cast<quint32>(sampleRate);
    const quint16 channels = 1;
    const quint16 bitsPerSample = 16;
    const quint16 blockAlign = channels * (bitsPerSample / 8);
    const quint32 byteRate = sr * blockAlign;

    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);

    file.write("RIFF", 4);
    out << quint32(dataSize + 36);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    out << quint32(16);
    out << quint16(1);
    out << quint16(channels);
    out << quint32(sr);
    out << quint32(byteRate);
    out << quint16(blockAlign);
    out << quint16(bitsPerSample);
    file.write("data", 4);
    out << quint32(dataSize);

    file.write(pcmData);
    file.close();
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// WAV Temizleme, DC Offset Giderme ve Peak/RMS Normalizasyon Yardımcısı
// ---------------------------------------------------------------------------
static void cleanRecordedWav(const QString& wavPath) {
    QFile file(wavPath);
    if (!file.open(QIODevice::ReadWrite)) {
        return;
    }

    QByteArray content = file.readAll();
    if (content.size() < 44) return;
    if (!content.startsWith("RIFF") || content.mid(8, 4) != "WAVE") return;

    int idx = 12;
    int fmtOffset = 0;
    int dataOffset = 0;
    quint32 dataSize = 0;
    quint16 audioFormat = 0;
    quint16 numChannels = 0;
    quint32 sampleRate = 0;
    quint16 blockAlign = 0;
    quint16 bitsPerSample = 0;

    while (idx + 8 <= content.size()) {
        QByteArray chunkId = content.mid(idx, 4);
        quint32 chunkSz = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(content.constData() + idx + 4));

        if (chunkId == "fmt " && chunkSz >= 16) {
            fmtOffset = idx + 8;
            audioFormat = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(content.constData() + fmtOffset));
            numChannels = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(content.constData() + fmtOffset + 2));
            sampleRate = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(content.constData() + fmtOffset + 4));
            blockAlign = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(content.constData() + fmtOffset + 12));
            bitsPerSample = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(content.constData() + fmtOffset + 14));
        } else if (chunkId == "data") {
            dataOffset = idx + 8;
            dataSize = chunkSz;
            break;
        }

        idx += 8 + chunkSz;
    }

    if (dataOffset <= 0 || audioFormat != 1 || bitsPerSample != 16 || blockAlign == 0 || numChannels == 0 || sampleRate == 0) {
        return;
    }

    if (dataOffset + static_cast<int>(dataSize) > content.size()) {
        dataSize = static_cast<quint32>(content.size() - dataOffset);
    }

    // Donanım başlatma pikini (turn-on transient / click) yok etmek için ilk 80ms'yi atla
    int skipFrames = static_cast<int>(sampleRate * 0.08);
    int skipBytes = skipFrames * blockAlign;
    if (static_cast<int>(dataSize) <= skipBytes + static_cast<int>(sampleRate * 0.12) * blockAlign) {
        skipBytes = 0;
    }

    int newDataSize = static_cast<int>(dataSize) - skipBytes;
    char* dataPtr = content.data() + dataOffset;
    if (skipBytes > 0) {
        memmove(dataPtr, dataPtr + skipBytes, newDataSize);
    }

    const int totalSamples = newDataSize / sizeof(qint16);
    qint16* samples = reinterpret_cast<qint16*>(dataPtr);

    // 1. İkinci geçiş DC ortalama temizliği + tepe/RMS analizi
    double sumVal = 0.0;
    for (int i = 0; i < totalSamples; ++i) {
        qint16 v = qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(&samples[i]));
        sumVal += v;
    }
    const double meanVal = (totalSamples > 0) ? (sumVal / totalSamples) : 0.0;

    double peakAbs = 1.0;
    double sumSq = 0.0;
    for (int i = 0; i < totalSamples; ++i) {
        qint16 v = qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(&samples[i]));
        double ac = static_cast<double>(v) - meanVal;
        if (std::fabs(ac) > peakAbs) peakAbs = std::fabs(ac);
        sumSq += ac * ac;
    }
    const double acRms = (totalSamples > 0) ? std::sqrt(sumSq / totalSamples) : 0.0;

    // 2. Peak/RMS Normalizasyon (eğer kayıtta gerçek ses varsa -2 dBFS tepe / -12 dBFS RMS hedefle)
    double normGain = 1.0;
    if (acRms > 40.0 && peakAbs > 120.0) {
        const double gainByPeak = 27000.0 / peakAbs;
        const double gainByRms  = 7500.0 / acRms;
        normGain = qBound(1.0, std::min(gainByPeak, gainByRms), 16.0);
    }

    for (int i = 0; i < totalSamples; ++i) {
        qint16 v = qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(&samples[i]));
        double ac = (static_cast<double>(v) - meanVal) * normGain;
        qint16 cleanVal = static_cast<qint16>(qBound(-32760.0, std::round(ac), 32760.0));
        qToLittleEndian<qint16>(cleanVal, reinterpret_cast<uchar*>(&samples[i]));
    }

    // 3. İlk 30ms fade-in ve son 30ms fade-out
    const int totalFrames = newDataSize / blockAlign;
    const int fadeFrames = qMin(static_cast<int>(sampleRate * 0.030), totalFrames / 4);
    for (int f = 0; f < fadeFrames; ++f) {
        const float gain = static_cast<float>(f) / static_cast<float>(qMax(1, fadeFrames));
        for (int c = 0; c < numChannels; ++c) {
            int sIn = f * numChannels + c;
            qint16 vIn = qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(&samples[sIn]));
            qToLittleEndian<qint16>(static_cast<qint16>(vIn * gain), reinterpret_cast<uchar*>(&samples[sIn]));

            int sOut = (totalFrames - 1 - f) * numChannels + c;
            qint16 vOut = qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(&samples[sOut]));
            qToLittleEndian<qint16>(static_cast<qint16>(vOut * gain), reinterpret_cast<uchar*>(&samples[sOut]));
        }
    }

    qToLittleEndian<quint32>(static_cast<quint32>(newDataSize), reinterpret_cast<uchar*>(content.data() + dataOffset - 4));
    quint32 riffSize = (dataOffset - 8) + newDataSize;
    qToLittleEndian<quint32>(riffSize, reinterpret_cast<uchar*>(content.data() + 4));

    file.seek(0);
    file.write(content.constData(), dataOffset + newDataSize);
    file.resize(dataOffset + newDataSize);
    file.close();

    Logger::info(QObject::tr("SessionManager: Ses kaydı temizlendi (DC=%1 giderildi, Kazanç=x%2, 30ms fade-in/out).")
                 .arg(qRound(meanVal))
                 .arg(normGain, 0, 'f', 1));
}

// ---------------------------------------------------------------------------
// normalizeAudioFileToWav: MP3/OGG/M4A/FLAC/WAV → 24kHz 16-bit Mono PCM WAV
// ---------------------------------------------------------------------------
QString SessionManager::normalizeAudioFileToWav(const QString& inputPath, QString* errorOut) {
    if (inputPath.isEmpty() || !QFile::exists(inputPath)) {
        if (errorOut) *errorOut = QObject::tr("Ses dosyası bulunamadı.");
        return {};
    }

    // Eğer dosya zaten geçerli bir 16-bit PCM WAV ise ve boyutu makulse doğrudan DC/format kontrolü yap
    {
        QFile f(inputPath);
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray hdr = f.read(44);
            f.close();
            if (hdr.size() >= 44 && hdr.startsWith("RIFF") && hdr.mid(8, 4) == "WAVE") {
                const quint16 fmtTag = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(hdr.constData() + 20));
                const quint16 ch     = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(hdr.constData() + 22));
                const quint32 sr     = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(hdr.constData() + 24));
                const quint16 bits   = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(hdr.constData() + 34));
                if (fmtTag == 1 && bits == 16 && (ch == 1 || ch == 2) && sr >= 8000 && sr <= 96000) {
                    return inputPath;
                }
            }
        }
    }

    QString outWavPath;
    {
        QTemporaryFile tmp(QDir::tempPath() + "/okidia_norm_XXXXXX.wav");
        tmp.setAutoRemove(false);
        if (!tmp.open()) {
            if (errorOut) *errorOut = QObject::tr("Geçici WAV dosyası oluşturulamadı.");
            return {};
        }
        outWavPath = tmp.fileName();
        tmp.close();
    }

    // 1. Önce varsa harici hızlı CLI dönüştürücüleri (ffmpeg / gst-launch-1.0 / mpg123) dene
    const QString ffmpegBin = QStandardPaths::findExecutable("ffmpeg");
    if (!ffmpegBin.isEmpty()) {
        QProcess p;
        p.start(ffmpegBin, {"-y", "-i", inputPath, "-vn", "-ac", "1", "-ar", QString::number(kTargetSampleRate), "-sample_fmt", "s16", "-f", "wav", outWavPath});
        if (p.waitForFinished(30000) && p.exitCode() == 0 && QFileInfo(outWavPath).size() > 100) {
            Logger::info(QObject::tr("SessionManager: Ses dosyası ffmpeg ile standart WAV'a dönüştürüldü → ") + outWavPath);
            return outWavPath;
        }
    }

    // 2. Saf Qt6 QAudioDecoder (FFmpeg / GStreamer / WMF / CoreAudio backend) ile süreç içi PCM çözme
    QAudioFormat targetFmt;
    targetFmt.setSampleRate(kTargetSampleRate);
    targetFmt.setChannelCount(1);
    targetFmt.setSampleFormat(QAudioFormat::Int16);

    QAudioDecoder decoder;
    decoder.setAudioFormat(targetFmt);
    decoder.setSource(QUrl::fromLocalFile(inputPath));

    QByteArray decodedPcm;
    decodedPcm.reserve(kTargetSampleRate * 2 * 30);

    QEventLoop loop;
    bool decodeError = false;
    QString errMsg;

    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, [&]() {
        while (decoder.bufferAvailable()) {
            const QAudioBuffer buf = decoder.read();
            if (!buf.isValid() || buf.byteCount() <= 0) continue;
            const QAudioFormat bFmt = buf.format();
            if (bFmt.sampleRate() == kTargetSampleRate &&
                bFmt.channelCount() == 1 &&
                bFmt.sampleFormat() == QAudioFormat::Int16) {
                decodedPcm.append(buf.constData<char>(), buf.byteCount());
            } else {
                std::vector<float> mono = convertRawPcmToMonoFloat(buf.constData<char>(), buf.byteCount(), bFmt);
                std::vector<float> res  = resampleMonoFloat(mono, bFmt.sampleRate(), kTargetSampleRate);
                const int oldSz = decodedPcm.size();
                decodedPcm.resize(oldSz + static_cast<int>(res.size() * sizeof(qint16)));
                uchar* dst = reinterpret_cast<uchar*>(decodedPcm.data() + oldSz);
                for (size_t i = 0; i < res.size(); ++i) {
                    const float v = qBound(-1.0f, res[i], 1.0f);
                    const qint16 s = static_cast<qint16>(std::lrint(v * 32767.0f));
                    qToLittleEndian<qint16>(s, dst + i * sizeof(qint16));
                }
            }
        }
    });

    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, &QEventLoop::quit);
    QObject::connect(&decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error),
                     &loop, [&](QAudioDecoder::Error) {
        decodeError = true;
        errMsg = decoder.errorString();
        loop.quit();
    });

    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]() {
        decodeError = true;
        errMsg = QObject::tr("Ses çözme zaman aşımına uğradı.");
        loop.quit();
    });
    timeoutTimer.start(30000);

    decoder.start();
    loop.exec();
    decoder.stop();

    if (!decodedPcm.isEmpty() && writeStandardMono16Wav(outWavPath, decodedPcm, kTargetSampleRate)) {
        Logger::info(QObject::tr("SessionManager: Ses dosyası QAudioDecoder ile standart 24kHz WAV'a dönüştürüldü → %1 (%2 KB)")
                     .arg(outWavPath)
                     .arg(decodedPcm.size() / 1024));
        return outWavPath;
    }

    // 3. Yedek: GStreamer gst-launch-1.0 CLI
    const QString gstBin = QStandardPaths::findExecutable("gst-launch-1.0");
    if (!gstBin.isEmpty()) {
        QProcess p;
        p.start(gstBin, {"-q", "filesrc", "location=" + inputPath, "!", "decodebin", "!", "audioconvert", "!", "audioresample",
                         "!", QString("audio/x-raw,format=S16LE,channels=1,rate=%1").arg(kTargetSampleRate),
                         "!", "wavenc", "!", "filesink", "location=" + outWavPath});
        if (p.waitForFinished(30000) && p.exitCode() == 0 && QFileInfo(outWavPath).size() > 100) {
            Logger::info(QObject::tr("SessionManager: Ses dosyası GStreamer ile standart WAV'a dönüştürüldü → ") + outWavPath);
            return outWavPath;
        }
    }

    QFile::remove(outWavPath);
    Logger::warning(QObject::tr("SessionManager: Ses dosyası WAV'a dönüştürülemedi (%1), orijinal dosya gönderilecek: %2")
                    .arg(errMsg, inputPath));
    return inputPath;
}

// ---------------------------------------------------------------------------
// startRecording (Canlı DC Blocker + AGC + VU Seviye Ölçümü ile)
// ---------------------------------------------------------------------------
bool SessionManager::startRecording(const QAudioDevice& device) {
    if (m_audioSource) return false;

    {
        QTemporaryFile tmp(QDir::tempPath() + "/okidia_anons_XXXXXX.wav");
        tmp.setAutoRemove(false);
        if (!tmp.open()) return false;
        m_audioWavPath = tmp.fileName();
    }

    QAudioDevice targetDev = device;
    if (targetDev.isNull()) {
        targetDev = QMediaDevices::defaultAudioInput();
    }

    if (targetDev.isNull()) {
        Logger::error(QObject::tr("SessionManager: Kullanılabilir ses giriş aygıtı (mikrofon) bulunamadı!"));
        return false;
    }

    // İstenen mikrofon formatı: 24 kHz veya cihazın doğal formatı
    QAudioFormat format;
    format.setSampleRate(kTargetSampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    if (!targetDev.isFormatSupported(format)) {
        QAudioFormat altFmt;
        altFmt.setSampleRate(44100);
        altFmt.setChannelCount(1);
        altFmt.setSampleFormat(QAudioFormat::Int16);
        if (targetDev.isFormatSupported(altFmt)) {
            format = altFmt;
        } else {
            format = targetDev.preferredFormat();
        }
    }
    m_recordFormat = format;

    m_audioRecordFile = new QFile(m_audioWavPath, this);
    if (!m_audioRecordFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Logger::error(QObject::tr("SessionManager: Ses dosyası açılamadı: ") + m_audioWavPath);
        delete m_audioRecordFile;
        m_audioRecordFile = nullptr;
        return false;
    }

    // 44 baytlık WAV başlığı yerini boşlukla rezerve et (çıktı her zaman kTargetSampleRate 24000 Hz 1ch Int16)
    const QByteArray dummyHeader(44, 0);
    m_audioRecordFile->write(dummyHeader);

    m_recDcPrevIn  = 0.0f;
    m_recDcPrevOut = 0.0f;
    m_recAgcGain   = 4.0f;

    m_audioSource = new QAudioSource(targetDev, m_recordFormat, this);
    const int bufBytes = m_recordFormat.bytesForDuration(60000);
    if (bufBytes > 0) m_audioSource->setBufferSize(bufBytes);

    m_recordIoDevice = m_audioSource->start();
    if (!m_recordIoDevice || m_audioSource->error() != QAudio::NoError) {
        Logger::error(QObject::tr("SessionManager: Mikrofon kaydı başlatılamadı: %1")
                     .arg(m_audioSource ? static_cast<int>(m_audioSource->error()) : -1));
        if (m_audioSource) {
            m_audioSource->stop();
            delete m_audioSource;
            m_audioSource = nullptr;
        }
        m_recordIoDevice = nullptr;
        m_audioRecordFile->close();
        delete m_audioRecordFile;
        m_audioRecordFile = nullptr;
        return false;
    }

    connect(m_recordIoDevice, &QIODevice::readyRead, this, [this]() {
        if (!m_recordIoDevice || !m_audioRecordFile) return;
        const QByteArray raw = m_recordIoDevice->readAll();
        if (raw.isEmpty()) return;

        int levelPct = 0;
        const QByteArray cleanPcm = processLiveMicChunk(
            raw.constData(), raw.size(), m_recordFormat, kTargetSampleRate,
            m_recDcPrevIn, m_recDcPrevOut, m_recAgcGain, &levelPct);

        if (!cleanPcm.isEmpty()) {
            m_audioRecordFile->write(cleanPcm);
        }
        emit audioInputLevelChanged(levelPct);
    });

    Logger::info(QObject::tr("SessionManager: Kayıt başladı (Saf Qt6 QAudioSource + DC/AGC) → Aygıt: %1 | Giriş: %2Hz %3ch → Çıkış: %4Hz 1ch | Dosya: %5")
                 .arg(targetDev.description())
                 .arg(m_recordFormat.sampleRate())
                 .arg(m_recordFormat.channelCount())
                 .arg(kTargetSampleRate)
                 .arg(m_audioWavPath));
    return true;
}

// ---------------------------------------------------------------------------
// stopRecording
// ---------------------------------------------------------------------------
QString SessionManager::stopRecording() {
    if (!m_audioSource || !m_audioRecordFile) return {};

    if (m_recordIoDevice) {
        const QByteArray remaining = m_recordIoDevice->readAll();
        if (!remaining.isEmpty()) {
            const QByteArray cleanPcm = processLiveMicChunk(
                remaining.constData(), remaining.size(), m_recordFormat, kTargetSampleRate,
                m_recDcPrevIn, m_recDcPrevOut, m_recAgcGain, nullptr);
            if (!cleanPcm.isEmpty()) {
                m_audioRecordFile->write(cleanPcm);
            }
        }
    }

    m_audioSource->stop();
    delete m_audioSource;
    m_audioSource = nullptr;
    m_recordIoDevice = nullptr;
    emit audioInputLevelChanged(0);

    // Dosyayı flush et ve standart 24kHz 16-bit Mono WAV (RIFF) başlığını yaz
    m_audioRecordFile->flush();
    const qint64 totalSize = m_audioRecordFile->size();
    const quint32 dataSize = (totalSize > 44) ? static_cast<quint32>(totalSize - 44) : 0;

    const quint32 sampleRate = static_cast<quint32>(kTargetSampleRate);
    const quint16 channels   = 1;
    const quint16 bitsPerSample = 16;
    const quint16 blockAlign = channels * (bitsPerSample / 8);
    const quint32 byteRate   = sampleRate * blockAlign;

    m_audioRecordFile->seek(0);
    QDataStream out(m_audioRecordFile);
    out.setByteOrder(QDataStream::LittleEndian);

    m_audioRecordFile->write("RIFF", 4);
    out << quint32(dataSize + 36);
    m_audioRecordFile->write("WAVE", 4);

    m_audioRecordFile->write("fmt ", 4);
    out << quint32(16);
    out << quint16(1);
    out << quint16(channels);
    out << quint32(sampleRate);
    out << quint32(byteRate);
    out << quint16(blockAlign);
    out << quint16(bitsPerSample);

    m_audioRecordFile->write("data", 4);
    out << quint32(dataSize);

    m_audioRecordFile->close();
    delete m_audioRecordFile;
    m_audioRecordFile = nullptr;

    cleanRecordedWav(m_audioWavPath);

    qint64 sz = QFileInfo(m_audioWavPath).size();
    Logger::info(QObject::tr("SessionManager: Kayıt tamamlandı → %1 (%2 KB)")
                 .arg(m_audioWavPath)
                 .arg(sz / 1024));
    return m_audioWavPath;
}

// ---------------------------------------------------------------------------
// Anons — WAV Normalizasyon → file transfer → /tmp → AUDIO_PLAY_LOCAL
// ---------------------------------------------------------------------------
void SessionManager::sendAnons(const QString& wavPath,
                                const QStringList& targets,
                                int repeatCount) {
    if (targets.isEmpty()) return;

    if (!m_conn || !m_conn->isConnected()) {
        Logger::error(QObject::tr("SessionManager::sendAnons: Broker bağlantısı yok!"));
        return;
    }

    // İdari Bilgisayarları (Tip 2) anonstan tamamen soyutla
    QStringList filteredTargets;
    for (const QString& id : targets) {
        if (OkidiaDatabase::instance().isOpen()) {
            auto rec = OkidiaDatabase::instance().agent(id);
            if (rec.agentType == 2) {
                continue;
            }
        }
        filteredTargets << id;
    }

    if (filteredTargets.isEmpty()) {
        Logger::info(QObject::tr("SessionManager::sendAnons: Hedef listede anons yapılacak istemci kalmadı (İdari bilgisayarlar filtrelendi)."));
        return;
    }

    // Önceki aktif anons yüklemesi varsa iptal et
    if (m_activeAnonsSender) {
        m_activeAnonsSender->cancel();
        m_activeAnonsSender->deleteLater();
        m_activeAnonsSender = nullptr;
    }

    // Yöntem 2: Gönderilecek ses dosyasını (MP3/OGG/M4A/WAV) standart 24 kHz 16-bit Mono PCM WAV'a normalize et
    QString errStr;
    const QString normalizedPath = normalizeAudioFileToWav(wavPath, &errStr);
    const QString sendPath = normalizedPath.isEmpty() ? wavPath : normalizedPath;
    const bool isTempNormalized = (sendPath != wavPath && sendPath.startsWith(QDir::tempPath()));

    // Tahmini çalma süresini hesapla (WAV boyutu / 48000 bayt/sn * repeat + 8sn ding-dong ve ağ marjı)
    const qint64 fileBytes = QFileInfo(sendPath).size();
    const int estSingleSec = qMax(3, static_cast<int>(fileBytes / (kTargetSampleRate * 2)));
    const int totalWaitMs  = (estSingleSec * qMax(1, repeatCount) + 12) * 1000;

    m_activeAnnounceId = QUuid::createUuid().toString(QUuid::Id128).left(12);
    m_activeAnnounceTargets = filteredTargets;

    // Multicast anons hedef formatı: __multicast_anons__:id1,id2,id3...?repeat=X&id=Y
    QString multicastTarget = QString("__multicast_anons__:%1?repeat=%2&id=%3")
                              .arg(filteredTargets.join(","))
                              .arg(repeatCount)
                              .arg(m_activeAnnounceId);

    QString overrideName = QString("okidia_anons_%1.wav").arg(m_activeAnnounceId);
    if (!sendPath.endsWith(".wav", Qt::CaseInsensitive)) {
        overrideName = QString("okidia_anons_%1.%2").arg(m_activeAnnounceId, QFileInfo(sendPath).suffix().toLower());
    }

    FileSender* sender = new FileSender(m_conn, this);
    m_activeAnonsSender = sender;

    emit announceActiveStateChanged(true, QFileInfo(wavPath).fileName());

    if (!m_announceAutoResetTimer) {
        m_announceAutoResetTimer = new QTimer(this);
        m_announceAutoResetTimer->setSingleShot(true);
        connect(m_announceAutoResetTimer, &QTimer::timeout, this, [this]() {
            if (!m_activeAnnounceId.isEmpty() && !m_liveAudioSource) {
                m_activeAnnounceId.clear();
                m_activeAnnounceTargets.clear();
                emit announceActiveStateChanged(false, QString());
            }
        });
    }
    m_announceAutoResetTimer->start(totalWaitMs);

    connect(sender, &FileSender::finished, this, [this, sender, sendPath, isTempNormalized](bool ok, const QString& msg) {
        if (m_activeAnonsSender == sender) {
            m_activeAnonsSender = nullptr;
        }
        if (isTempNormalized) {
            QFile::remove(sendPath);
        }
        if (ok) {
            Logger::info(QObject::tr("SessionManager::sendAnons: Asenkron multicast anons yüklemesi tamamlandı → ") + msg);
        } else {
            Logger::error(QObject::tr("SessionManager::sendAnons: Asenkron multicast anons yüklemesi başarısız → ") + msg);
        }
        sender->deleteLater();
    });

    if (!sender->sendToClient(multicastTarget, sendPath, false, "tmp", overrideName)) {
        Logger::error(QObject::tr("SessionManager::sendAnons: Transfer başlatılamadı!"));
        if (m_activeAnonsSender == sender) m_activeAnonsSender = nullptr;
        if (isTempNormalized) QFile::remove(sendPath);
        sender->deleteLater();
        m_activeAnnounceId.clear();
        m_activeAnnounceTargets.clear();
        if (m_announceAutoResetTimer) m_announceAutoResetTimer->stop();
        emit announceActiveStateChanged(false, QString());
    }
}

// ---------------------------------------------------------------------------
// broadcastAudioFile
// ---------------------------------------------------------------------------
bool SessionManager::broadcastAudioFile(const QString& filePath,
                                        const QStringList& targets,
                                        int repeatCount) {
    QStringList finalTargets = targets.isEmpty() ? m_clientStates.keys() : targets;
    if (finalTargets.isEmpty()) {
        Logger::warning(QObject::tr("SessionManager::broadcastAudioFile: Çevrimiçi hedef bulunamadı!"));
        return false;
    }

    sendAnons(filePath, finalTargets, repeatCount);
    return true;
}

// ---------------------------------------------------------------------------
// broadcastAudioStop
// ---------------------------------------------------------------------------
bool SessionManager::broadcastAudioStop() {
    stopRecording();

    if (m_activeAnonsSender) {
        m_activeAnonsSender->cancel();
        m_activeAnonsSender->deleteLater();
        m_activeAnonsSender = nullptr;
    }
    if (m_announceAutoResetTimer) {
        m_announceAutoResetTimer->stop();
    }

    const QString stopId = !m_activeAnnounceId.isEmpty() ? m_activeAnnounceId : m_liveAnnounceId;
    const QStringList targetList = !m_activeAnnounceTargets.isEmpty() ? m_activeAnnounceTargets : m_liveBroadcastTargets;

    m_activeAnnounceId.clear();
    m_activeAnnounceTargets.clear();

    if (m_liveAudioSource) {
        m_liveAudioSource->stop();
        delete m_liveAudioSource;
        m_liveAudioSource = nullptr;
        m_liveIoDevice = nullptr;
        m_liveBroadcastTargets.clear();
        m_liveAnnounceId.clear();
        emit audioInputLevelChanged(0);
    }

    okidia::AudioBroadcastCommand stopCmd;
    if (!stopId.isEmpty()) {
        stopCmd.set_announce_id(stopId.toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::AUDIO_BROADCAST_STOP);
    env.set_sequence_id(0);
    env.set_payload(stopCmd.SerializeAsString());

    // Önce Broker'daki bekleyen multicast görevlerini iptal etmek ve hedeflere STOP göndermek için
    bool ok = true;
    if (targetList.isEmpty()) {
        ok = broadcastCommand(env);
    } else {
        // Broker'ın MulticastManager::cancelAnnounceTasks çağrısını tetiklemek için ilk hedefe ve tüm hedeflere gönder
        for (const QString& tid : targetList) {
            sendCommand(tid, env);
        }
    }

    emit announceActiveStateChanged(false, QString());
    Logger::info(QObject::tr("SessionManager: Anons ve canlı ses yayını durdurma komutu gönderildi (id=%1)")
                 .arg(stopId.isEmpty() ? QObject::tr("Tümü") : stopId));
    return ok;
}

// ---------------------------------------------------------------------------
// Canlı Ses Yayını (Live Audio Streaming / Push-to-Talk)
// ---------------------------------------------------------------------------
bool SessionManager::startLiveAudioBroadcast(const QStringList& targets, const QAudioDevice& device) {
    if (m_liveAudioSource) return false;

    QAudioDevice targetDev = device;
    if (targetDev.isNull()) {
        targetDev = QMediaDevices::defaultAudioInput();
    }
    if (targetDev.isNull()) {
        Logger::error(QObject::tr("SessionManager: Canlı ses için mikrofon bulunamadı!"));
        return false;
    }

    QAudioFormat format;
    format.setSampleRate(kTargetSampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    if (!targetDev.isFormatSupported(format)) {
        QAudioFormat altFmt;
        altFmt.setSampleRate(44100);
        altFmt.setChannelCount(1);
        altFmt.setSampleFormat(QAudioFormat::Int16);
        if (targetDev.isFormatSupported(altFmt)) {
            format = altFmt;
        } else {
            format = targetDev.preferredFormat();
        }
    }
    m_liveFormat = format;
    m_liveBroadcastTargets = targets;
    m_liveAnnounceId = QUuid::createUuid().toString(QUuid::Id128).left(12);
    m_liveSeqCounter = 0;
    m_liveDcPrevIn   = 0.0f;
    m_liveDcPrevOut  = 0.0f;
    m_liveAgcGain    = 4.0f;

    m_liveAudioSource = new QAudioSource(targetDev, m_liveFormat, this);
    const int bufferBytes = m_liveFormat.bytesForDuration(60000);
    if (bufferBytes > 0) m_liveAudioSource->setBufferSize(bufferBytes);

    m_liveIoDevice = m_liveAudioSource->start();
    if (!m_liveIoDevice || m_liveAudioSource->error() != QAudio::NoError) {
        Logger::error(QObject::tr("SessionManager: Canlı ses aygıtı başlatılamadı"));
        delete m_liveAudioSource;
        m_liveAudioSource = nullptr;
        m_liveIoDevice = nullptr;
        return false;
    }

    emit announceActiveStateChanged(true, QObject::tr("Canlı Mikrofon"));

    connect(m_liveIoDevice, &QIODevice::readyRead, this, [this]() {
        if (!m_liveIoDevice || !m_liveAudioSource) return;
        const QByteArray raw = m_liveIoDevice->readAll();
        if (raw.isEmpty()) return;

        int levelPct = 0;
        const QByteArray pcm = processLiveMicChunk(
            raw.constData(), raw.size(), m_liveFormat, kTargetSampleRate,
            m_liveDcPrevIn, m_liveDcPrevOut, m_liveAgcGain, &levelPct);

        emit audioInputLevelChanged(levelPct);
        if (pcm.isEmpty()) return;

        okidia::AudioBroadcastCommand cmd;
        cmd.set_source(okidia::AudioBroadcastCommand::LIVE);
        cmd.set_audio_data(pcm.constData(), pcm.size());
        cmd.set_sample_rate(static_cast<uint32_t>(kTargetSampleRate));
        cmd.set_channels(1);
        cmd.set_volume(0);
        cmd.set_announce_id(m_liveAnnounceId.toStdString());
        cmd.set_seq(++m_liveSeqCounter);

        okidia::Envelope env;
        env.set_type(okidia::MessageType::AUDIO_BROADCAST_LIVE);
        env.set_payload(cmd.SerializeAsString());

        if (m_liveBroadcastTargets.isEmpty()) {
            broadcastCommand(env);
        } else {
            for (const auto& targetId : m_liveBroadcastTargets) {
                sendCommand(targetId, env);
            }
        }
    });

    Logger::info(QObject::tr("SessionManager: Canlı anons yayını başladı (DC/AGC aktif) → Aygıt: %1 | Giriş: %2Hz %3ch → Yayın: %4Hz 1ch | Hedef: %5")
                 .arg(targetDev.description())
                 .arg(m_liveFormat.sampleRate())
                 .arg(m_liveFormat.channelCount())
                 .arg(kTargetSampleRate)
                 .arg(targets.isEmpty() ? tr("Tüm İstemciler") : QString::number(targets.size()) + tr(" istemci")));
    return true;
}

void SessionManager::stopLiveAudioBroadcast() {
    if (!m_liveAudioSource) return;

    m_liveAudioSource->stop();
    delete m_liveAudioSource;
    m_liveAudioSource = nullptr;
    m_liveIoDevice = nullptr;
    emit audioInputLevelChanged(0);

    okidia::AudioBroadcastCommand stopCmd;
    if (!m_liveAnnounceId.isEmpty()) {
        stopCmd.set_announce_id(m_liveAnnounceId.toStdString());
    }

    okidia::Envelope env;
    env.set_type(okidia::MessageType::AUDIO_BROADCAST_STOP);
    env.set_payload(stopCmd.SerializeAsString());

    if (m_liveBroadcastTargets.isEmpty()) {
        broadcastCommand(env);
    } else {
        for (const auto& targetId : m_liveBroadcastTargets) {
            sendCommand(targetId, env);
        }
    }
    m_liveBroadcastTargets.clear();
    m_liveAnnounceId.clear();

    if (m_activeAnnounceId.isEmpty()) {
        emit announceActiveStateChanged(false, QString());
    }

    Logger::info(QObject::tr("SessionManager: Canlı anons yayını durduruldu"));
}

// ---------------------------------------------------------------------------
// broadcastAudioMute / broadcastAudioUnmute
// ---------------------------------------------------------------------------
bool SessionManager::broadcastAudioMute() {
    return broadcastCommand(makeEnv(okidia::MessageType::AUDIO_MUTE));
}

bool SessionManager::broadcastAudioUnmute() {
    return broadcastCommand(makeEnv(okidia::MessageType::AUDIO_UNMUTE));
}
