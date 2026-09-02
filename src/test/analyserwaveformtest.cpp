#include <gtest/gtest.h>

#include <QDir>
#include <QtDebug>
#include <vector>

#include "analyzer/analyzertrack.h"
#include "analyzer/analyzerwaveform.h"
#include "library/dao/analysisdao.h"
#include "test/mixxxtest.h"
#include "track/track.h"
#include "waveform/renderers/waveformoverviewrenderer.h"

#include <QImage>
#include <QPainter>

namespace {

constexpr std::size_t kBigBufSize = 2 * 1920; // Matches the WaveformSummary
constexpr std::size_t kCanarySize = 1024 * 4;
constexpr float kMagicFloat = 1234.567890f;
constexpr float kCanaryFloat = 0.0f;
constexpr int kChannelCount = 2;
const QString kReferenceBuffersPath = QStringLiteral("reference_buffers/");

class AnalyzerWaveformTest : public MixxxTest {
  protected:
    AnalyzerWaveformTest()
            : m_aw(config(), QSqlDatabase()) {
    }

    void SetUp() override {
        m_pTrack = Track::newTemporary();
        m_pTrack->setAudioProperties(
                mixxx::audio::ChannelCount(kChannelCount),
                mixxx::audio::SampleRate(44100),
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromMillis(1000));

        // Memory layout for m_canaryBigBuf looks like
        //   [ canary | big buf | canary ]

        m_canaryBigBuf.resize(kBigBufSize + 2 * kCanarySize);
        for (std::size_t i = 0; i < kCanarySize; i++) {
            m_canaryBigBuf[i] = kCanaryFloat;
        }
        for (std::size_t i = kCanarySize; i < kCanarySize + kBigBufSize; i++) {
            m_canaryBigBuf[i] = kMagicFloat;
        }
        for (std::size_t i = kCanarySize + kBigBufSize; i < 2 * kCanarySize + kBigBufSize; i++) {
            m_canaryBigBuf[i] = kCanaryFloat;
        }
    }

    void assertWaveformReference(
            ConstWaveformPointer pWaveform,
            const QString& reference_title) {
        pWaveform->dump();

        QFile f(getTestDir().filePath(kReferenceBuffersPath + reference_title));
        bool pass = true;
        // If the file is not there, we will fail and write out the .actual
        // reference file.
        const QByteArray actual = pWaveform->toByteArray();

        ASSERT_TRUE(f.open(QFile::ReadOnly));
        const QByteArray reference = f.readAll();

        if (actual.size() == reference.size()) {
            for (int i = 0; i < actual.size(); ++i) {
                if (actual.at(i) != reference.at(i)) {
                    qDebug() << "#" << i << QString::number(actual[i], 16)
                             << QString::number(reference[i], 16);
                    pass = false;
                }
            }
        } else {
            qDebug() << "##" << actual.size() << reference.size();
            pass = false;
        }

        // Fail if either we didn't pass, or the comparison file was empty.
        if (!pass) {
            QString fname_actual = reference_title + ".actual";
            qWarning() << "Buffer does not match" << reference_title
                       << ", actual buffer written to "
                       << "reference_buffers/" + fname_actual;
            QFile actualFile(getTestDir().filePath(kReferenceBuffersPath + fname_actual));
            ASSERT_TRUE(actualFile.open(QFile::WriteOnly));
            actualFile.write(actual);
            actualFile.close();
            EXPECT_TRUE(false);
        }
        f.close();
    }

    void TearDown() override {
    }

