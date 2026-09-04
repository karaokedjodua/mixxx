#pragma once

#include <QColor>

#include "skin/legacy/skincontext.h"
#include "util/class.h"
#include "waveform/renderers/waveformrendererabstract.h"

class WaveformRenderBeat : public WaveformRendererAbstract {
  public:
    explicit WaveformRenderBeat(WaveformWidgetRenderer* waveformWidgetRenderer);
    virtual ~WaveformRenderBeat();

    virtual void setup(const QDomNode& node, const SkinContext& context);
    virtual void draw(QPainter* painter, QPaintEvent* event);

  private:
    QColor m_beatColor;
    QColor m_barColor;
    QColor m_phraseColor;
    QVector<QLineF> m_beats;
    QVector<QLineF> m_bars;
    QVector<QLineF> m_phrases;
    QVector<QRectF> m_phraseMarks;

    DISALLOW_COPY_AND_ASSIGN(WaveformRenderBeat);
};
