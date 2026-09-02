#include <gtest/gtest.h>

#include <QtDebug>

#include "sources/soundsourceffmpeg.h"
#include "sources/soundsourceproxy.cpp"
#include "test/mixxxtest.h"
#include "track/track.h"
#include "util/samplebuffer.h"

using namespace mixxx;

#define STEM_FILE QStringLiteral("stems/sin_%1.stem.mp4").arg(QString::fromStdString(GetParam()))

namespace {

const std::vector<std::string> supportedCodecs = {
#if !defined(Q_OS_WIN)
        "AAC_256kbps_VBR",
#endif
        "ALAC_24bit"};

const QList<QString> kStemFiles = {
        "01-drum.wav",
        "02-bass.wav",
        "03-melody.wav",
        "04-vocal.wav",
};

class StemFixture : public MixxxTest, public ::testing::WithParamInterface<std::string> {
  protected:
    void SetUp() override {
        ASSERT_TRUE(SoundSourceProxy::isFileTypeSupported("stem.mp4") ||
                SoundSourceProxy::registerProviders());
    }
};

TEST_P(StemFixture, FetchStemInfo) {
    TrackPointer pTrack(Track::newTemporary(getTestDir().filePath(STEM_FILE)));

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(mixxx::audio::ChannelCount(2));

    ASSERT_NE(SoundSourceProxy(pTrack).openAudioSource(config), nullptr);

    auto stemInfo = pTrack->getStemInfo();
    ASSERT_EQ(stemInfo.size(), 4);
    ASSERT_EQ(stemInfo.at(0), StemInfo("Drums", QColor(0xfd, 0x4a, 0x4a)));  // #fd4a4a
    ASSERT_EQ(stemInfo.at(1), StemInfo("Bass", QColor(0xff, 0xff, 0x00)));   // #ffff00
    ASSERT_EQ(stemInfo.at(2), StemInfo("Synths", QColor(0x00, 0xe8, 0xe8))); // #00e8e8
    ASSERT_EQ(stemInfo.at(3), StemInfo("Vox", QColor(0xad, 0x65, 0xff)));    // #ad65ff
}

TEST_P(StemFixture, FetchStemEmptyInfo) {
    TrackPointer pTrack(Track::newTemporary(
            getTestDir().filePath("stems/test_missing_stem_details.stem.mp4")));

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(mixxx::audio::ChannelCount(2));

    ASSERT_NE(SoundSourceProxy(pTrack).openAudioSource(config), nullptr);

    auto stemInfo = pTrack->getStemInfo();
    ASSERT_EQ(stemInfo.size(), 4);
    ASSERT_EQ(stemInfo.at(0), StemInfo("Stem #1", QColor(0x00, 0x9E, 0x73)));
    ASSERT_EQ(stemInfo.at(1), StemInfo("Stem #2", QColor(0xD5, 0x5E, 0x00)));
    ASSERT_EQ(stemInfo.at(2), StemInfo("Stem #3", QColor(0xCC, 0x79, 0xA7)));
    ASSERT_EQ(stemInfo.at(3), StemInfo("Stem #4", QColor(0x56, 0xB4, 0xE9)));
}

TEST_P(StemFixture, ReadMainMix) {
    SoundSourceFFmpeg sourceMainMix(
            QUrl::fromLocalFile(getTestDir().filePath("stems/mainmix.wav")));
    SoundSourceSTEM sourceStem(QUrl::fromLocalFile(getTestDir().filePath(STEM_FILE)));

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(mixxx::audio::ChannelCount(2));

    ASSERT_EQ(sourceMainMix.open(AudioSource::OpenMode::Strict, config),
            AudioSource::OpenResult::Succeeded);
    ASSERT_EQ(sourceStem.open(AudioSource::OpenMode::Strict, config),
            AudioSource::OpenResult::Succeeded);

    ASSERT_EQ(sourceMainMix.getSignalInfo(), sourceStem.getSignalInfo());

    SampleBuffer buffer1(1024), buffer2(1024);
    ASSERT_EQ(sourceMainMix.readSampleFrames(WritableSampleFrames(
                                                     IndexRange::between(
                                                             0,
                                                             512),
                                                     SampleBuffer::WritableSlice(
                                                             buffer1.data(),
                                                             buffer1.size())))
                      .readableLength(),
            buffer1.size());
    ASSERT_EQ(sourceStem.readSampleFrames(WritableSampleFrames(
                                                  IndexRange::between(
                                                          0,
                                                          512),
                                                  SampleBuffer::WritableSlice(
                                                          buffer2.data(),
                                                          buffer2.size())))
                      .readableLength(),
            buffer2.size());
    EXPECT_TRUE(0 == std::memcmp(buffer1.data(), buffer1.data(), sizeof(buffer1)));
}

TEST_P(StemFixture, ReadEachStem) {
    int stemIdx = 0;
    for (auto& stem : kStemFiles) {
        SoundSourceFFmpeg sourceStandaloneStem(
                QUrl::fromLocalFile(getTestDir().filePath("stems/" + stem)));
        SoundSourceFFmpeg sourceStem(
                QUrl::fromLocalFile(getTestDir().filePath(STEM_FILE)), stemIdx++);

        mixxx::AudioSource::OpenParams config;
        config.setChannelCount(mixxx::audio::ChannelCount(2));

        ASSERT_EQ(sourceStandaloneStem.open(AudioSource::OpenMode::Strict, config),
                AudioSource::OpenResult::Succeeded);
        ASSERT_EQ(sourceStem.open(AudioSource::OpenMode::Strict, config),
                AudioSource::OpenResult::Succeeded);

        ASSERT_EQ(sourceStandaloneStem.getSignalInfo(), sourceStem.getSignalInfo());

        SampleBuffer buffer1(1024), buffer2(1024);
        ASSERT_EQ(sourceStandaloneStem.readSampleFrames(WritableSampleFrames(
                                                                IndexRange::between(
                                                                        0,
                                                                        512),
                                                                SampleBuffer::WritableSlice(
                                                                        buffer1.data(),
                                                                        buffer1.size())))
                          .readableLength(),
                buffer1.size());
        ASSERT_EQ(sourceStem.readSampleFrames(WritableSampleFrames(
                                                      IndexRange::between(
                                                              0,
                                                              512),
                                                      SampleBuffer::WritableSlice(
                                                              buffer2.data(),
                                                              buffer2.size())))
                          .readableLength(),
                buffer2.size());
        EXPECT_TRUE(0 == std::memcmp(buffer1.data(), buffer1.data(), sizeof(buffer1)));
    }
}

TEST_P(StemFixture, OpenStem) {
    SoundSourceSTEM sourceStem(QUrl::fromLocalFile(getTestDir().filePath(STEM_FILE)));

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(mixxx::audio::ChannelCount(8));
    ASSERT_EQ(sourceStem.open(AudioSource::OpenMode::Strict, config),
            AudioSource::OpenResult::Succeeded);

    ASSERT_EQ(mixxx::audio::SignalInfo(mixxx::audio::ChannelCount::stem(),
                      mixxx::audio::SampleRate(44100)),
            sourceStem.getSignalInfo());
}

INSTANTIATE_TEST_SUITE_P(
        StemTest,
        StemFixture,
        ::testing::ValuesIn(supportedCodecs),
        [](const testing::TestParamInfo<StemFixture::ParamType>& info) {
            return info.param;
        });

// dj-station: чтение стемов VirtualDJ. Путь к файлу берём из переменной
// окружения MIXXX_VDJSTEMS_TEST_FILE — держать в репозитории десятки мегабайт
// ради одного теста незачем, а свободно распространяемого образца у нас нет.
class VdjStemsFixture : public MixxxTest {
  protected:
    void SetUp() override {
        ASSERT_TRUE(SoundSourceProxy::isFileTypeSupported("vdjstems") ||
                SoundSourceProxy::registerProviders());
        m_filePath = qEnvironmentVariable("MIXXX_VDJSTEMS_TEST_FILE");
    }

