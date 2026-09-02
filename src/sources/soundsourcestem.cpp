#include "sources/soundsourcestem.h"

extern "C" {

#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100) // FFmpeg 5.1
#include <libavutil/channel_layout.h>
#endif

} // extern "C"

#include <algorithm>
#include <memory>
#include <vector>

#include "sources/soundsourceffmpeg.h"
#include "track/steminfoimporter.h"
#include "util/assert.h"
#include "util/logger.h"
#include "util/sample.h"

#if !defined(VERBOSE_DEBUG_LOG)
#define VERBOSE_DEBUG_LOG false
#endif

namespace mixxx {

namespace {

// STEM constants
constexpr int kNumStreams = 5;
constexpr int kRequiredStreamCount = kNumStreams - 1; // Stem count doesn't include the main mix

// dj-station: раскладка .vdjstems. Пять дорожек, готового микса среди них нет —
// в отличие от файлов Native Instruments, где первая дорожка это премикс.
constexpr int kVdjStreamCount = 5;
constexpr int kVdjVocal = 0;
constexpr int kVdjHihat = 1;
constexpr int kVdjBass = 2;
constexpr int kVdjInstruments = 3;
constexpr int kVdjKick = 4;

const QString kVdjStemsExtension = QStringLiteral(".vdjstems");

const Logger kLogger("SoundSourceSTEM");

// Local RAII for AVFormatContext; SoundSourceFFmpeg's wrapper is private.
struct AVFormatContextDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx) {
            avformat_close_input(&ctx);
        }
    }
};
using AVFormatContextPtr =
        std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;

} // anonymous namespace

const QString SoundSourceProviderSTEM::kDisplayName = QStringLiteral("STEM with FFmpeg");

QStringList SoundSourceProviderSTEM::getSupportedFileTypes() const {
    return {"stem.mp4", "stem.m4a", "vdjstems"};
}

SoundSourceProviderPriority SoundSourceProviderSTEM::getPriorityHint(
        const QString& supportedFileType) const {
    Q_UNUSED(supportedFileType)
    return SoundSourceProviderPriority::Higher;
}

QString SoundSourceProviderSTEM::getVersionString() const {
    return QString::fromUtf8(av_version_info());
}

SoundSourceSTEM::SoundSourceSTEM(const QUrl& url)
        : SoundSource(url) {
}

SoundSourceSTEM::~SoundSourceSTEM() = default;

