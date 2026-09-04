#include "waveform/renderers/waveformrenderbeat.h"

#include <QPainter>

#include <algorithm>

#include "track/track.h"
#include "util/painterscope.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "widget/wskincolor.h"

class QPaintEvent;

namespace {
// Каждый 4-й бит — такт, 16-й и 32-й — фразовые границы («квадратики» VDJ).
constexpr int kBarBeats = 4;
constexpr int kPhraseBeats16 = 16;
constexpr int kPhraseBeats32 = 32;
// Размер «квадратика» в пикселях: начало 32-й фразы заметнее начала 16-й,
// как в VirtualDJ, — по нему на глаз ловится восьмёрка.
constexpr double kPhraseMark16 = 9.0;
constexpr double kPhraseMark32 = 13.0;

// Остаток всегда неотрицательный: сетка может начинаться до нуля трека,
// тогда beatIndex отрицательный и обычный % дал бы -3 вместо 13.
inline int positiveMod(int value, int modulus) {
    const int rest = value % modulus;
    return rest < 0 ? rest + modulus : rest;
}
} // namespace

WaveformRenderBeat::WaveformRenderBeat(WaveformWidgetRenderer* waveformWidgetRenderer)
        : WaveformRendererAbstract(waveformWidgetRenderer) {
    m_beats.resize(128);
    m_bars.resize(64);
    m_phrases.resize(16);
    m_phraseMarks.resize(16);
}

WaveformRenderBeat::~WaveformRenderBeat() {
}

void WaveformRenderBeat::setup(const QDomNode& node, const SkinContext& context) {
    m_beatColor = QColor(context.selectString(node, "BeatColor"));
    m_beatColor = WSkinColor::getCorrectColor(m_beatColor).toRgb();
    // BeatColor обычно белый с малой альфой (#40ffffff): RGB-производные
    // бесполезны, различаем уровни сетки альфой. Скин может переопределить
    // через BarColor / PhraseColor.
    const QString barColorStr = context.selectString(node, "BarColor");
    const QString phraseColorStr = context.selectString(node, "PhraseColor");
    m_barColor = barColorStr.isEmpty()
            ? QColor(m_beatColor.red(), m_beatColor.green(), m_beatColor.blue(),
                     qMin(255, m_beatColor.alpha() * 2 + 32))
            : WSkinColor::getCorrectColor(QColor(barColorStr)).toRgb();
    m_phraseColor = phraseColorStr.isEmpty()
            ? QColor(m_beatColor.red(), m_beatColor.green(), m_beatColor.blue(),
                     qMin(255, m_beatColor.alpha() * 4 + 64))
            : WSkinColor::getCorrectColor(QColor(phraseColorStr)).toRgb();
}

