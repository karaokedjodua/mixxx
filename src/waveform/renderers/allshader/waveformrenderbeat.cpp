#include "waveform/renderers/allshader/waveformrenderbeat.h"

#include <QDomNode>

#include "engine/engine.h"
#include "moc_waveformrenderbeat.cpp"
#include "rendergraph/geometry.h"
#include "rendergraph/material/unicolormaterial.h"
#include "rendergraph/vertexupdaters/vertexupdater.h"
#include "skin/legacy/skincontext.h"
#include "track/track.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveform.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/wskincolor.h"

using namespace rendergraph;

namespace {
// «Квадратики» как в VirtualDJ: на каждом такте (4 бита) маленький,
// на границе 16 бит — крупнее, на границе 32 бит (фраза) — самый крупный.
// Различаем уровни ТОЛЩИНОЙ линии и размером квадратика: один цвет,
// один material, один draw call — на слабом планшете это бесплатно.
constexpr int kBarBeats = 4;
constexpr int kPhraseBeats16 = 16;
constexpr int kPhraseBeats32 = 32;
constexpr float kBeatWidth = 1.f;
constexpr float kBarWidth = 2.f;
constexpr float kPhraseWidth = 4.f;
constexpr float kBarMark = 6.f;       // сторона квадратика такта
constexpr float kPhraseMark16 = 10.f; // сторона квадратика на 16 битах
constexpr float kPhraseMark32 = 16.f; // сторона квадратика на 32 битах

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
    initForRectangles<UniColorMaterial>(0);
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

    m_color.setAlphaF(alpha / 100.0f);

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
    int numMarksInRange = 0;
    {
        auto it = trackBeats->iteratorFrom(startPosition);
        // Абсолютный индекс первого видимого бита — разностью итераторов от
        // начала трека. Фразы и такты считаются от первого удара сетки.
        int beatIndex = static_cast<int>(it - trackBeats->iteratorFrom(
                                                     mixxx::audio::kStartFramePos));
        for (; it != trackBeats->cend() && *it <= endPosition; ++it, ++beatIndex) {
            numBeatsInRange++;
            if (positiveMod(beatIndex, kBarBeats) == 0) {
                numMarksInRange++;
            }
        }
    }

    const int numBoxesPerBeat = (m_isSlipRenderer && splitStemTracks)
            ? mixxx::kMaxSupportedStems
            : 1;
    // Каждому такту нужен ещё один прямоугольник под квадратик.
    const int reserved = (numBeatsInRange * numBoxesPerBeat + numMarksInRange) *
            numVerticesPerLine;
    geometry().allocate(reserved);

    VertexUpdater vertexUpdater{geometry().vertexDataAs<Geometry::Point2D>()};

    const float boxBreadth = splitStemTracks
            ? rendererBreadth / static_cast<float>(mixxx::kMaxSupportedStems)
            : rendererBreadth;

    auto it = trackBeats->iteratorFrom(startPosition);
    int beatIndex = static_cast<int>(it - trackBeats->iteratorFrom(
                                                 mixxx::audio::kStartFramePos));
    for (; it != trackBeats->cend() && *it <= endPosition; ++it, ++beatIndex) {
        double beatPosition = it->toEngineSamplePos();
        double xBeatPoint =
                m_waveformRenderer->transformSamplePositionInRendererWorld(
                        beatPosition, positionType);

        xBeatPoint = qRound(xBeatPoint * devicePixelRatio) / devicePixelRatio;

        const bool isPhrase32 = positiveMod(beatIndex, kPhraseBeats32) == 0;
        const bool isPhrase16 = positiveMod(beatIndex, kPhraseBeats16) == 0;
        const bool isBar = positiveMod(beatIndex, kBarBeats) == 0;
        const float width = isPhrase16 ? kPhraseWidth
                : isBar                  ? kBarWidth
                                         : kBeatWidth;

        const float x1 = static_cast<float>(xBeatPoint);
        const float x2 = x1 + width;

        if (m_isSlipRenderer && splitStemTracks) {
            for (int stemIdx = 0; stemIdx < mixxx::kMaxSupportedStems; ++stemIdx) {
                const float posy1 = stemIdx * boxBreadth;
                const float posy2 = posy1 + boxBreadth / 2.f;
                vertexUpdater.addRectangle({x1, posy1}, {x2, posy2});
            }
        } else {
            vertexUpdater.addRectangle({x1, 0.f},
                    {x2, m_isSlipRenderer ? rendererBreadth / 2 : rendererBreadth});
        }

        // «Квадратик» у верхнего края волны: на каждом такте маленький,
        // на границе 16 бит крупнее, на фразе (32 бита) — самый крупный.
        if (isBar) {
            const float mark = isPhrase32 ? kPhraseMark32
                    : isPhrase16          ? kPhraseMark16
                                          : kBarMark;
            vertexUpdater.addRectangle({x1 - mark / 2.f, 0.f}, {x1 + mark / 2.f, mark});
        }
    }
    markDirtyGeometry();

    DEBUG_ASSERT(reserved == vertexUpdater.index());

    material().setUniform(1, m_color);
    markDirtyMaterial();

    return true;
}

} // namespace allshader
