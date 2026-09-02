#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QTest>
#include <QtDebug>
#include <algorithm>
#include <cmath>
#include <memory>
#include <span>

#include "control/pollingcontrolproxy.h"
#include "mixxxtest.h"
#include "test/signalpathtest.h"

// dj-station: сценарии стемов на живом движке, но на устойчивых метриках.
// Золотые буферы гниют от смены версии FFmpeg или SoundTouch; RMS, пик и
// величина скачка между соседними отсчётами — нет. Меряем только выход
// мастера: у стем-групп своего буфера нет.
//
// Два урока этой обвязки, стоившие по нескольку сборок: (1) без
// EffectsManager::setup() стем-дека отдаёт тишину — базовая обвязка его не
// зовёт; (2) читатель декодирует в своём потоке, а тест дёргает process()
// без пауз — мерить можно только после того, как он доставил данные.

namespace {

const QString kVdjStemsFixture = QStringLiteral("stems/sin.vdjstems");
constexpr int kWarmupBuffers = 2;
constexpr int kMeasureBuffers = 6;

struct Metrics {
    double rms = 0.0;
    double peak = 0.0;
    double maxJump = 0.0;
    bool finite = true;
};

} // namespace

class StationScenarioTest : public BaseSignalPathTest {
  protected:
    static QString stemGroup(const QString& deckGroup, int stemNr) {
        return deckGroup.chopped(1) + QStringLiteral("_Stem") + QChar('0' + stemNr) + QChar(']');
    }

    void SetUp() override {
        BaseSignalPathTest::SetUp();
        // Базовая обвязка EffectsManager::setup() не зовёт. Без него цепочки
        // стемов остаются без движковой части, и стем-дека отдаёт тишину — на
        // этом же молча споткнулись бы золотые тесты, которые тут никогда не
        // гонялись. Порядок важен: после setup() addStem() сам дочитывает
        // цепочку дорожки и регистрирует её в движке.
        m_pEffectsManager->setup();
        for (int i = 1; i <= 4; i++) {
            ChannelHandleAndGroup handle =
                    m_pEngineMixer->registerChannelGroup(stemGroup(m_sGroup1, i));
            m_pChannel1->addStemHandle(handle);
            m_pEffectsManager->addStem(handle);
        }
        // MIXXX_SCENARIO_FIXTURE переключает образец: так один и тот же сценарий
        // гоняется и на стемах VirtualDJ, и на файле Native Instruments.
        m_fixture = qEnvironmentVariable("MIXXX_SCENARIO_FIXTURE");
        if (m_fixture.isEmpty()) {
            m_fixture = kVdjStemsFixture;
        }
        const QString location = QFileInfo(m_fixture).isAbsolute()
                ? m_fixture
                : getTestDir().filePath(m_fixture);
        TrackPointer pTrack(Track::newTemporary(location));
        loadTrackPumped(m_pMixerDeck1.get(), pTrack);

        m_pPlay = std::make_unique<PollingControlProxy>(m_sGroup1, QStringLiteral("play"));
        m_pStemCount = std::make_unique<PollingControlProxy>(m_sGroup1, QStringLiteral("stem_count"));
        for (int i = 1; i <= 4; i++) {
            m_volume[i - 1] = std::make_unique<PollingControlProxy>(
                    stemGroup(m_sGroup1, i), QStringLiteral("volume"));
            m_mute[i - 1] = std::make_unique<PollingControlProxy>(
                    stemGroup(m_sGroup1, i), QStringLiteral("mute"));
        }
        // Цепочки эффектов на стемах НЕ трогаем: гейн дорожки применяется
        // внутри их обработки, и выключенная цепочка глушит стем целиком.
    }