    bool skipWithoutSample() {
        if (m_filePath.isEmpty()) {
            return true;
        }
        return false;
    }

    QString m_filePath;
};

TEST_F(VdjStemsFixture, FileTypeIsSupported) {
    EXPECT_TRUE(SoundSourceProxy::isFileTypeSupported("vdjstems"));
}

// Стемы рядом с треком: в фонотеке остаётся одна запись — сам трек, со своими
// метками, — но играют её стемы. Так раскладывает файлы VirtualDJ.
TEST_F(VdjStemsFixture, SidecarTrackPlaysStemsKeepsTags) {
    const QString trackPath =
            qEnvironmentVariable("MIXXX_VDJSTEMS_SIDECAR_TRACK");
    if (trackPath.isEmpty()) {
        GTEST_SKIP() << "MIXXX_VDJSTEMS_SIDECAR_TRACK не задан";
    }
    ASSERT_FALSE(mixxx::StemInfoImporter::vdjStemsSidecarPath(trackPath).isEmpty())
            << "рядом с треком нет файла стемов";

    TrackPointer pTrack(Track::newTemporary(trackPath));

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(mixxx::audio::ChannelCount::stem());
    const auto pAudioSource = SoundSourceProxy(pTrack).openAudioSource(config);
    ASSERT_NE(pAudioSource, nullptr);

    // Звук пришёл из стемов...
    EXPECT_EQ(pAudioSource->getSignalInfo().getChannelCount(),
            mixxx::audio::ChannelCount::stem());
    EXPECT_EQ(pTrack->getStemInfo().size(), 4);

    // ...а запись в фонотеке осталась самим треком: путь не подменён, и тип
    // файла прежний, то есть метки по-прежнему читаются из него, а не из
    // Matroska, где их нет вовсе.
    EXPECT_EQ(QFileInfo(pTrack->getLocation()).absoluteFilePath(),
            QFileInfo(trackPath).absoluteFilePath());
    EXPECT_EQ(mixxx::SoundSource::getTypeFromFile(QFileInfo(trackPath)),
            QStringLiteral("mp3"));

    // Длительность взята у стемов, а не у короткого тестового mp3 —
    // значит играет действительно файл-спутник.
    SoundSourceSTEM stemsOnly(
            QUrl::fromLocalFile(
                    mixxx::StemInfoImporter::vdjStemsSidecarPath(trackPath)));
    mixxx::AudioSource::OpenParams stemsConfig;
    stemsConfig.setChannelCount(mixxx::audio::ChannelCount::stem());
    ASSERT_EQ(stemsOnly.open(AudioSource::OpenMode::Strict, stemsConfig),
            AudioSource::OpenResult::Succeeded);
    EXPECT_EQ(pAudioSource->frameIndexRange(), stemsOnly.frameIndexRange());
}

// Спутник не должен появляться в фонотеке вторым треком, но отдельно лежащий
// файл стемов — вполне себе трек.
TEST_F(VdjStemsFixture, SidecarIsNotALibraryTrackButStandaloneIs) {
    const QString trackPath =
            qEnvironmentVariable("MIXXX_VDJSTEMS_SIDECAR_TRACK");
    if (trackPath.isEmpty() || skipWithoutSample()) {
        GTEST_SKIP() << "не заданы MIXXX_VDJSTEMS_SIDECAR_TRACK и MIXXX_VDJSTEMS_TEST_FILE";
    }
    const QString sidecar =
            mixxx::StemInfoImporter::vdjStemsSidecarPath(trackPath);
    ASSERT_FALSE(sidecar.isEmpty());

    EXPECT_FALSE(SoundSourceProxy::isFileSupported(mixxx::FileInfo(sidecar)));
    EXPECT_TRUE(SoundSourceProxy::isFileSupported(mixxx::FileInfo(trackPath)));
    EXPECT_TRUE(SoundSourceProxy::isFileSupported(mixxx::FileInfo(m_filePath)));
}

TEST_F(VdjStemsFixture, ImporterRecognisesFile) {
    if (skipWithoutSample()) {
        GTEST_SKIP() << "MIXXX_VDJSTEMS_TEST_FILE не задан";
    }
    EXPECT_TRUE(mixxx::StemInfoImporter::maybeStemFile(m_filePath));
    EXPECT_EQ(mixxx::StemInfoImporter::importStemInfos(m_filePath).size(), 4);

    TrackPointer pTrack(Track::newTemporary(m_filePath));
    EXPECT_TRUE(mixxx::StemInfoImporter::maybeStemFile(pTrack->getLocation()))
            << "путь трека: " << pTrack->getLocation().toStdString();
}

TEST_F(VdjStemsFixture, StemInfoIsSynthesised) {
    if (skipWithoutSample()) {
        GTEST_SKIP() << "MIXXX_VDJSTEMS_TEST_FILE не задан";
    }
    TrackPointer pTrack(Track::newTemporary(m_filePath));

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(mixxx::audio::ChannelCount(2));
    ASSERT_NE(SoundSourceProxy(pTrack).openAudioSource(config), nullptr);

    const auto stemInfo = pTrack->getStemInfo();
    ASSERT_EQ(stemInfo.size(), 4);
    EXPECT_EQ(stemInfo.at(0).getLabel(), QStringLiteral("Drums"));
    EXPECT_EQ(stemInfo.at(1).getLabel(), QStringLiteral("Bass"));
    EXPECT_EQ(stemInfo.at(2).getLabel(), QStringLiteral("Other"));
    EXPECT_EQ(stemInfo.at(3).getLabel(), QStringLiteral("Vocals"));
}

TEST_F(VdjStemsFixture, OpensAsFourStems) {
    if (skipWithoutSample()) {
        GTEST_SKIP() << "MIXXX_VDJSTEMS_TEST_FILE не задан";
    }
    SoundSourceSTEM sourceStem(QUrl::fromLocalFile(m_filePath));

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(mixxx::audio::ChannelCount(8));
    ASSERT_EQ(sourceStem.open(AudioSource::OpenMode::Strict, config),
            AudioSource::OpenResult::Succeeded);

    EXPECT_EQ(mixxx::audio::SignalInfo(mixxx::audio::ChannelCount::stem(),
                      mixxx::audio::SampleRate(44100)),
            sourceStem.getSignalInfo());
}

// Каждый из четырёх стемов должен звучать и отличаться от остальных. Если бы
// сложение дорожек ломалось, ударные оказались бы тишиной или копией соседа.
TEST_F(VdjStemsFixture, EveryStemDecodesDistinctAudio) {
    if (skipWithoutSample()) {
        GTEST_SKIP() << "MIXXX_VDJSTEMS_TEST_FILE не задан";
    }
    SoundSourceSTEM sourceStem(QUrl::fromLocalFile(m_filePath));

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(mixxx::audio::ChannelCount(8));
    ASSERT_EQ(sourceStem.open(AudioSource::OpenMode::Strict, config),
            AudioSource::OpenResult::Succeeded);

    constexpr SINT kStemSlots = 4;
    constexpr SINT kChannels = 8;
    // Читаем из середины трека: начало часто оказывается тишиной.
    const SINT startFrame = sourceStem.frameIndexRange().length() / 2;
    const SINT frameCount = 100000;
    ASSERT_GT(sourceStem.frameIndexRange().length(), startFrame + frameCount);

    SampleBuffer buffer(frameCount * kChannels);
    const auto readFrames = sourceStem.readSampleFrames(
            WritableSampleFrames(
                    mixxx::IndexRange::forward(startFrame, frameCount),
                    SampleBuffer::WritableSlice(buffer.data(), buffer.size())));
    ASSERT_EQ(readFrames.frameLength(), frameCount);

    double peak[kStemSlots] = {0.0, 0.0, 0.0, 0.0};
    for (SINT frame = 0; frame < frameCount; frame++) {
        for (SINT slot = 0; slot < kStemSlots; slot++) {
            const CSAMPLE sample = buffer[frame * kChannels + 2 * slot];
            peak[slot] = qMax(peak[slot], static_cast<double>(qAbs(sample)));
        }
    }

    for (SINT slot = 0; slot < kStemSlots; slot++) {
        EXPECT_GT(peak[slot], 0.0001) << "стем " << slot << " оказался тишиной";
    }

    for (SINT slot = 1; slot < kStemSlots; slot++) {
        bool identical = true;
        for (SINT frame = 0; frame < frameCount && identical; frame++) {
            if (buffer[frame * kChannels] != buffer[frame * kChannels + 2 * slot]) {
                identical = false;
            }
        }
        EXPECT_FALSE(identical) << "стем " << slot << " совпал с ударными";
    }
}

} // namespace
