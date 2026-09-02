#pragma once

#include <QMimeType>

#include "track/steminfo.h"

namespace mixxx {

/// Importer class for StemInfo objects that contains metadata about the stems of a track.
class StemInfoImporter {
  public:
    static QList<StemInfo> importStemInfos(
            const QString& filePath);

    static bool maybeStemFile(
            const QString& aFileName, QMimeType mimeType = QMimeType());

    static bool hasStemAtom(
            const QString& aFileName);

    /// dj-station: путь к стемам VirtualDJ, лежащим рядом с треком файлом
    /// "<имя трека>.vdjstems", либо пустая строка. Так их раскладывает сам
    /// VirtualDJ, и так же поступаем мы: трек остаётся в фонотеке одной
    /// записью со своими метками, а стемы подхватываются сами.
    static QString vdjStemsSidecarPath(
            const QString& filePath);

    /// dj-station: правда ли, что этот файл — стемы при каком-то треке,
    /// лежащем рядом. Такие файлы не попадают в фонотеку отдельной записью.
    static bool isVdjStemsSidecarOf(
            const QString& filePath);
};

} // namespace mixxx