    // stem_count выставляется по сигналу из потока чтения — без прокрутки
    // цикла событий он не дойдёт до деки (см. stemcontrolobjecttest).
    void loadTrackPumped(Deck* pDeck, TrackPointer pTrack) {
        TrackPointer pLoaded;
        QMetaObject::Connection c = QObject::connect(pDeck,
                &BaseTrackPlayerImpl::newTrackLoaded,
                [&pLoaded](TrackPointer pNew) { pLoaded = pNew; }); // clazy:exclude=lambda-in-connect
        BaseSignalPathTest::loadTrack(pDeck, pTrack);
        for (int i = 0; i < 10000 && pLoaded != pTrack; ++i) {
            QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 1);
        }
        QObject::disconnect(c);
        ASSERT_EQ(pLoaded, pTrack) << "трек не загрузился";
    }

    // Крутит движок с короткими паузами, пока канал не отдаст ненулевой
    // отсчёт (или пока не кончится лимит). Возвращает число итераций.
    int waitForAudio(const QString& group, int maxIterations) {
        for (int it = 0; it < maxIterations; it++) {
            ProcessBuffer();
            const std::span<const CSAMPLE> ch = m_pEngineMixer->getChannelBuffer(group);
            for (const CSAMPLE v : ch) {
                if (std::fabs(v) > 1e-6f) {
                    return it;
                }
            }
            QTest::qSleep(3);
        }
        return -1;
    }

    Metrics measure(double fromSeconds = 1.0) {
        // Каждый замер — с одной и той же точки: образец короткий, а гейн с
        // рампой доезжает до значения за пару буферов. Не с самого нуля: у
        // AAC в Matroska первые кадры уходят на задержку декодера.
        const double sampleRate =
                PollingControlProxy(m_sGroup1, QStringLiteral("track_samplerate")).get();
        const double rate = sampleRate > 0 ? sampleRate : 44100.0;
        const double trackSeconds =
                PollingControlProxy(m_sGroup1, QStringLiteral("track_samples")).get() / (2.0 * rate);
        // Образцы короткие: точку замера берём не дальше трети трека, иначе
        // за время ожидания дека доедет до конца и остановится.
        const double startSec = std::min(fromSeconds, std::max(0.2, trackSeconds * 0.3));
        m_pChannel1->getEngineBuffer()->queueNewPlaypos(
                mixxx::audio::FramePos{startSec * rate}, EngineBuffer::SEEK_STANDARD);
        PollingControlProxy pos(m_sGroup1, QStringLiteral("playposition"));
        PollingControlProxy play(m_sGroup1, QStringLiteral("play"));
        // Читатель работает в своём потоке и декодирует пять потоков, а тест
        // дёргает process() без пауз: первые буферы после перемотки — тишина
        // «ещё не прочитано». Даём читателю подсказку одной прокруткой, ждём
        // не проигрывая, и лишь потом ищем первый ненулевой отсчёт.
        const double wasPlaying = play.get();
        play.set(0.0);
        ProcessBuffer();
        QTest::qSleep(250);
        play.set(wasPlaying);
        m_readerIterations = waitForAudio(m_sGroup1, 120);
        for (int i = 0; i < kWarmupBuffers; i++) {
            ProcessBuffer();
        }
        const double posBefore = pos.get();
        // playposition обновляется лишь 15 раз в секунду — сравниваем через
        // несколько буферов, иначе два соседних попадают в одно окно.
        for (int i = 0; i < 6; i++) {
            ProcessBuffer();
        }
        const double posAfter = pos.get();
        // qDebug в этом бинарнике не печатается — кладём показания в текст проверки.
        EXPECT_GT(posAfter, posBefore)
                << "дека не движется: play=" << play.get()
                << " playposition " << posBefore << " -> " << posAfter
                << " sampleRate=" << sampleRate
                << " rate_ratio=" << PollingControlProxy(m_sGroup1, QStringLiteral("rate_ratio")).get()
                << " rate=" << PollingControlProxy(m_sGroup1, QStringLiteral("rate")).get()
                << " sync=" << PollingControlProxy(m_sGroup1, QStringLiteral("sync_enabled")).get()
                << " track_samples=" << PollingControlProxy(m_sGroup1, QStringLiteral("track_samples")).get()
                << " passthrough=" << PollingControlProxy(m_sGroup1, QStringLiteral("passthrough")).get()
                << " fixture=" << m_fixture.toStdString();
        Metrics m;
        double sumSq = 0.0;
        SINT count = 0;
        for (int b = 0; b < kMeasureBuffers; b++) {
            ProcessBuffer();
            const std::span<const CSAMPLE> pOut = m_pEngineMixer->getMainBuffer();
            const SINT n = static_cast<SINT>(pOut.size());
            for (SINT i = 0; i < n; i++) {
                const CSAMPLE v = pOut[i];
                if (!std::isfinite(v)) {
                    m.finite = false;
                    continue;
                }
                sumSq += static_cast<double>(v) * v;
                count++;
                m.peak = std::max(m.peak, static_cast<double>(std::fabs(v)));
                // Соседние отсчёты одного канала стоят через один (стерео).
                if (i >= 2) {
                    m.maxJump = std::max(m.maxJump, static_cast<double>(std::fabs(v - pOut[i - 2])));
                }
            }
        }
        m.rms = count ? std::sqrt(sumSq / count) : 0.0;

        // Отдельно — что отдаёт сам канал деки до сведения в мастер: так
        // видно, теряется звук в деке или уже в микшере.
        double chSq = 0.0;
        SINT chN = 0;
        const std::span<const CSAMPLE> ch = m_pEngineMixer->getChannelBuffer(m_sGroup1);
        for (const CSAMPLE v : ch) {
            if (std::isfinite(v)) {
                chSq += static_cast<double>(v) * v;
                chN++;
            }
        }
        m_lastChannelRms = chN ? std::sqrt(chSq / chN) : -1.0;
        return m;
    }

    void setAll(double volume, double mute) {
        for (int i = 0; i < 4; i++) {
            m_volume[i]->set(volume);
            m_mute[i]->set(mute);
        }
    }

    QString m_fixture;
    double m_lastChannelRms = -1.0;
    int m_readerIterations = -1;
    std::unique_ptr<PollingControlProxy> m_pPlay;
    std::unique_ptr<PollingControlProxy> m_pStemCount;
    std::unique_ptr<PollingControlProxy> m_volume[4];
    std::unique_ptr<PollingControlProxy> m_mute[4];
};

