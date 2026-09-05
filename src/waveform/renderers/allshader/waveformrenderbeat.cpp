#include "waveform/renderers/allshader/waveformrenderbeat.h"

#include <QDomNode>
#include <QVector4D>

#include "engine/engine.h"
#include "moc_waveformrenderbeat.cpp"
#include "rendergraph/geometry.h"
#include "rendergraph/material/rgbamaterial.h"
#include "rendergraph/vertexupdaters/rgbavertexupdater.h"
#include "skin/legacy/skincontext.h"
#include "track/track.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveform.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/wskincolor.h"

using namespace rendergraph;

namespace {
// Разметка волны как в VirtualDJ: бит - тонкая тусклая риска, такт (4 бита) -
// заметнее и с квадратиком, 16 и 32 бита - границы фраз, по которым и сводят.
//
// Почему уровни различаются ЯРКОСТЬЮ, а не только шириной: раньше вся сетка
// рисовалась одним цветом на 90 % непрозрачности, и полсотни белых линий
// забивали саму волну - смотреть было не на что. Теперь бит едва виден, а
// глаз цепляется за такты и фразы, то есть ровно за то, по чему выравнивают.
//
// Почему это по-прежнему один вызов отрисовки: RGBAMaterial берёт цвет из
// вершины, поэтому разные уровни живут в одной геометрии. Планшет слабый,
// второй проход по волне мы себе позволить не можем.
constexpr int kBarBeats = 4;
constexpr int kPhraseBeats16 = 16;
// синхронно с kPhraseBeats в remoteapihandler.cpp (сведение по фразе)
constexpr int kPhraseBeats32 = 32;

constexpr float kBeatWidth = 1.f;
constexpr float kBarWidth = 2.f;
constexpr float kPhraseWidth = 3.f;

// Доля от общей непрозрачности сетки ([Waveform] beatGridAlpha).
constexpr float kBeatAlpha = 0.28f;
constexpr float kBarAlpha = 0.60f;
constexpr float kPhraseAlpha = 1.0f;

constexpr float kBarMark = 5.f;        // сторона квадратика такта
constexpr float kPhraseMark16 = 9.f;   // сторона квадратика на 16 битах
constexpr float kPhraseMark32 = 13.f;  // сторона квадратика на фразе (32)

// Если соседние риски ближе этого, они сливаются в сплошную заливку: рисовать
// их бессмысленно и дорого. На мелком масштабе уровень просто выключается.
constexpr float kMinGapPx = 6.f;

inline int positiveMod(int v, int m) {
    const int r = v % m;
    return r < 0 ? r + m : r;
}
} // namespace

namespace allshader {

WaveformRenderBeat::WaveformRenderBeat(WaveformWidgetRenderer* waveformWidget,
        ::WaveformRendererAbstract::PositionSource type)
        : ::WaveformRendererAbstract(waveformWidget),
          m_isSlipRenderer(type == ::WaveformRendererAbstract::Slip) {
    initForRectangles<RGBAMaterial>(0);
    setUsePreprocess(true);
}

void WaveformRenderBeat::setup(const QDomNode& node, const SkinContext& skinContext) {
    m_color = QColor(skinContext.selectString(node, QStringLiteral("BeatColor")));
    m_color = WSkinColor::getCorrectColor(m_color).toRgb();
}

void WaveformRenderBeat::draw(QPainter* painter, QPaintEvent* event) {
    Q_UNUSED(painter);
    Q_UNUSED(event);
    DEBUG_ASSERT(false);
}

void WaveformRenderBeat::preprocess() {
    if (!preprocessInner()) {
        geometry().allocate(0);
        markDirtyGeometry();
    }
}

bool WaveformRenderBeat::preprocessInner() {
    const TrackPointer trackInfo = m_waveformRenderer->getTrackInfo();

    if (!trackInfo || (m_isSlipRenderer && !m_waveformRenderer->isSlipActive())) {
        return false;
    }

    const bool isStemTrack = trackInfo && trackInfo->hasStem() &&
            trackInfo->getWaveform() && trackInfo->getWaveform()->hasStem();
    const bool splitStemTracks = isStemTrack &&
            WaveformWidgetFactory::instance()->isStemSplitTracks();

    auto positionType = m_isSlipRenderer ? ::WaveformRendererAbstract::Slip
                                         : ::WaveformRendererAbstract::Play;

    mixxx::BeatsPointer trackBeats = trackInfo->getBeats();
    if (!trackBeats) {
        return false;
    }

    int alpha = m_waveformRenderer->getBeatGridAlpha();
    if (alpha == 0) {
        return false;
    }

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();

    const float gridAlpha = alpha / 100.0f;

    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0.0) {
        return false;
    }

    const double firstDisplayedPosition =
            m_waveformRenderer->getFirstDisplayedPosition(positionType);
    const double lastDisplayedPosition =
            m_waveformRenderer->getLastDisplayedPosition(positionType);

