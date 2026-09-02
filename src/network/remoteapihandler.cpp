#include "network/remoteapihandler.h"

#include "moc_remoteapihandler.cpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include "control/control.h"
#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "library/trackcollectionmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "network/remoteapihttp.h"
#include "preferences/configobject.h"
#include "soundio/soundmanager.h"
#include "track/track.h"
#include "util/logger.h"

namespace mixxx {

namespace {

const Logger kLogger("RemoteApiHandler");

// Склейка изменений ControlObject перед отправкой: playposition пишется
// 15 раз в секунду на деку, и без склейки поток событий захлебнётся.
constexpr int kFlushIntervalMs = 66;

QByteArray toJson(const QJsonObject& obj) {
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

double controlOrZero(const QString& group, const QString& item) {
    const ConfigKey key(group, item);
    if (!ControlObject::exists(key)) {
        return 0.0;
    }
    return ControlObject::get(key);
}

int parseDeckNumber(const QByteArray& segment) {
    bool ok = false;
    const int n = segment.toInt(&ok);
    return (ok && n >= 1) ? n : 0;
}

} // anonymous namespace

RemoteApiHandler::RemoteApiHandler(PlayerManager* pPlayerManager,
        TrackCollectionManager* pTrackCollectionManager,
        QObject* parent)
        : QObject(parent),
          m_pPlayerManager(pPlayerManager),
          m_pTrackCollectionManager(pTrackCollectionManager) {
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(kFlushIntervalMs);
    connect(&m_flushTimer, &QTimer::timeout, this, &RemoteApiHandler::slotFlushControlEvents);
    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            &RemoteApiHandler::slotTrackChanged);
}

RemoteApiHandler::~RemoteApiHandler() = default;

QJsonObject RemoteApiHandler::trackJson(const TrackPointer& pTrack) const {
    QJsonObject t;
    if (!pTrack) {
        return t;
    }
    t.insert(QStringLiteral("track_id"), QJsonValue::fromVariant(pTrack->getId().toVariant()));
    t.insert(QStringLiteral("location"), pTrack->getLocation());
    t.insert(QStringLiteral("title"), pTrack->getTitle());
    t.insert(QStringLiteral("artist"), pTrack->getArtist());
    t.insert(QStringLiteral("duration"), pTrack->getDurationSecondsInt());
    t.insert(QStringLiteral("bpm"), pTrack->getBpm());
    t.insert(QStringLiteral("key"), pTrack->getKeyText());
#ifdef __STEM__
    t.insert(QStringLiteral("stems"), pTrack->getStemInfo().size());
#endif
    return t;
}

QJsonObject RemoteApiHandler::deckJson(int deckNumber) const {
    const QString group = deckGroup(deckNumber);
    QJsonObject d;
    d.insert(QStringLiteral("deck"), deckNumber);
    d.insert(QStringLiteral("group"), group);
    BaseTrackPlayer* pPlayer = m_pPlayerManager->getPlayer(group);
    const TrackPointer pTrack = pPlayer ? pPlayer->getLoadedTrack() : TrackPointer();
    d.insert(QStringLiteral("loaded"), static_cast<bool>(pTrack));
    d.insert(QStringLiteral("play"), controlOrZero(group, QStringLiteral("play")) > 0.5);
    d.insert(QStringLiteral("playposition"), controlOrZero(group, QStringLiteral("playposition")));
    d.insert(QStringLiteral("bpm"), controlOrZero(group, QStringLiteral("bpm")));
    d.insert(QStringLiteral("rate"), controlOrZero(group, QStringLiteral("rate")));
    d.insert(QStringLiteral("duration"), controlOrZero(group, QStringLiteral("duration")));
#ifdef __STEM__
    d.insert(QStringLiteral("stem_count"), controlOrZero(group, QStringLiteral("stem_count")));
#endif
    if (pTrack) {
        d.insert(QStringLiteral("track"), trackJson(pTrack));
    }
    return d;
}

void RemoteApiHandler::reply(RemoteApiReply* pReply, int status, const QJsonObject& json) const {
    pReply->status = status;
    pReply->contentType = QByteArrayLiteral("application/json");
    pReply->body = toJson(json);
}

void RemoteApiHandler::replyError(RemoteApiReply* pReply, int status, const QString& message) const {
    QJsonObject o;
    o.insert(QStringLiteral("error"), message);
    reply(pReply, status, o);
}

bool RemoteApiHandler::pressButton(const QString& group, const QString& item) const {
    const ConfigKey key(group, item);
    if (!ControlObject::exists(key)) {
        return false;
    }
    // Кнопочные ControlObject ждут пару 1/0, иначе остаются «нажатыми».
    ControlObject::set(key, 1.0);
    ControlObject::set(key, 0.0);
    return true;
}

void RemoteApiHandler::handleNow(const QByteArray& method,
        const QByteArray& path,
        const QByteArray& query,
        const QByteArray& body,
        RemoteApiReply* pReply) {
    if (pReply->abandoned) {
        return;
    }
    const QList<QByteArray> parts = path.split('/');
    // path вида /api/decks/1/load -> ["", "api", "decks", "1", "load"]
    if (parts.size() < 3 || parts[1] != "api") {
        replyError(pReply, 404, QStringLiteral("unknown path"));
        return;
    }
    const QByteArray& resource = parts[2];

    if (resource == "status") {
        QJsonObject o;
        o.insert(QStringLiteral("app"), QStringLiteral("mixxx-dj-station"));
        const double sound = controlOrZero(QStringLiteral("[SoundManager]"), QStringLiteral("status"));
        o.insert(QStringLiteral("sound_status"), sound);
        o.insert(QStringLiteral("sound_ready"), sound == SOUNDMANAGER_CONNECTED);
        o.insert(QStringLiteral("decks"), m_pPlayerManager->numberOfDecks());
        reply(pReply, 200, o);
        return;
    }

    if (resource == "decks") {
        if (parts.size() == 3) {
            QJsonArray arr;
            for (int n = 1; n <= m_pPlayerManager->numberOfDecks(); n++) {
                arr.append(deckJson(n));
            }
            QJsonObject o;
            o.insert(QStringLiteral("decks"), arr);
            reply(pReply, 200, o);
            return;
        }
        const int n = parseDeckNumber(parts[3]);
        if (n == 0 || n > m_pPlayerManager->numberOfDecks()) {
            replyError(pReply, 404, QStringLiteral("no such deck"));
            return;
        }
        if (parts.size() == 4) {
            reply(pReply, 200, deckJson(n));
            return;
        }
        if (method != "POST") {
            replyError(pReply, 405, QStringLiteral("POST required"));
            return;
        }
        handleDeckAction(n, parts[4], body, pReply);
        return;
    }

    if (resource == "control") {
        handleControl(method, query, body, pReply);
        return;
    }

    if (resource == "controls") {
        if (method == "POST") {
            handleControlsBatch(body, pReply);
        } else {
            handleControlsList(pReply);
        }
        return;
    }

    replyError(pReply, 404, QStringLiteral("unknown resource"));
}

void RemoteApiHandler::handleDeckAction(int deckNumber,
        const QByteArray& action,
        const QByteArray& body,
        RemoteApiReply* pReply) {
    const QString group = deckGroup(deckNumber);
    const ConfigKey playKey(group, QStringLiteral("play"));

    if (action == "load") {
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        const bool play = o.value(QStringLiteral("play")).toBool(false);
        const QString path = o.value(QStringLiteral("path")).toString();
        if (!path.isEmpty()) {
            m_pPlayerManager->slotLoadLocationToPlayer(path, group, play);
            QJsonObject r;
            r.insert(QStringLiteral("deck"), deckNumber);
            r.insert(QStringLiteral("path"), path);
            reply(pReply, 202, r);
            return;
        }
        const QJsonValue idValue = o.value(QStringLiteral("track_id"));
        if (!idValue.isUndefined() && !idValue.isNull()) {
            const TrackId trackId(idValue.toVariant());
            const TrackPointer pTrack = m_pTrackCollectionManager
                    ? m_pTrackCollectionManager->getTrackById(trackId)
                    : TrackPointer();
            if (!pTrack) {
                replyError(pReply, 404, QStringLiteral("no such track"));
                return;
            }
            m_pPlayerManager->slotLoadLocationToPlayer(pTrack->getLocation(), group, play);
            QJsonObject r;
            r.insert(QStringLiteral("deck"), deckNumber);
            r.insert(QStringLiteral("track"), trackJson(pTrack));
            reply(pReply, 202, r);
            return;
        }
        replyError(pReply, 400, QStringLiteral("path or track_id required"));
        return;
    }

    if (!ControlObject::exists(playKey)) {
        replyError(pReply, 404, QStringLiteral("deck has no play control"));
        return;
    }

    if (action == "play") {
        ControlObject::set(playKey, 1.0);
    } else if (action == "pause") {
        ControlObject::set(playKey, 0.0);
    } else if (action == "toggle") {
        ControlObject::set(playKey, ControlObject::get(playKey) > 0.5 ? 0.0 : 1.0);
    } else if (action == "cue") {
        pressButton(group, QStringLiteral("cue_default"));
    } else if (action == "sync") {
        pressButton(group, QStringLiteral("beatsync"));
    } else if (action == "eject") {
        // На играющей деке eject молча игнорируется — сначала останавливаем.
        if (ControlObject::get(playKey) > 0.5) {
            ControlObject::set(playKey, 0.0);
        }
        pressButton(group, QStringLiteral("eject"));
    } else {
        replyError(pReply, 404, QStringLiteral("unknown deck action"));
        return;
    }
    reply(pReply, 200, deckJson(deckNumber));
}

void RemoteApiHandler::handleControl(const QByteArray& method,
        const QByteArray& query,
        const QByteArray& body,
        RemoteApiReply* pReply) {
    QString group;
    QString item;
    bool hasValue = false;
    double value = 0.0;

    if (method == "POST") {
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        group = o.value(QStringLiteral("group")).toString();
        item = o.value(QStringLiteral("item")).toString();
        const QJsonValue v = o.value(QStringLiteral("value"));
        if (v.isDouble() || v.isBool()) {
            hasValue = true;
            value = v.isBool() ? (v.toBool() ? 1.0 : 0.0) : v.toDouble();
        }
    } else {
        const auto q = remoteapi::parseQuery(query);
        group = QString::fromUtf8(q.value("group"));
        item = QString::fromUtf8(q.value("item"));
    }

    if (group.isEmpty() || item.isEmpty()) {
        replyError(pReply, 400, QStringLiteral("group and item required"));
        return;
    }
    const ConfigKey key(group, item);
    if (!ControlObject::exists(key)) {
        replyError(pReply, 404, QStringLiteral("no such control"));
        return;
    }
    if (method == "POST") {
        if (!hasValue) {
            replyError(pReply, 400, QStringLiteral("value required"));
            return;
        }
        ControlObject::set(key, value);
    }
    QJsonObject o;
    o.insert(QStringLiteral("group"), group);
    o.insert(QStringLiteral("item"), item);
    o.insert(QStringLiteral("value"), ControlObject::get(key));
    reply(pReply, 200, o);
}

void RemoteApiHandler::handleControlsBatch(const QByteArray& body, RemoteApiReply* pReply) {
    const QJsonArray arr = QJsonDocument::fromJson(body).array();
    QJsonArray results;
    for (const QJsonValue& entry : arr) {
        const QJsonObject o = entry.toObject();
        const QString group = o.value(QStringLiteral("group")).toString();
        const QString item = o.value(QStringLiteral("item")).toString();
        QJsonObject r;
        r.insert(QStringLiteral("group"), group);
        r.insert(QStringLiteral("item"), item);
        const ConfigKey key(group, item);
        if (group.isEmpty() || item.isEmpty() || !ControlObject::exists(key)) {
            r.insert(QStringLiteral("error"), QStringLiteral("no such control"));
        } else {
            const QJsonValue v = o.value(QStringLiteral("value"));
            if (v.isDouble() || v.isBool()) {
                ControlObject::set(key, v.isBool() ? (v.toBool() ? 1.0 : 0.0) : v.toDouble());
            }
            r.insert(QStringLiteral("value"), ControlObject::get(key));
        }
        results.append(r);
    }
    QJsonObject o;
    o.insert(QStringLiteral("results"), results);
    reply(pReply, 200, o);
}

void RemoteApiHandler::handleControlsList(RemoteApiReply* pReply) {
    QJsonArray arr;
    const auto instances = ControlDoublePrivate::getAllInstances();
    for (const auto& pControl : instances) {
        if (!pControl) {
            continue;
        }
        const ConfigKey& key = pControl->getKey();
        QJsonObject o;
        o.insert(QStringLiteral("group"), key.group);
        o.insert(QStringLiteral("item"), key.item);
        o.insert(QStringLiteral("value"), pControl->get());
        arr.append(o);
    }
    QJsonObject o;
    o.insert(QStringLiteral("controls"), arr);
    reply(pReply, 200, o);
}

void RemoteApiHandler::subscribeNow(const QStringList& keys) {
    for (const QString& keyText : keys) {
        const QString trimmed = keyText.trimmed();
        if (trimmed.isEmpty() || m_proxies.contains(trimmed)) {
            continue;
        }
        const ConfigKey key = ConfigKey::parseCommaSeparated(trimmed);
        if (!key.isValid() || !ControlObject::exists(key)) {
            kLogger.debug() << "subscribe: no such control" << trimmed;
            continue;
        }
        // ControlProxy обязан создаваться и жить в одном потоке — в нашем.
        auto* pProxy = new ControlProxy(key, this);
        pProxy->connectValueChanged(this, [this, trimmed](double value) {
            m_pendingControls.insert(trimmed, value);
            if (!m_flushTimer.isActive()) {
                m_flushTimer.start();
            }
        });
        m_proxies.insert(trimmed, pProxy);
    }
}

void RemoteApiHandler::slotFlushControlEvents() {
    if (m_pendingControls.isEmpty()) {
        return;
    }
    QJsonObject o;
    for (auto it = m_pendingControls.cbegin(); it != m_pendingControls.cend(); ++it) {
        o.insert(it.key(), it.value());
    }
    m_pendingControls.clear();
    emit event(QByteArrayLiteral("control"), toJson(o));
}

void RemoteApiHandler::slotTrackChanged(const QString& group,
        TrackPointer pNewTrack,
        TrackPointer pOldTrack) {
    Q_UNUSED(pOldTrack);
    QJsonObject o;
    o.insert(QStringLiteral("group"), group);
    o.insert(QStringLiteral("loaded"), static_cast<bool>(pNewTrack));
    if (pNewTrack) {
        o.insert(QStringLiteral("track"), trackJson(pNewTrack));
    }
    emit event(QByteArrayLiteral("track"), toJson(o));
}

} // namespace mixxx
