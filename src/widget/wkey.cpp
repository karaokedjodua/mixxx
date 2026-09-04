#include "widget/wkey.h"

#include "library/library_prefs.h"
#include "moc_wkey.cpp"
#include "skin/legacy/skincontext.h"
#include "track/keyutils.h"
#include "util/color/predefinedcolorpalettes.h"

// static
std::optional<ColorPalette> WKey::s_keyColorPalette;

// static
void WKey::setKeyColorPalette(const ColorPalette& palette) {
    s_keyColorPalette = palette;
}

WKey::WKey(const QString& group, QWidget* pParent)
        : WLabel(pParent),
          m_dOldValue(0),
          m_keyNotation(mixxx::library::prefs::kKeyNotationConfigKey, this),
          m_engineKeyDistance(group,
                  "visual_key_distance",
                  this,
                  ControlFlag::AllowMissingOrInvalid) {
    setValue(m_dOldValue);
    m_keyNotation.connectValueChanged(this, &WKey::keyNotationChanged);
    m_engineKeyDistance.connectValueChanged(this, &WKey::setCents);
}

void WKey::onConnectedControlChanged(double dParameter, double dValue) {
    Q_UNUSED(dParameter);
    // Enums are not currently represented using parameter space so it doesn't
    // make sense to use the parameter here yet.
    setValue(dValue);
}

void WKey::setup(const QDomNode& node, const SkinContext& context) {
    WLabel::setup(node, context);
    m_displayCents = context.selectBool(node, "DisplayCents", false);
    m_displayKey = context.selectBool(node, "DisplayKey", true);
}

void WKey::setValue(double dValue) {
    m_dOldValue = dValue;
    mixxx::track::io::key::ChromaticKey key =
            KeyUtils::keyFromNumericValue(dValue);
    if (key != mixxx::track::io::key::INVALID) {
        // Render this key with the user-provided notation.
        QString keyStr = "";
        if (m_displayKey) {
            keyStr = KeyUtils::keyToString(key);
        }
        if (m_displayCents) {
            double diff_cents = m_engineKeyDistance.get();
            int cents_to_display = static_cast<int>(diff_cents * 100);
            char sign = ' ';
            if (diff_cents < 0) {
                sign = '-';
            } else if (diff_cents > 0) {
                sign = '+';
            }
            keyStr.append(QString(" %1%2c").arg(sign).arg(qAbs(cents_to_display)));
        }
        setText(keyStr);
    } else {
        setText("");
    }
    applyKeyColor(key);
}

void WKey::applyKeyColor(mixxx::track::io::key::ChromaticKey key) {
    // Цвет тональности в деке - тот же, что у колонки «Тональность» в
    // фонотеке. Так следующий трек подбирается глазами: увидел цвет деки,
    // ищешь в списке такой же или соседний.
    const ColorPalette& palette = s_keyColorPalette.has_value()
            ? s_keyColorPalette.value()
            : mixxx::PredefinedColorPalettes::kDefaultKeyColorPalette;
    const QColor color = (key == mixxx::track::io::key::INVALID)
            ? QColor()
            : KeyUtils::keyToColor(key, palette);
    if (color == m_appliedColor) {
        return;
    }
    m_appliedColor = color;
    if (!color.isValid()) {
        // Трека нет - возвращаем оформление скина.
        setStyleSheet(QString());
        return;
    }
    // Заливаем не фоном, а текстом и рамкой: цифра остаётся читаемой на любом
    // цвете палитры, а рамка видна с расстояния вытянутой руки.
    setStyleSheet(QStringLiteral("color: %1; border-color: %1;").arg(color.name()));
}

void WKey::setCents() {
    setValue(m_dOldValue);
}

void WKey::keyNotationChanged(double dKeyNotationValue) {
    Q_UNUSED(dKeyNotationValue);
    // NOTE: dKeyNotationValue is the index of the key notation type, NOT the
    // key itself, so we intentionally set the old value again to update the UI.
    setValue(m_dOldValue);
}