  protected:
    AnalyzerWaveform m_aw;
    TrackPointer m_pTrack;
    std::vector<CSAMPLE> m_canaryBigBuf;
};

// Basic test to make sure we don't alter the input buffer and don't step out of bounds.
TEST_F(AnalyzerWaveformTest, canary) {
    m_aw.initialize(AnalyzerTrack(m_pTrack),
            m_pTrack->getSampleRate(),
            m_pTrack->getChannels(),
            kBigBufSize / kChannelCount);
    m_aw.processSamples(&m_canaryBigBuf[kCanarySize], kBigBufSize);
    m_aw.storeResults(m_pTrack);
    m_aw.cleanup();
    std::size_t i = 0;
    for (; i < kCanarySize; i++) {
        EXPECT_FLOAT_EQ(m_canaryBigBuf[i], kCanaryFloat);
    }
    for (; i < kCanarySize + kBigBufSize; i++) {
        EXPECT_FLOAT_EQ(m_canaryBigBuf[i], kMagicFloat);
    }
    for (; i < 2 * kCanarySize + kBigBufSize; i++) {
        EXPECT_FLOAT_EQ(m_canaryBigBuf[i], kCanaryFloat);
    }

    // Small reference, compare bitwise
    assertWaveformReference(m_pTrack->getWaveform(), "AnalyzerWaveformsTest");

    // The summary is always big, so we check only the metadata
    ConstWaveformPointer pWaveformSummary = m_pTrack->getWaveformSummary();
    ASSERT_NE(pWaveformSummary, nullptr);
    EXPECT_EQ(pWaveformSummary->getDataSize(), 3842);
    EXPECT_EQ(pWaveformSummary->getCompletion(), 3842);
    EXPECT_DOUBLE_EQ(pWaveformSummary->getAudioVisualRatio(), 1.0);
}

#ifdef __STEM__
// dj-station: сводная волна обязана хранить дорожки отдельно — на ней рисуется
// полоса обзора под декой. Раньше averageStore() их не записывала, полоса
// получала нули и оставалась пустой, хотя число дорожек было проставлено верно.
TEST(WaveformStrideStemTest, SummaryKeepsStemData) {
    constexpr int kStemCount = 4;
    WaveformStride stride(2.0, 4.0, kStemCount);

    // Один шаг разбора: у каждой дорожки свой уровень, по возрастанию.
    for (int channel = 0; channel < 2; channel++) {
        stride.m_overallData[channel] = 0.5f;
        for (int stemIdx = 0; stemIdx < kStemCount; stemIdx++) {
            stride.m_stemData[channel][stemIdx] = 0.1f * (stemIdx + 1);
        }
    }

    WaveformData detailed[2] = {};
    stride.store(detailed);
    // Подробная волна дорожки сохраняла и раньше.
    EXPECT_GT(detailed[0].stems[0], 0);

    WaveformData summary[2] = {};
    stride.averageStore(summary);
    for (int stemIdx = 0; stemIdx < kStemCount; stemIdx++) {
        EXPECT_GT(summary[0].stems[stemIdx], 0)
                << "дорожка " << stemIdx << " потерялась в сводной волне слева";
        EXPECT_GT(summary[1].stems[stemIdx], 0)
                << "дорожка " << stemIdx << " потерялась в сводной волне справа";
    }
    // Уровни должны сохранить заданный порядок, а не слиться в одно значение.
    EXPECT_LT(summary[0].stems[0], summary[0].stems[kStemCount - 1]);
}
#endif

// dj-station: сквозная проверка через НАСТОЯЩИЙ анализатор, а не модель шага.
// Подаём восемь каналов (четыре стема по стерео) с разной громкостью и смотрим,
// что сводная волна получила дорожки и что рисовальщик обзора по ним рисует,
// а убранная дорожка с картинки исчезает. Если это проходит, а на станции
// полоса не реагирует — причина не в данных и не в рисовании.
TEST_F(AnalyzerWaveformTest, SummaryFromRealAnalyzerCarriesStems) {
    constexpr int kStems = 4;
    constexpr int kChannels = kStems * 2;
    constexpr int kFrames = 4096;

    TrackPointer pTrack = Track::newTemporary();
    pTrack->setAudioProperties(
            mixxx::audio::ChannelCount(kChannels),
            mixxx::audio::SampleRate(44100),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromMillis(1000));

    // Стем s звучит с амплитудой 0.2*(s+1): порядок громкостей известен.
    std::vector<CSAMPLE> buf(static_cast<std::size_t>(kFrames) * kChannels);
    for (int f = 0; f < kFrames; f++) {
        const float sign = (f % 2 == 0) ? 1.f : -1.f;
        for (int st = 0; st < kStems; st++) {
            const float amp = 0.2f * (st + 1) * sign;
            buf[static_cast<std::size_t>(f) * kChannels + 2 * st] = amp;
            buf[static_cast<std::size_t>(f) * kChannels + 2 * st + 1] = amp;
        }
    }

    // Анализатор из фикстуры: у него есть настройки, без них он падает.
    ASSERT_TRUE(m_aw.initialize(AnalyzerTrack(pTrack),
            pTrack->getSampleRate(),
            pTrack->getChannels(),
            kFrames));
    m_aw.processSamples(buf.data(), static_cast<SINT>(buf.size()));
    m_aw.storeResults(pTrack);
    m_aw.cleanup();

    ConstWaveformPointer pSummary = pTrack->getWaveformSummary();
    ASSERT_NE(pSummary, nullptr);
    ASSERT_TRUE(pSummary->hasStem()) << "сводная волна не объявила дорожки";
    const int dataSize = pSummary->getDataSize();
    ASSERT_GT(dataSize, 4);
    ASSERT_EQ(pSummary->getCompletion(), dataSize);

    // Данные дорожек в сводной волне не нули и сохраняют порядок громкости.
    const WaveformData* pData = pSummary->data();
    int nonZero = 0;
    bool ordered = true;
    for (int i = 0; i < dataSize; i++) {
        if (pData[i].stems[0] > 0) {
            nonZero++;
            for (int st = 1; st < kStems; st++) {
                if (pData[i].stems[st] <= pData[i].stems[st - 1]) {
                    ordered = false;
                }
            }
        }
    }
    EXPECT_GT(nonZero, dataSize / 2) << "дорожки в сводной волне почти пусты";
    EXPECT_TRUE(ordered) << "порядок громкостей дорожек потерян";

    // Рисовальщик обзора: полная картина и картина без последней дорожки.
    QList<StemInfo> info;
    for (int st = 0; st < kStems; st++) {
        info.append(StemInfo(QStringLiteral("S%1").arg(st),
                QColor(50 + 50 * st, 100, 150)));
    }
    auto renderCount = [&](const QVector<float>& gains) {
        QImage img(dataSize / 2, 2 * 255, QImage::Format_ARGB32_Premultiplied);
        img.fill(QColor(0, 0, 0, 0).value());
        QPainter painter(&img);
        painter.translate(0.0, img.height() / 2.0);
        int start = 0;
        waveformOverviewRenderer::drawWaveformPartStem(
                &painter, pSummary, &start, dataSize, info, gains);
        painter.end();
        int painted = 0;
        for (int y = 0; y < img.height(); y++) {
            const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            for (int x = 0; x < img.width(); x++) {
                if (qAlpha(row[x]) > 0) {
                    painted++;
                }
            }
        }
        return painted;
    };
    const int full = renderCount({1.f, 1.f, 1.f, 1.f});
    const int noVocals = renderCount({1.f, 1.f, 1.f, 0.f});
    const int silent = renderCount({0.f, 0.f, 0.f, 0.f});
    EXPECT_GT(full, 0) << "обзор по стемам пуст";
    EXPECT_LT(noVocals, full) << "убранная дорожка не изменила картину";
    EXPECT_EQ(silent, 0) << "при нулевых громкостях что-то нарисовано";
}

} // namespace