void WaveformRenderBeat::draw(QPainter* painter, QPaintEvent* /*event*/) {
    TrackPointer pTrackInfo = m_waveformRenderer->getTrackInfo();

    if (!pTrackInfo) {
        return;
    }

    mixxx::BeatsPointer trackBeats = pTrackInfo->getBeats();
    if (!trackBeats) {
        return;
    }

    int alpha = m_waveformRenderer->getBeatGridAlpha();
    if (alpha == 0) {
        return;
    }
#ifdef MIXXX_USE_QOPENGL
    // Using alpha transparency with drawLines causes a graphical issue when
    // drawing with QPainter on the QOpenGLWindow: instead of individual lines
    // a large rectangle encompassing all beatlines is drawn.
    m_beatColor.setAlphaF(1.f);
    m_barColor.setAlphaF(1.f);
    m_phraseColor.setAlphaF(1.f);
#else
    m_beatColor.setAlphaF(alpha/100.0);
    m_barColor.setAlphaF(alpha/100.0);
    m_phraseColor.setAlphaF(alpha/100.0);
#endif

    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0) {
        return;
    }

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();

    const double firstDisplayedPosition =
            m_waveformRenderer->getFirstDisplayedPosition();
    const double lastDisplayedPosition =
            m_waveformRenderer->getLastDisplayedPosition();

    const auto startPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            firstDisplayedPosition * trackSamples);
    const auto endPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            lastDisplayedPosition * trackSamples);
    auto it = trackBeats->iteratorFrom(startPosition);

    // if no beat do not waste time saving/restoring painter
    if (it == trackBeats->cend() || *it > endPosition) {
        return;
    }

    // Абсолютный индекс первого видимого бита — разностью итераторов от
    // начала трека (O(число маркеров), для обычной сетки — константа).
    // Фразы и такты считаются от первого удара сетки, как в VirtualDJ.
    int beatIndex = static_cast<int>(it - trackBeats->iteratorFrom(
                                                 mixxx::audio::kStartFramePos));

    PainterScope PainterScope(painter);

    painter->setRenderHint(QPainter::Antialiasing);

    const Qt::Orientation orientation = m_waveformRenderer->getOrientation();
    const float rendererWidth = m_waveformRenderer->getWidth();
    const float rendererHeight = m_waveformRenderer->getHeight();
    const double markScale = std::max(1.0, scaleFactor());

    int beatCount = 0;
    int barCount = 0;
    int phraseCount = 0;

    for (; it != trackBeats->cend() && *it <= endPosition; ++it, ++beatIndex) {
        double beatPosition = it->toEngineSamplePos();
        double xBeatPoint =
                m_waveformRenderer->transformSamplePositionInRendererWorld(beatPosition);

        xBeatPoint = qRound(xBeatPoint * devicePixelRatio) / devicePixelRatio;

        const int barMod = positiveMod(beatIndex, kBarBeats);
        const int phraseMod = positiveMod(beatIndex, kPhraseBeats16);

        if (phraseMod == 0) {
            if (phraseCount >= m_phrases.size()) {
                m_phrases.resize(m_phrases.size() * 2);
                m_phraseMarks.resize(m_phraseMarks.size() * 2);
            }
            const float phraseMarkSize = static_cast<float>(
                    (positiveMod(beatIndex, kPhraseBeats32) == 0 ? kPhraseMark32
                                                                 : kPhraseMark16) *
                    markScale);
            if (orientation == Qt::Horizontal) {
                m_phrases[phraseCount].setLine(xBeatPoint, 0.0f, xBeatPoint, rendererHeight);
                // «Квадратик» VDJ в верхнем краю волны.
                m_phraseMarks[phraseCount].setRect(
                        xBeatPoint - phraseMarkSize / 2.0f, 0.0f,
                        phraseMarkSize, phraseMarkSize);
            } else {
                m_phrases[phraseCount].setLine(0.0f, xBeatPoint, rendererWidth, xBeatPoint);
                m_phraseMarks[phraseCount].setRect(
                        0.0f, xBeatPoint - phraseMarkSize / 2.0f,
                        phraseMarkSize, phraseMarkSize);
            }
            phraseCount++;
        } else if (barMod == 0) {
            if (barCount >= m_bars.size()) {
                m_bars.resize(m_bars.size() * 2);
            }
            if (orientation == Qt::Horizontal) {
                m_bars[barCount++].setLine(xBeatPoint, 0.0f, xBeatPoint, rendererHeight);
            } else {
                m_bars[barCount++].setLine(0.0f, xBeatPoint, rendererWidth, xBeatPoint);
            }
        } else {
            if (beatCount >= m_beats.size()) {
                m_beats.resize(m_beats.size() * 2);
            }
            if (orientation == Qt::Horizontal) {
                m_beats[beatCount++].setLine(xBeatPoint, 0.0f, xBeatPoint, rendererHeight);
            } else {
                m_beats[beatCount++].setLine(0.0f, xBeatPoint, rendererWidth, xBeatPoint);
            }
        }
    }

    // Make sure to use constData to prevent detaches!
    if (beatCount > 0) {
        QPen beatPen(m_beatColor);
        beatPen.setWidthF(std::max(1.0, scaleFactor()));
        painter->setPen(beatPen);
        painter->drawLines(m_beats.constData(), beatCount);
    }
    if (barCount > 0) {
        QPen barPen(m_barColor);
        barPen.setWidthF(std::max(1.0, scaleFactor()) * 1.25f);
        painter->setPen(barPen);
        painter->drawLines(m_bars.constData(), barCount);
    }
    if (phraseCount > 0) {
        QPen phrasePen(m_phraseColor);
        phrasePen.setWidthF(std::max(1.0, scaleFactor()) * 1.5f);
        painter->setPen(phrasePen);
        painter->drawLines(m_phrases.constData(), phraseCount);
        painter->setPen(Qt::NoPen);
        painter->setBrush(m_phraseColor);
        painter->drawRects(m_phraseMarks.constData(), phraseCount);
    }
}
