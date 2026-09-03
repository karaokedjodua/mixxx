#include "waveformoverviewrenderer.h"

#include <QLine>
#include <QPainter>
#include <QVector>

#include <algorithm>

#include "engine/engine.h"
#include "util/colorcomponents.h"
#include "util/math.h"
#include "util/timer.h"
#include "waveform/renderers/waveformsignalcolors.h"

namespace waveformOverviewRenderer {

QImage render(ConstWaveformPointer pWaveform,
        mixxx::OverviewType type,
        const WaveformSignalColors& signalColors,
        bool mono,
        QSize targetSize) {
    const int dataSize = pWaveform->getDataSize();
    if (dataSize <= 0) {
        return QImage();
    }

    QImage image(dataSize / 2, 2 * 255, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(0, 0, 0, 0).value());

    QPainter painter(&image);
    painter.translate(0.0, static_cast<double>(image.height()) / 2.0);

    if (type == mixxx::OverviewType::HSV) {
        drawWaveformPartHSV(&painter,
                pWaveform,
                nullptr,
                dataSize,
                signalColors,
                mono);
    } else if (type == mixxx::OverviewType::Filtered) {
        drawWaveformPartLMH(&painter,
                pWaveform,
                nullptr,
                dataSize,
                signalColors,
                mono);
    } else {
        drawWaveformPartRGB(&painter,
                pWaveform,
                nullptr,
                dataSize,
                signalColors,
                mono);
    }

    // Evaluate waveform ratio peak
    float peak = 1;
    for (int i = 0; i < dataSize; i += 2) {
        peak = math_max3(
                peak,
                static_cast<float>(pWaveform->getAll(i)),
                static_cast<float>(pWaveform->getAll(i + 1)));
    }
    // Normalize
    float diffGain = 0;
    if (peak > 1) {
        diffGain = 255 - peak - 1;
    }

    const int topLeft = static_cast<int>(mono ? diffGain * 2 : diffGain);
    const QRect sourceRect(0,
            topLeft,
            image.width(),
            image.height() -
                    2 * static_cast<int>(diffGain));
    QImage croppedImage = image.copy(sourceRect);
    // Copy image, otherwise QPainter crashes when we alter it.
    // Нормализация — это растяжение обрезанной картинки обратно. Если размер
    // назначения известен (ячейка списка треков — около сотни пикселей),
    // растягивать до 1921x510, чтобы следом уменьшить, чистая потеря: делаем
    // одно уменьшение сразу в нужный размер.
    const QSize outSize = targetSize.isEmpty() ? image.size() : targetSize;
    QImage normImage = croppedImage.scaled(outSize,
            Qt::IgnoreAspectRatio,
            Qt::SmoothTransformation);

    return normImage;
}

void drawWaveformPartRGB(
        QPainter* pPainter,
        ConstWaveformPointer pWaveform,
        int* start,
        int end,
        const WaveformSignalColors& signalColors,
        bool mono) {
    ScopedTimer t(QStringLiteral("waveformOverviewRenderer::drawNextPixmapPartRGB"));
    int startVal = 0;
    if (start) {
        startVal = *start;
    }

    const QColor lowColor = signalColors.getRgbLowColor();
    const QColor midColor = signalColors.getRgbMidColor();
    const QColor highColor = signalColors.getRgbHighColor();
    QColor color;

    float lowColor_r = 0, lowColor_g = 0, lowColor_b = 0,
          midColor_r = 0, midColor_g = 0, midColor_b = 0,
          highColor_r = 0, highColor_g = 0, highColor_b = 0,
          all = 0, low = 0, mid = 0, high = 0,
          red = 0, green = 0, blue = 0, max = 0;

    getRgbF(lowColor, &lowColor_r, &lowColor_g, &lowColor_b);
    getRgbF(midColor, &midColor_r, &midColor_g, &midColor_b);
    getRgbF(highColor, &highColor_r, &highColor_g, &highColor_b);

    if (mono) {
        // Mono means we're going to paint from bottom to top with l+r.
        const qreal dy = pPainter->deviceTransform().dy();
        pPainter->resetTransform();
        // shift y0 to bottom
        pPainter->translate(0, 2 * dy);
        // flip y-axis
        pPainter->scale(1, -1);
        for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
            // Left
            all = pWaveform->getAll(i) + pWaveform->getAll(i + 1);
            low = pWaveform->getLow(i) + pWaveform->getLow(i + 1);
            mid = pWaveform->getMid(i) + pWaveform->getMid(i + 1);
            high = pWaveform->getHigh(i) + pWaveform->getHigh(i + 1);

            red = low * lowColor_r + mid * midColor_r + high * highColor_r;
            green = low * lowColor_g + mid * midColor_g + high * highColor_g;
            blue = low * lowColor_b + mid * midColor_b + high * highColor_b;
            // Normalize
            max = math_max3(red, green, blue);
            // Draw
            if (max > 0.0) {
                color.setRgbF(static_cast<float>(low / max),
                        static_cast<float>(mid / max),
                        static_cast<float>(high / max));
                pPainter->setPen(color);
                pPainter->drawLine(x, static_cast<int>(all), x, 0);
            }
        }
    } else { // stereo
        for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
            // Left
            all = pWaveform->getAll(i);
            low = pWaveform->getLow(i);
            mid = pWaveform->getMid(i);
            high = pWaveform->getHigh(i);

            red = low * lowColor_r + mid * midColor_r + high * highColor_r;
            green = low * lowColor_g + mid * midColor_g + high * highColor_g;
            blue = low * lowColor_b + mid * midColor_b + high * highColor_b;
            // Normalize
            max = math_max3(red, green, blue);
            // Draw
            if (max > 0.0) {
                color.setRgbF(static_cast<float>(low / max),
                        static_cast<float>(mid / max),
                        static_cast<float>(high / max));
                pPainter->setPen(color);
                pPainter->drawLine(x, static_cast<int>(-all), x, 0);
            }

            // Right
            all = pWaveform->getAll(i + 1);
            low = pWaveform->getLow(i + 1);
            mid = pWaveform->getMid(i + 1);
            high = pWaveform->getHigh(i + 1);

            red = low * lowColor_r + mid * midColor_r + high * highColor_r;
            green = low * lowColor_g + mid * midColor_g + high * highColor_g;
            blue = low * lowColor_b + mid * midColor_b + high * highColor_b;

            max = math_max3(red, green, blue);

            if (max > 0.0) {
                color.setRgbF(static_cast<float>(low / max),
                        static_cast<float>(mid / max),
                        static_cast<float>(high / max));
                pPainter->setPen(color);
                pPainter->drawLine(x, 0, x, static_cast<int>(all));
            }
        }
    }

    if (start) {
        *start = end;
    }
}