TEST_F(StationScenarioTest, DeckSeesFourStems) {
    EXPECT_EQ(m_pStemCount->get(), 4.0);
}

// Проба-разделитель: preview-дека не основная и открывает стемовый файл как
// обычное стерео, минуя сведение дорожек. Если она звучит, а основная дека
// молчит — потеря в сведении стемов, а не в чтении файла.
TEST_F(StationScenarioTest, PreviewDeckPlaysStemFileAsPlainStereo) {
    const QString location = QFileInfo(m_fixture).isAbsolute()
            ? m_fixture
            : getTestDir().filePath(m_fixture);
    TrackPointer pTrack(Track::newTemporary(location));
    // PreviewDeck — не Deck, грузим напрямую и ждём чтение.
    m_pPreview1->slotLoadTrack(pTrack,
#ifdef __STEM__
            mixxx::StemChannelSelection(),
#endif
            false);
    ProcessBuffer();
    for (int i = 0; i < 15000; ++i) {
        if (m_pPreview1->getEngineDeck()->getEngineBuffer()->isTrackLoaded()) {
            break;
        }
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 1);
    }
    ASSERT_TRUE(m_pPreview1->getEngineDeck()->getEngineBuffer()->isTrackLoaded())
            << "preview-дека не загрузила трек";
    PollingControlProxy play(m_sPreviewGroup, QStringLiteral("play"));
    play.set(1.0);
    m_pPreview1->getEngineDeck()->getEngineBuffer()->queueNewPlaypos(
            mixxx::audio::FramePos{44100.0}, EngineBuffer::SEEK_STANDARD);
    const int iterations = waitForAudio(m_sPreviewGroup, 400);
    double sq = 0.0;
    SINT n = 0;
    for (int b = 0; b < 4; b++) {
        ProcessBuffer();
        const std::span<const CSAMPLE> ch = m_pEngineMixer->getChannelBuffer(m_sPreviewGroup);
        for (const CSAMPLE v : ch) {
            if (std::isfinite(v)) {
                sq += static_cast<double>(v) * v;
                n++;
            }
        }
    }
    const double rms = n ? std::sqrt(sq / n) : 0.0;
    EXPECT_GT(rms, 0.01) << "preview-дека молчит на стемовом файле: чтение файла в движке даёт тишину"
                         << " (итераций до звука=" << iterations << ")"
                         << " (каналов=" << static_cast<int>(m_pPreview1->getEngineDeck()->getEngineBuffer()->getChannelCount()) << ")";
}