SoundSource::OpenResult SoundSourceSTEM::tryOpen(
        OpenMode /*mode*/,
        const OpenParams& params) {
    // Ensure that the source isn't yet opened
    VERIFY_OR_DEBUG_ASSERT(!m_requestedChannelCount.isValid()) {
        return OpenResult::Failed;
    }

    // dj-station: звук берём либо из самого файла, либо из стемов-спутника,
    // если они лежат рядом с треком. Метки при этом читаются из исходного
    // файла — его путь остаётся адресом источника.
    QString stemFilePath = getLocalFileName();
    const QString sidecar =
            StemInfoImporter::vdjStemsSidecarPath(stemFilePath);
    if (!sidecar.isEmpty()) {
        stemFilePath = sidecar;
    }
    const QUrl stemUrl = QUrl::fromLocalFile(stemFilePath);

    // Open input. RAII handles cleanup on every return path.
    AVFormatContextPtr pavInputFormatContextGuard(
            SoundSourceFFmpeg::openInputFile(stemFilePath));
    AVFormatContext* pavInputFormatContext = pavInputFormatContextGuard.get();
    if (pavInputFormatContext == nullptr) {
        kLogger.warning()
                << "Failed to open input file"
                << stemFilePath;
        return OpenResult::Failed;
    }
#if VERBOSE_DEBUG_LOG
    kLogger.debug()
            << "AVFormatContext"
            << "{ nb_streams" << pavInputFormatContext->nb_streams
            << "| start_time" << pavInputFormatContext->start_time
            << "| duration" << pavInputFormatContext->duration
            << "| bit_rate" << pavInputFormatContext->bit_rate
            << "| packet_size" << pavInputFormatContext->packet_size
            << "| audio_codec_id" << pavInputFormatContext->audio_codec_id
            << "| output_ts_offset" << pavInputFormatContext->output_ts_offset
            << '}';
#endif

    // Retrieve stream information
    const int avformat_find_stream_info_result =
            avformat_find_stream_info(pavInputFormatContext, nullptr);
    if (avformat_find_stream_info_result != 0) {
        DEBUG_ASSERT(avformat_find_stream_info_result < 0);
        kLogger.warning().noquote()
                << "avformat_find_stream_info() failed:"
                << SoundSourceFFmpeg::formatErrorString(avformat_find_stream_info_result);
        return OpenResult::Failed;
    }

    // dj-station: формат определяем по расширению — внутри это два разных
    // контейнера, и раскладка дорожек у них своя.
    const bool isVdjStems = stemFilePath.endsWith(
            kVdjStemsExtension, Qt::CaseInsensitive);

    uint selectedStemMask = params.stemMask();
    VERIFY_OR_DEBUG_ASSERT(selectedStemMask <= 2 << mixxx::kMaxSupportedStems) {
        kLogger.warning().noquote()
                << "Invalid selected stem mask" << selectedStemMask;
        return OpenResult::Failed;
    }
    OpenParams stemParam = params;
    stemParam.setChannelCount(mixxx::audio::ChannelCount::stereo());

    // Сначала просто собираем номера звуковых дорожек по порядку.
    std::vector<int> audioStreams;
    for (unsigned int streamIdx = 0; streamIdx < pavInputFormatContext->nb_streams; streamIdx++) {
        const AVCodecParameters* pCodecPar =
                pavInputFormatContext->streams[streamIdx]->codecpar;
        if (pCodecPar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100) // FFmpeg 5.1
        if (pCodecPar->ch_layout.nb_channels != mixxx::audio::ChannelCount::stereo()) {
#else
        if (pCodecPar->channels != mixxx::audio::ChannelCount::stereo()) {
#endif
            kLogger.warning().noquote()
                    << "stream at position" << streamIdx << "is not in stereo";
            return OpenResult::Failed;
        }
        audioStreams.push_back(static_cast<int>(streamIdx));
    }

    // План раскладки: на каждый стем — одна дорожка, либо две, если их нужно
    // сложить.
    std::vector<std::vector<int>> slotPlan;
    if (isVdjStems) {
        if (static_cast<int>(audioStreams.size()) != kVdjStreamCount) {
            kLogger.warning().noquote()
                    << "expected to find" << kVdjStreamCount
                    << "stem but found" << audioStreams.size();
            return OpenResult::Failed;
        }
        // VirtualDJ подписывает дорожки, поэтому ищем их по именам: порядок
        // внутри файла может измениться, а метки останутся.
        const auto findByTitle = [&](const char* title) -> int {
            for (int streamIdx : audioStreams) {
                const AVDictionaryEntry* pTitle = av_dict_get(
                        pavInputFormatContext->streams[streamIdx]->metadata,
                        "title",
                        nullptr,
                        0);
                if (pTitle && qstricmp(pTitle->value, title) == 0) {
                    return streamIdx;
                }
            }
            return -1;
        };

        int vocal = findByTitle("vocal");
        int hihat = findByTitle("hihat");
        int bass = findByTitle("bass");
        int instruments = findByTitle("instruments");
        int kick = findByTitle("kick");
        if (vocal < 0 || hihat < 0 || bass < 0 || instruments < 0 || kick < 0) {
            // Меток нет — берём обычный для VirtualDJ порядок дорожек.
            vocal = audioStreams[kVdjVocal];
            hihat = audioStreams[kVdjHihat];
            bass = audioStreams[kVdjBass];
            instruments = audioStreams[kVdjInstruments];
            kick = audioStreams[kVdjKick];
        }

        // Готового микса в .vdjstems нет — звучит сумма всех дорожек.
        // Бочку и хэты сводим в общие ударные: движок держит ровно четыре
        // стема, а порядок слотов должен совпадать со StemInfoImporter.
        slotPlan = {{hihat, kick}, {bass}, {instruments}, {vocal}};
    } else {
        if (static_cast<int>(audioStreams.size()) != kNumStreams) {
            kLogger.warning().noquote()
                    << "expected to find" << kRequiredStreamCount
                    << "stem but found"
                    << static_cast<int>(audioStreams.size()) - 1;
            return OpenResult::Failed;
        }
        // The first stream is the pre-mastered mix, which we NEVER LOAD, as we
        // do not have analyzer data for it.
        // This is because we only support one set of metadata for the whole
        // STEM file, where we determine the track parameters from an
        // on-the-fly mix of all 4 stems. Especially the replaygain differs
        // between the on-the-fly mix and the pre-mastered track, because we
        // do not apply DSP (limiter, equalizer, compressor) to the
        // on-the-fly mix. If this ever gets changed, we should set
        // `initSampleRateOnce` and `initBitrateOnce` to match the stem
        // sample rate and bit rate, such that
        // SoundSourceFFmpeg::resampleDecodedAVFrame will take care to
        // resample the main stream, in order to use the same time scale and
        // keep a working grid/cue definition
        for (int stemIdx = 0; stemIdx < kRequiredStreamCount; stemIdx++) {
            slotPlan.push_back({audioStreams[stemIdx + 1]});
        }
    }

    // Все дорожки должны быть в одном кодеке и на одной частоте, иначе сетка
    // битов и метки разъедутся между стемами.
    const AVStream* pFirstStem = nullptr;
    for (const auto& slot : slotPlan) {
        for (int streamIdx : slot) {
            const AVStream* pStream = pavInputFormatContext->streams[streamIdx];
            if (!pFirstStem) {
                pFirstStem = pStream;
                continue;
            }
            if (pStream->codecpar->codec_id != pFirstStem->codecpar->codec_id) {
                kLogger.warning().noquote()
                        << "Stem at position" << streamIdx << "is using a different codec";
                return OpenResult::Failed;
            }
            if (pStream->codecpar->sample_rate != pFirstStem->codecpar->sample_rate) {
                kLogger.warning().noquote()
                        << "Stem at position" << streamIdx << "is using a different sample rate";
                return OpenResult::Failed;
            }
        }
    }

    for (std::size_t slotIdx = 0; slotIdx < slotPlan.size(); slotIdx++) {
        if (selectedStemMask && !(selectedStemMask & 1u << slotIdx)) {
            continue;
        }

        auto pPrimary = std::make_unique<SoundSourceFFmpeg>(stemUrl,slotPlan[slotIdx][0]);
        if (pPrimary->open(OpenMode::Strict /*Unused*/, stemParam) != OpenResult::Succeeded) {
            return OpenResult::Failed;
        }
        m_pStereoStreams.emplace_back(std::move(pPrimary));

        // Вторая дорожка слота, если она есть, звучит вместе с первой.
        std::unique_ptr<SoundSourceFFmpeg> pAux;
        if (slotPlan[slotIdx].size() > 1) {
            pAux = std::make_unique<SoundSourceFFmpeg>(stemUrl,slotPlan[slotIdx][1]);
            if (pAux->open(OpenMode::Strict /*Unused*/, stemParam) != OpenResult::Succeeded) {
                return OpenResult::Failed;
            }
        }
        m_pAuxStreams.emplace_back(std::move(pAux));
    }

    VERIFY_OR_DEBUG_ASSERT(!m_pStereoStreams.empty()) {
        kLogger.warning().noquote()
                << "no stem track were selected";
        close();
        return OpenResult::Failed;
    }

    if (params.getSignalInfo().getChannelCount() ==
                    mixxx::audio::ChannelCount::stereo() ||
            selectedStemMask) {
        // Requesting a stereo stream (used for samplers and preview decks)
        m_requestedChannelCount = mixxx::audio::ChannelCount::stereo();
        initChannelCountOnce(mixxx::audio::ChannelCount::stereo());
    } else {
        // No special channel format request
        m_requestedChannelCount = mixxx::audio::ChannelCount::stem();
        initChannelCountOnce(
                static_cast<int>(mixxx::audio::ChannelCount::stereo() *
                        m_pStereoStreams.size()));
    }

    initSampleRateOnce(m_pStereoStreams.front()->getSignalInfo().getSampleRate());
    initBitrateOnce(m_pStereoStreams.front()->getBitrate());
    initFrameIndexRangeOnce(m_pStereoStreams.front()->frameIndexRange());

    return OpenResult::Succeeded;
}

void SoundSourceSTEM::close() {
    for (auto& stream : m_pStereoStreams) {
        stream->close();
    }
    for (auto& stream : m_pAuxStreams) {
        if (stream) {
            stream->close();
        }
    }
}

ReadableSampleFrames SoundSourceSTEM::readSampleFramesClamped(
        const WritableSampleFrames& globalSampleFrames) {
    VERIFY_OR_DEBUG_ASSERT(m_requestedChannelCount.isValid()) {
        return ReadableSampleFrames();
    }

    VERIFY_OR_DEBUG_ASSERT(globalSampleFrames.writableLength() %
                    m_requestedChannelCount ==
            0) {
        return ReadableSampleFrames();
    };

    SINT stemSampleLength = m_pStereoStreams.front()->getSignalInfo().frames2samples(
            globalSampleFrames.frameLength());

    // The same buffer is reused between requests tp prevent reallocation, but
    // it will be reallocated if a larger chunk is requested and will keep the
    // new maximum size
    if (stemSampleLength > m_buffer.size()) {
        m_buffer = SampleBuffer(stemSampleLength);
    }
    // Второй буфер нужен только там, где в стем сходятся две дорожки.
    const bool hasAuxStream = std::any_of(m_pAuxStreams.cbegin(),
            m_pAuxStreams.cend(),
            [](const auto& pStream) { return pStream != nullptr; });
    if (hasAuxStream && stemSampleLength > m_auxBuffer.size()) {
        m_auxBuffer = SampleBuffer(stemSampleLength);
    }

    ReadableSampleFrames read(globalSampleFrames.frameIndexRange(),
            SampleBuffer::ReadableSlice(
                    globalSampleFrames.writableData(),
                    globalSampleFrames.writableLength()));
    std::size_t stemCount = m_pStereoStreams.size();
    CSAMPLE* pBuffer = globalSampleFrames.writableData();

    if (m_requestedChannelCount == mixxx::audio::ChannelCount::stereo() && stemCount != 1) {
        SampleUtil::clear(pBuffer, globalSampleFrames.writableLength());
    } else {
        DEBUG_ASSERT(stemSampleLength * static_cast<SINT>(stemCount) ==
                globalSampleFrames.writableLength());
    }

    // Подмешать вторую дорожку слота в уже прочитанный кусок.
    const auto readAuxInto = [&](std::size_t slotIdx, CSAMPLE* pTarget) {
        if (slotIdx >= m_pAuxStreams.size() || !m_pAuxStreams[slotIdx]) {
            return;
        }
        WritableSampleFrames auxFrame = WritableSampleFrames(
                globalSampleFrames.frameIndexRange(),
                SampleBuffer::WritableSlice(
                        m_auxBuffer.data(),
                        stemSampleLength));
        m_pAuxStreams[slotIdx]->readSampleFrames(auxFrame);
        SampleUtil::add(pTarget, m_auxBuffer.data(), stemSampleLength);
    };

    if (stemCount == 1) {
        m_pStereoStreams[0]->readSampleFrames(globalSampleFrames);
        readAuxInto(0, globalSampleFrames.writableData());
        return read;
    }

    for (std::size_t streamIdx = 0; streamIdx < stemCount; streamIdx++) {
        WritableSampleFrames currentStemFrame = WritableSampleFrames(
                globalSampleFrames.frameIndexRange(),
                SampleBuffer::WritableSlice(
                        m_buffer.data(),
                        stemSampleLength));
        m_pStereoStreams[streamIdx]->readSampleFrames(currentStemFrame);
        readAuxInto(streamIdx, m_buffer.data());

        // Each m_pStereoStreams[streamIdx] provides a standard stereo signal (L/R).
        // in stem mode we need to transform the data to an interleaved layout:
        // 1L1R2L2R3L3R4L4R, 1L1R2L2R3L3R4L4R ...
        if (m_requestedChannelCount != mixxx::audio::ChannelCount::stereo()) {
            // Change the sample layout to interleave all channels together
            for (SINT i = 0; i < stemSampleLength / 2; i++) {
                pBuffer[2 * stemCount * i + 2 * streamIdx] = m_buffer[2 * i];
                pBuffer[2 * stemCount * i + 2 * streamIdx + 1] = m_buffer[2 * i + 1];
            }
        } else {
            // Change the sample layout to mix all channels together
            SampleUtil::add(pBuffer, m_buffer.data(), stemSampleLength);
        }
    }
    return read;
}

} // namespace mixxx