void drawWaveformPartLMH(
        QPainter* pPainter,
        ConstWaveformPointer pWaveform,
        int* start,
        int end,
        const WaveformSignalColors& signalColors,
        bool mono) {
    ScopedTimer t(QStringLiteral("waveformOverviewRenderer::drawNextPixmapPartLMH"));
    const QColor lowColor = signalColors.getLowColor();
    const QColor midColor = signalColors.getMidColor();
    const QColor highColor = signalColors.getHighColor();
    int startVal = 0;
    if (start) {
        startVal = *start;
    }

    if (mono) {
        // Mono means we're going to paint from bottom to top with l+r.
        const qreal dy = pPainter->deviceTransform().dy();
        pPainter->resetTransform();
        // shift y0 to bottom
        pPainter->translate(0, 2 * dy);
        // flip y-axis
        pPainter->scale(1, -1);

        for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
            x = i / 2;
            pPainter->setPen(lowColor);
            pPainter->drawLine(QPoint(x, 0),
                    QPoint(x, pWaveform->getLow(i) + pWaveform->getLow(i + 1)));

            pPainter->setPen(midColor);
            pPainter->drawLine(QPoint(x, 0),
                    QPoint(x, pWaveform->getMid(i) + pWaveform->getMid(i + 1)));

            pPainter->setPen(highColor);
            pPainter->drawLine(QPoint(x, 0),
                    QPoint(x, pWaveform->getHigh(i) + pWaveform->getHigh(i + 1)));
        }
    } else { // stereo
        for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
            x = i / 2;
            pPainter->setPen(lowColor);
            pPainter->drawLine(QPoint(x, -pWaveform->getLow(i)),
                    QPoint(x, pWaveform->getLow(i + 1)));

            pPainter->setPen(midColor);
            pPainter->drawLine(QPoint(x, -pWaveform->getMid(i)),
                    QPoint(x, pWaveform->getMid(i + 1)));

            pPainter->setPen(highColor);
            pPainter->drawLine(QPoint(x, -pWaveform->getHigh(i)),
                    QPoint(x, pWaveform->getHigh(i + 1)));
        }
    }

    if (start) {
        *start = end;
    }
}

