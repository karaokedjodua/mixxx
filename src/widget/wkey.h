#pragma once

#include <optional>

#include "control/controlproxy.h"
#include "proto/keys.pb.h"
#include "util/color/colorpalette.h"
#include "widget/wlabel.h"

class WKey : public WLabel  {
    Q_OBJECT
  public:
    explicit WKey(const QString& group, QWidget* pParent = nullptr);

    void onConnectedControlChanged(double dParameter, double dValue) override;
    void setup(const QDomNode& node, const SkinContext& context) override;

    /// dj-station: та же палитра, что красит колонку тональности в фонотеке.
    /// Ставится при разборе скина, чтобы цифра тональности в деке светилась
    /// ровно тем же цветом, что и в списке треков - по цвету и подбирают.
    static void setKeyColorPalette(const ColorPalette& palette);

  private slots:
    void setValue(double dValue);
    void keyNotationChanged(double dValue);
    void setCents();

  private:
    void applyKeyColor(mixxx::track::io::key::ChromaticKey key);

    static std::optional<ColorPalette> s_keyColorPalette;

    double m_dOldValue;
    /// Последний применённый цвет: стиль переставляем только когда он менялся,
    /// иначе Qt на каждое движение питча перебирает стили заново.
    QColor m_appliedColor;
    bool m_displayCents;
    bool m_displayKey;
    ControlProxy m_keyNotation;
    ControlProxy m_engineKeyDistance;
};