    const auto startPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            firstDisplayedPosition * trackSamples);
    const auto endPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            lastDisplayedPosition * trackSamples);

    if (!startPosition.isValid() || !endPosition.isValid()) {
        return false;
    }

    const float rendererBreadth = m_waveformRenderer->getBreadth();

    const int numVerticesPerLine = 6; // 2 triangles

    // Count the number of beats in the range to reserve space in the m_vertices vector.
    // Note that we could also use
    //   int numBearsInRange = trackBeats->numBeatsInRange(startPosition, endPosition);
    // for this, but there have been reports of that method failing with a DEBUG_ASSERT.
    int numBeatsInRange = 0;
    int numBarsInRange = 0;
    int numPhrasesInRange = 0;
    // Абсолютный индекс первого видимого бита - разностью итераторов от начала
    // трека. Такты и фразы отсчитываются от первого удара сетки, как в VDJ.
    const int firstBeatIndex = static_cast<int>(
            trackBeats->iteratorFrom(startPosition) -
            trackBeats->iteratorFrom(mixxx::audio::kStartFramePos));
    {
        auto it = trackBeats->iteratorFrom(startPosition);
        int beatIndex = firstBeatIndex;
        for (; it != trackBeats->cend() && *it <= endPosition; ++it, ++beatIndex) {
            if (positiveMod(beatIndex, kPhraseBeats16) == 0) {
                numPhrasesInRange++;
            } else if (positiveMod(beatIndex, kBarBeats) == 0) {
                numBarsInRange++;
            } else {
                numBeatsInRange++;
            }
        }
    }

    const int totalBeats = numBeatsInRange + numBarsInRange + numPhrasesInRange;
    if (totalBeats == 0) {
        return false;
    }

    // Сколько пикселей между соседними битами на этом масштабе. Отсюда решаем,
    // какие уровни вообще имеет смысл рисовать.
    const float rendererLength = static_cast<float>(m_waveformRenderer->getLength());
    const float beatGapPx = totalBeats > 1
            ? rendererLength / static_cast<float>(totalBeats - 1)
            : rendererLength;
    const bool drawBeats = beatGapPx >= kMinGapPx;
    const bool drawBars = beatGapPx * kBarBeats >= kMinGapPx;

    const int numBoxesPerBeat = (m_isSlipRenderer && splitStemTracks)
            ? mixxx::kMaxSupportedStems
            : 1;
    // Линии рисуются столбиком на каждую дорожку стема, квадратики - один раз.
    const int numLines = numPhrasesInRange +
            (drawBars ? numBarsInRange : 0) +
            (drawBeats ? numBeatsInRange : 0);
    const int numMarks = numPhrasesInRange + (drawBars ? numBarsInRange : 0);
    const int reserved = (numLines * numBoxesPerBeat + numMarks) * numVerticesPerLine;
    geometry().allocate(reserved);

    RGBAVertexUpdater vertexUpdater{
            geometry().vertexDataAs<Geometry::RGBAColoredPoint2D>()};

    const float r = static_cast<float>(m_color.redF());
    const float g = static_cast<float>(m_color.greenF());
    const float b = static_cast<float>(m_color.blueF());
    const QVector4D beatColor{r, g, b, gridAlpha * kBeatAlpha};
    const QVector4D barColor{r, g, b, gridAlpha * kBarAlpha};
    const QVector4D phraseColor{r, g, b, gridAlpha * kPhraseAlpha};

    const float boxBreadth = splitStemTracks
            ? rendererBreadth / static_cast<float>(mixxx::kMaxSupportedStems)
            : rendererBreadth;

    auto it = trackBeats->iteratorFrom(startPosition);
    int beatIndex = firstBeatIndex;
    for (; it != trackBeats->cend() && *it <= endPosition; ++it, ++beatIndex) {
        const bool isPhrase32 = positiveMod(beatIndex, kPhraseBeats32) == 0;
        const bool isPhrase16 = positiveMod(beatIndex, kPhraseBeats16) == 0;
        const bool isBar = !isPhrase16 && positiveMod(beatIndex, kBarBeats) == 0;

        if (isBar && !drawBars) {
            continue;
        }
        if (!isBar && !isPhrase16 && !drawBeats) {
            continue;
        }

        double beatPosition = it->toEngineSamplePos();
        double xBeatPoint =
                m_waveformRenderer->transformSamplePositionInRendererWorld(
                        beatPosition, positionType);

        xBeatPoint = qRound(xBeatPoint * devicePixelRatio) / devicePixelRatio;

        const float width = isPhrase16 ? kPhraseWidth : (isBar ? kBarWidth : kBeatWidth);
        const QVector4D& color = isPhrase16 ? phraseColor : (isBar ? barColor : beatColor);

        const float x1 = static_cast<float>(xBeatPoint);
        const float x2 = x1 + width;

        if (m_isSlipRenderer && splitStemTracks) {
            for (int stemIdx = 0; stemIdx < mixxx::kMaxSupportedStems; ++stemIdx) {
                const float posy1 = stemIdx * boxBreadth;
                const float posy2 = posy1 + boxBreadth / 2.f;
                vertexUpdater.addRectangle({x1, posy1}, {x2, posy2}, color);
            }
        } else {
            vertexUpdater.addRectangle({x1, 0.f},
                    {x2, m_isSlipRenderer ? rendererBreadth / 2 : rendererBreadth},
                    color);
        }

        // «Квадратик» у верхнего края волны: на такте маленький, на 16 битах
        // крупнее, на фразе (32 бита) - самый крупный. По ним и сводят.
        if (isBar || isPhrase16) {
            const float mark = isPhrase32 ? kPhraseMark32
                    : (isPhrase16 ? kPhraseMark16 : kBarMark);
            vertexUpdater.addRectangle(
                    {x1 - mark / 2.f, 0.f}, {x1 + mark / 2.f, mark}, color);
        }
    }
    markDirtyGeometry();

    DEBUG_ASSERT(reserved == vertexUpdater.index());

    markDirtyMaterial();

    return true;
}

} // namespace allshader