void drawWaveformPartHSV(
        QPainter* pPainter,
        ConstWaveformPointer pWaveform,
        int* start,
        int end,
        const WaveformSignalColors& signalColors,
        bool mono) {
    ScopedTimer t(QStringLiteral("waveformOverviewRenderer::drawNextPixmapPartHSV"));
    int startVal = 0;
    if (start) {
        startVal = *start;
    }

    float h = 0, s = 0, v = 0, lo = 0, hi = 0, total = 0;
    // Get HSV of low color.
    const QColor lowColor = signalColors.getLowColor();
    getHsvF(lowColor, &h, &s, &v);
    QColor color;

    unsigned char low[2] = {0, 0};
    unsigned char high[2] = {0, 0};
    unsigned char mid[2] = {0, 0};
    unsigned char all[2] = {0, 0};

    if (mono) {
        // Mono means we're going to paint from bottom to top with l+r.
        const qreal dy = pPainter->deviceTransform().dy();
        pPainter->resetTransform();
        // shift y0 to bottom
        pPainter->translate(0, 2 * dy);
        // flip y-axis
        pPainter->scale(1, -1);
    }

    for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
        x = i / 2;
        all[0] = pWaveform->getAll(i);
        all[1] = pWaveform->getAll(i + 1);

        if (!all[0] && !all[1]) {
            continue;
        }

        low[0] = pWaveform->getLow(i);
        low[1] = pWaveform->getLow(i + 1);
        mid[0] = pWaveform->getMid(i);
        mid[1] = pWaveform->getMid(i + 1);
        high[0] = pWaveform->getHigh(i);
        high[1] = pWaveform->getHigh(i + 1);

        total = (low[0] + low[1] + mid[0] + mid[1] +
                        high[0] + high[1]) *
                1.2f;

        // Prevent division by zero
        if (total > 0) {
            // Normalize low and high
            // (mid not need, because it not change the color)
            lo = (low[0] + low[1]) / total;
            hi = (high[0] + high[1]) / total;
        } else {
            lo = hi = 0.0;
        }

        // Set color
        color.setHsvF(h, 1.0f - hi, 1.0f - lo);

        if (mono) {
            pPainter->setPen(color);
            pPainter->drawLine(QPoint(i / 2, 0),
                    QPoint(i / 2, all[0] + all[1]));
        } else {
            pPainter->setPen(color);
            pPainter->drawLine(QPoint(i / 2, -all[0]),
                    QPoint(i / 2, all[1]));
        }
    }

    if (start) {
        *start = end;
    }
}

void drawWaveformPartStem(
        QImage* pImage,
        ConstWaveformPointer pWaveform,
        int* start,
        int end,
        const QList<StemInfo>& stemInfo,
        const QVector<float>& stemGain) {
    ScopedTimer t(QStringLiteral("waveformOverviewRenderer::drawNextPixmapPartStem"));
    int startVal = 0;
    if (start) {
        startVal = *start;
    }

    const int stemCount = std::min({static_cast<int>(stemInfo.size()),
            static_cast<int>(stemGain.size()),
            mixxx::kMaxSupportedStems});
    if (!pImage || pImage->isNull() ||
            pImage->format() != QImage::Format_ARGB32_Premultiplied) {
        if (start) {
            *start = end;
        }
        return;
    }
    if (stemCount <= 0 || end <= startVal) {
        if (start) {
            *start = end;
        }
        return;
    }

    const WaveformData* pData = pWaveform->data();

    // Порядок слоёв выбираем один раз на весь отрезок по средней громкости:
    // громкие дорожки вниз, тихие поверх, иначе самая громкая закрыла бы
    // остальные. Считать порядок в каждом столбце было бы точнее, но тогда
    // перо приходится менять на каждую линию — это пятнадцать тысяч смен на
    // перерисовку, и станция от этого дёргалась.
    double sum[mixxx::kMaxSupportedStems] = {};
    for (int i = startVal; i < end; i++) {
        for (int stemIdx = 0; stemIdx < stemCount; stemIdx++) {
            sum[stemIdx] += pData[i].stems[stemIdx];
        }
    }
    int order[mixxx::kMaxSupportedStems];
    for (int stemIdx = 0; stemIdx < stemCount; stemIdx++) {
        order[stemIdx] = stemIdx;
    }
    for (int a = 1; a < stemCount; a++) {
        const int key = order[a];
        int b = a - 1;
        while (b >= 0 && sum[order[b]] < sum[key]) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = key;
    }

    // Один столбец — один заход по строкам картинки. Раньше здесь набиралось
    // до 15 000 QLine и рисовалось пером: сама отрисовка дёшева, а накладные
    // расходы пера давали стоп главного потока на сотни миллисекунд.
    const int width = pImage->width();
    const int height = pImage->height();
    const int centerY = height / 2;
    uchar* pBits = pImage->bits();
    const qsizetype stride = pImage->bytesPerLine();

    for (int k = 0; k < stemCount; k++) {
        const int stemIdx = order[k];
        const float gain = stemGain[stemIdx];
        if (gain <= 0.f) {
            continue;
        }
        const QRgb color = qPremultiply(stemInfo[stemIdx].getColor().rgba());
        for (int i = startVal, x = startVal / 2; i < end; i += 2, ++x) {
            if (x < 0 || x >= width) {
                continue;
            }
            const int up = static_cast<int>(pData[i].stems[stemIdx] * gain);
            const int down = static_cast<int>(pData[i + 1].stems[stemIdx] * gain);
            const int yTop = std::max(0, centerY - up);
            const int yBottom = std::min(height - 1, centerY + down);
            for (int y = yTop; y <= yBottom; y++) {
                reinterpret_cast<QRgb*>(pBits + y * stride)[x] = color;
            }
        }
    }

    if (start) {
        *start = end;
    }
}

} // namespace waveformOverviewRenderer
