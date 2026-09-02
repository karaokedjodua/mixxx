#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSemaphore>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <memory>

#include "track/track_decl.h"

class ControlProxy;
class PlayerManager;
class TrackCollectionManager;

namespace mixxx {

/// dj-station: ответ, который сетевой поток ждёт от главного. Живёт в
/// shared_ptr у обеих сторон: если сетевой поток устал ждать и ушёл (504),
/// он ставит abandoned, и главный поток просто не пишет в брошенный буфер.
struct RemoteApiReply {
    std::atomic<bool> abandoned{false};
    QSemaphore done;
    int status{500};
    QByteArray contentType{"application/json"};
    QByteArray body;
};

/// Вся настоящая работа сетевого управления. Объект живёт в ГЛАВНОМ потоке:
/// только оттуда можно грузить треки (база и кэш треков привязаны к потоку),
/// создавать ControlProxy и трогать PlayerManager. Сетевой поток лишь
/// передаёт сюда запросы через очередь событий и ждёт семафор.
///
/// Записи в ControlObject идут через ControlObject::exists — обращение к
/// несуществующему ключу в Mixxx не безобидно: там стоит DEBUG_ASSERT.
class RemoteApiHandler : public QObject {
    Q_OBJECT
  public:
    RemoteApiHandler(PlayerManager* pPlayerManager,
            TrackCollectionManager* pTrackCollectionManager,
            QObject* parent = nullptr);
    ~RemoteApiHandler() override;

    /// Синхронная обработка. Вызывать только из потока, где живёт объект.
    void handleNow(const QByteArray& method,
            const QByteArray& path,
            const QByteArray& query,
            const QByteArray& body,
            RemoteApiReply* pReply);

    /// Подписка на изменения ControlObject; ключи вида "[Channel1],playposition".
    /// Несуществующие пропускаются. Вызывать только из потока объекта.
    void subscribeNow(const QStringList& keys);

    static QString deckGroup(int deckNumber) {
        return QStringLiteral("[Channel%1]").arg(deckNumber);
    }

  signals:
    /// Событие для потока SSE: имя ("track" или "control") и JSON.
    void event(QByteArray name, QByteArray json);

  private slots:
    void slotFlushControlEvents();
    void slotTrackChanged(const QString& group, TrackPointer pNewTrack, TrackPointer pOldTrack);

  private:
    QJsonObject deckJson(int deckNumber) const;
    QJsonObject trackJson(const TrackPointer& pTrack) const;
    void reply(RemoteApiReply* pReply, int status, const QJsonObject& json) const;
    void replyError(RemoteApiReply* pReply, int status, const QString& message) const;
    bool pressButton(const QString& group, const QString& item) const;
    void handleDeckAction(int deckNumber,
            const QByteArray& action,
            const QByteArray& body,
            RemoteApiReply* pReply);
    void handleControl(const QByteArray& method,
            const QByteArray& query,
            const QByteArray& body,
            RemoteApiReply* pReply);
    void handleControlsBatch(const QByteArray& body, RemoteApiReply* pReply);
    void handleControlsList(RemoteApiReply* pReply);
    /// dj-station: записать в трек готовый разбор извне — BPM с якорем
    /// сетки, тональность, горячие метки. Так на станцию попадают данные
    /// VirtualDJ без пересчёта на слабом планшете.
    void handleLibraryAnalysis(const QByteArray& body, RemoteApiReply* pReply);

    PlayerManager* m_pPlayerManager;
    TrackCollectionManager* m_pTrackCollectionManager;
    QHash<QString, ControlProxy*> m_proxies;
    QHash<QString, double> m_pendingControls;
    QTimer m_flushTimer;
};

} // namespace mixxx