TEST_F(StationScenarioTest, FullMixIsCleanAndAudible) {
    setAll(1.0, 0.0);
    m_pPlay->set(1.0);
    const Metrics m = measure();
    EXPECT_TRUE(m.finite) << "в выходе NaN или бесконечность";
    EXPECT_GT(m.rms, 0.01) << "полный микс не звучит (канал деки RMS="
                           << m_lastChannelRms << ", итераций до звука=" << m_readerIterations
                           << ", каналов в буфере деки="
                           << static_cast<int>(m_pChannel1->getEngineBuffer()->getChannelCount())
                           << ", основная дека=" << m_pChannel1->isPrimaryDeck() << ")";
    // Образец — четыре синуса на полной громкости; их сумма законно больше 1.
    EXPECT_LE(m.peak, 4.05) << "уровень выше суммы четырёх дорожек — лишнее усиление";
    // Настоящий щелчок — скачок больше всего размаха сигнала.
    EXPECT_LE(m.maxJump, 2.0 * m.peak + 1e-6) << "разрыв в сигнале (щелчок)";
}

TEST_F(StationScenarioTest, MuteAllSilencesUnmuteRestores) {
    setAll(1.0, 0.0);
    m_pPlay->set(1.0);
    const double full = measure().rms;
    ASSERT_GT(full, 0.01);

    setAll(1.0, 1.0);
    EXPECT_LT(measure().rms, 1e-3) << "заглушённые дорожки всё ещё звучат";

    setAll(1.0, 0.0);
    const double back = measure().rms;
    EXPECT_NEAR(back, full, full * 0.25) << "после снятия заглушения громкость не вернулась";
}

TEST_F(StationScenarioTest, SoloEachStemIsQuieterThanMixAndNotSilent) {
    m_pPlay->set(1.0);
    setAll(1.0, 0.0);
    const double full = measure().rms;
    ASSERT_GT(full, 0.01);
    for (int solo = 0; solo < 4; solo++) {
        for (int i = 0; i < 4; i++) {
            m_volume[i]->set(i == solo ? 1.0 : 0.0);
            m_mute[i]->set(0.0);
        }
        const Metrics m = measure();
        EXPECT_TRUE(m.finite);
        EXPECT_GT(m.rms, 0.002) << "соло дорожки " << solo + 1 << " беззвучно";
        EXPECT_LT(m.rms, full) << "соло дорожки " << solo + 1 << " не тише полного микса";
        EXPECT_LE(m.maxJump, 2.0 * m.peak + 1e-6) << "щелчок при соло дорожки " << solo + 1;
    }
}

TEST_F(StationScenarioTest, VolumeIsMonotonic) {
    m_pPlay->set(1.0);
    setAll(1.0, 0.0);
    const double v100 = measure().rms;
    setAll(0.5, 0.0);
    const double v50 = measure().rms;
    setAll(0.0, 0.0);
    const double v0 = measure().rms;
    EXPECT_GT(v100, v50);
    EXPECT_GT(v50, v0);
    EXPECT_LT(v0, 1e-3);
}
