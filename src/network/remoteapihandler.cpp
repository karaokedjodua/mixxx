#include "network/remoteapihandler.h"

#include "moc_remoteapihandler.cpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include <algorithm>

#include "control/control.h"
#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "library/dao/playlistdao.h"
#include "library/trackcollection.h"
#include "controllers/controller.h"
#include "controllers/controllermanager.h"
#include "soundio/soundmanager.h"
#include "library/trackcollectionmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "network/remoteapihttp.h"
#include "preferences/configobject.h"
#include "soundio/soundmanager.h"
#include "track/beats.h"
#include "track/bpm.h"
#include "track/cue.h"
#include "track/track.h"
#include "track/trackref.h"
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
        ControllerManager* pControllerManager,
        SoundManager* pSoundManager,
        QObject* parent)
        : QObject(parent),
          m_pPlayerManager(pPlayerManager),
          m_pTrackCollectionManager(pTrackCollectionManager),
          m_pControllerManager(pControllerManager),
          m_pSoundManager(pSoundManager) {
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
        // Сколько пультов Mixxx реально держит открытыми. Сторожу этого хватает,
        // чтобы чинить состояние, а не ловить момент переподключения: пульт на
        // шине есть, а открытых ноль - значит пора пересканировать.
        int openControllers = 0;
        if (m_pControllerManager) {
            const QList<Controller*> controllers = m_pControllerManager->getControllers();
            for (const Controller* pController : controllers) {
                if (pController->isOpen()) {
                    openControllers++;
                }
            }
        }
        o.insert(QStringLiteral("controllers_open"), openControllers);
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

    if (resource == "devices") {
        if (parts.size() >= 4 && parts[3] == "rescan") {
            handleDevicesRescan(method, query, pReply);
            return;
        }
        replyError(pReply, 404, QStringLiteral("unknown devices call"));
        return;
    }

    if (resource == "library") {
        if (parts.size() >= 4 && parts[3] == "analysis") {
            if (method != "POST") {
                replyError(pReply, 405, QStringLiteral("POST required"));
                return;
            }
            handleLibraryAnalysis(body, pReply);
            return;
        }
        if (parts.size() >= 4 && parts[3] == "playlists") {
            handleLibraryPlaylists(method,
                    parts.size() >= 5 ? parts[4] : QByteArray(),
                    body,
                    pReply);
            return;
        }
        // POST /api/library/autodj/clear — очистить очередь Auto DJ
        if (parts.size() >= 5 && parts[3] == "autodj" && parts[4] == "clear") {
            if (method != "POST") {
                replyError(pReply, 405, QStringLiteral("POST required"));
                return;
            }
            if (!m_pTrackCollectionManager) {
                replyError(pReply, 503, QStringLiteral("no library"));
                return;
            }
            m_pTrackCollectionManager->internalCollection()->getPlaylistDAO().clearAutoDJQueue();
            reply(pReply, 200, QJsonObject{{QStringLiteral("cleared"), true}});
            return;
        }
        replyError(pReply, 404, QStringLiteral("unknown library call"));
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

void RemoteApiHandler::handleLibraryAnalysis(const QByteArray& body, RemoteApiReply* pReply) {
    if (!m_pTrackCollectionManager) {
        replyError(pReply, 503, QStringLiteral("no library"));
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(body).object();

    TrackPointer pTrack;
    const QString path = o.value(QStringLiteral("path")).toString();
    if (!path.isEmpty()) {
        pTrack = m_pTrackCollectionManager->getOrAddTrack(TrackRef::fromFilePath(path));
    } else {
        const QJsonValue idValue = o.value(QStringLiteral("track_id"));
        if (!idValue.isUndefined() && !idValue.isNull()) {
            pTrack = m_pTrackCollectionManager->getTrackById(TrackId(idValue.toVariant()));
        }
    }
    if (!pTrack) {
        replyError(pReply, 404, QStringLiteral("no such track"));
        return;
    }

    // Позиции внутри Mixxx — в кадрах, а снаружи приходят секунды: нужна
    // частота дискретизации. Она известна после сканирования меток файла.
    const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
    const double rate = sampleRate.isValid() ? static_cast<double>(sampleRate) : 0.0;

    QJsonObject result;
    result.insert(QStringLiteral("track"), trackJson(pTrack));

    if (o.contains(QStringLiteral("bpm"))) {
        const double bpmValue = o.value(QStringLiteral("bpm")).toDouble();
        const mixxx::Bpm bpm(bpmValue);
        if (!bpm.isValid() || bpmValue > mixxx::Bpm::kValueMax) {
            replyError(pReply, 400, QStringLiteral("bad bpm"));
            return;
        }
        if (rate <= 0.0) {
            replyError(pReply, 409, QStringLiteral("sample rate unknown yet"));
            return;
        }
        const double anchorSec = o.value(QStringLiteral("beat_anchor_sec")).toDouble(0.0);
        const mixxx::audio::FramePos anchor(std::max(0.0, anchorSec) * rate);
        const auto pBeats = mixxx::Beats::fromConstTempo(
                sampleRate, anchor, bpm, QStringLiteral("vdj-import"));
        result.insert(QStringLiteral("beats_set"), pTrack->trySetBeats(pBeats));
        result.insert(QStringLiteral("bpm"), pTrack->getBpm());
    }

    if (o.contains(QStringLiteral("key"))) {
        const QString key = o.value(QStringLiteral("key")).toString();
        if (!key.isEmpty()) {
            pTrack->setKeyText(key, mixxx::track::io::key::USER);
        }
        result.insert(QStringLiteral("key"), pTrack->getKeyText());
    }

    if (o.contains(QStringLiteral("cues"))) {
        if (rate <= 0.0) {
            replyError(pReply, 409, QStringLiteral("sample rate unknown yet"));
            return;
        }
        if (o.value(QStringLiteral("replace_cues")).toBool(false)) {
            pTrack->removeCuesOfType(mixxx::CueType::HotCue);
        }
        int written = 0;
        const QJsonArray cues = o.value(QStringLiteral("cues")).toArray();
        for (const QJsonValue& v : cues) {
            const QJsonObject c = v.toObject();
            // У VirtualDJ метки нумеруются с единицы, у Mixxx — с нуля.
            const int hotcue = c.value(QStringLiteral("num")).toInt(0) - 1;
            const double posSec = c.value(QStringLiteral("pos_sec")).toDouble(-1.0);
            if (hotcue < 0 || hotcue >= 64 || posSec < 0.0) {
                continue;
            }
            const mixxx::audio::FramePos start(posSec * rate);
            CuePointer pCue;
            const QList<CuePointer> existing = pTrack->getCuePoints();
            for (const CuePointer& pExisting : existing) {
                if (pExisting->getHotCue() == hotcue) {
                    pCue = pExisting;
                    break;
                }
            }
            if (pCue) {
                pCue->setStartAndEndPosition(start, mixxx::audio::kInvalidFramePos);
            } else {
                pCue = pTrack->createAndAddCue(
                        mixxx::CueType::HotCue, hotcue, start, mixxx::audio::kInvalidFramePos);
            }
            const QString label = c.value(QStringLiteral("name")).toString();
            if (!label.isEmpty()) {
                pCue->setLabel(label);
            }
            written++;
        }
        result.insert(QStringLiteral("cues_written"), written);
    }

    m_pTrackCollectionManager->saveTrack(pTrack);
    reply(pReply, 200, result);
}

// dj-station: плейлисты с пульта (подбор ИИ / по правилам). Пишем через
// PlaylistDAO в главном потоке, а не в базу мимо Mixxx: тогда боковая панель
// библиотеки обновляется сразу, а не после перезапуска.
//   GET  -> {playlists:[{id,name,tracks,locked}]}
//   POST {name, track_ids:[...], replace:true, autodj:""|"top"|"bottom"}
//        -> {id, name, created, tracks}
// dj-station: выдернутый пульт Mixxx сам не подхватывает - горячего
// подключения устройств в нём нет, и до сих пор это лечилось перезапуском
// Mixxx, то есть обрывом музыки посреди вечера. DDJ-SX для станции - одно
// устройство и для нот, и для звука, поэтому поднимаем обе половины.
void RemoteApiHandler::handleDevicesRescan(const QByteArray& method,
        const QByteArray& query,
        RemoteApiReply* pReply) {
    if (method != "POST") {
        replyError(pReply, 405, QStringLiteral("POST required"));
        return;
    }

    const QMap<QByteArray, QByteArray> q = remoteapi::parseQuery(query);
    const auto flag = [&q](const char* name, bool byDefault) {
        const auto it = q.find(QByteArray(name));
        if (it == q.end()) {
            return byDefault;
        }
        return it.value() != QByteArrayLiteral("0");
    };
    const bool wantAudio = flag("audio", true);
    const bool wantMidi = flag("midi", true);
    const bool force = flag("force", false);

    QJsonObject result;

    // Звук. Переоткрывать живую карту нельзя без причины: это разрыв звука
    // посреди сета, а на этой машине ещё и полтора гигабайта невозвращённой
    // памяти за вызов. Поэтому по умолчанию трогаем, только когда звука нет.
    const bool audioAlive = controlOrZero(QStringLiteral("[SoundManager]"),
                                    QStringLiteral("status")) ==
            SOUNDMANAGER_CONNECTED;
    if (!m_pSoundManager || !wantAudio) {
        result.insert(QStringLiteral("audio"), audioAlive);
        result.insert(QStringLiteral("audioReopened"), false);
    } else if (audioAlive && !force) {
        result.insert(QStringLiteral("audio"), true);
        result.insert(QStringLiteral("audioReopened"), false);
    } else {
        // clearAndQueryDevices закрывает карты и заново перечисляет их через
        // PortAudio; setupDevices открывает их ТЕМ ЖЕ конфигом, так что
        // маршрутизация Master/Headphones остаётся как была.
        m_pSoundManager->clearAndQueryDevices();
        const SoundDeviceStatus status = m_pSoundManager->setupDevices();
        const bool ok = status == SoundDeviceStatus::Ok;
        result.insert(QStringLiteral("audio"), ok);
        result.insert(QStringLiteral("audioReopened"), true);
        if (!ok) {
            result.insert(QStringLiteral("audioError"),
                    m_pSoundManager->getLastErrorMessage(status));
        }
    }

    // Пульт. Запрос уходит в поток контроллеров очередью - здесь только просим,
    // поэтому "true" значит "попросили", а не "открылся". Открылся или нет,
    // видно по controllers_open в /api/status.
    if (m_pControllerManager && wantMidi) {
        m_pControllerManager->setUpDevices();
        result.insert(QStringLiteral("midi"), true);
    } else {
        result.insert(QStringLiteral("midi"), false);
    }

    reply(pReply, 200, result);
}

void RemoteApiHandler::handleLibraryPlaylists(const QByteArray& method,
        const QByteArray& idPart,
        const QByteArray& body,
        RemoteApiReply* pReply) {
    if (!m_pTrackCollectionManager) {
        replyError(pReply, 503, QStringLiteral("no library"));
        return;
    }
    PlaylistDAO& dao = m_pTrackCollectionManager->internalCollection()->getPlaylistDAO();

    if (!idPart.isEmpty()) {
        bool ok = false;
        const int playlistId = idPart.toInt(&ok);
        if (!ok || !dao.playlistExists(playlistId)) {
            replyError(pReply, 404, QStringLiteral("no such playlist"));
            return;
        }
        if (method == "GET") {
            QJsonArray ids;
            const QList<TrackId> trackIds = dao.getTrackIdsInPlaylistOrder(playlistId);
            for (const TrackId& trackId : trackIds) {
                ids.append(trackId.toVariant().toInt());
            }
            reply(pReply, 200,
                    QJsonObject{{QStringLiteral("id"), playlistId},
                            {QStringLiteral("name"), dao.getPlaylistName(playlistId)},
                            {QStringLiteral("locked"), dao.isPlaylistLocked(playlistId)},
                            {QStringLiteral("track_ids"), ids}});
            return;
        }
        if (method == "DELETE") {
            if (dao.isPlaylistLocked(playlistId)) {
                replyError(pReply, 409, QStringLiteral("playlist is locked"));
                return;
            }
            dao.deletePlaylist(playlistId);
            reply(pReply, 200, QJsonObject{{QStringLiteral("deleted"), playlistId}});
            return;
        }
        replyError(pReply, 405, QStringLiteral("GET or DELETE required"));
        return;
    }

    if (method == "GET") {
        QJsonArray list;
        const auto playlists = dao.getPlaylists(PlaylistDAO::PLHT_NOT_HIDDEN);
        for (const auto& pair : playlists) {
            QJsonObject p;
            p.insert(QStringLiteral("id"), pair.first);
            p.insert(QStringLiteral("name"), pair.second);
            p.insert(QStringLiteral("tracks"), static_cast<int>(dao.getTrackIds(pair.first).size()));
            p.insert(QStringLiteral("locked"), dao.isPlaylistLocked(pair.first));
            list.append(p);
        }
        reply(pReply, 200, QJsonObject{{QStringLiteral("playlists"), list}});
        return;
    }
    if (method != "POST") {
        replyError(pReply, 405, QStringLiteral("GET or POST required"));
        return;
    }

    const QJsonObject o = QJsonDocument::fromJson(body).object();
    const QString name = o.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) {
        replyError(pReply, 400, QStringLiteral("name required"));
        return;
    }
    const QJsonValue idsValue = o.value(QStringLiteral("track_ids"));
    if (!idsValue.isArray()) {
        replyError(pReply, 400, QStringLiteral("track_ids must be an array"));
        return;
    }
    // Чужой id — ошибка целиком, а не молчаливая дыра в плейлисте.
    QList<TrackId> trackIds;
    const QJsonArray idsArray = idsValue.toArray();
    for (const QJsonValue& v : idsArray) {
        const TrackId trackId(v.toVariant());
        if (!trackId.isValid() || !m_pTrackCollectionManager->getTrackById(trackId)) {
            replyError(pReply, 400,
                    QStringLiteral("no such track: %1").arg(v.toVariant().toString()));
            return;
        }
        trackIds.append(trackId);
    }
    const bool replace = o.value(QStringLiteral("replace")).toBool(true);

    int playlistId = dao.getPlaylistIdFromName(name);
    bool created = false;
    if (playlistId >= 0 && replace) {
        if (dao.isPlaylistLocked(playlistId)) {
            replyError(pReply, 409, QStringLiteral("playlist is locked"));
            return;
        }
        dao.deletePlaylist(playlistId);
        playlistId = -1;
    }
    if (playlistId < 0) {
        playlistId = dao.createPlaylist(name);
        created = true;
    }
    if (playlistId < 0) {
        replyError(pReply, 500, QStringLiteral("could not create playlist"));
        return;
    }
    if (!trackIds.isEmpty() && !dao.appendTracksToPlaylist(trackIds, playlistId)) {
        replyError(pReply, 500, QStringLiteral("could not add tracks"));
        return;
    }
    const QString autodj = o.value(QStringLiteral("autodj")).toString();
    if (!autodj.isEmpty() && dao.getPlaylistIdFromName(QStringLiteral("Auto DJ")) < 0) {
        // Очередь Auto DJ (AUTODJ_TABLE) заводит AutoDJFeature; в тестах её нет.
        dao.createPlaylist(QStringLiteral("Auto DJ"), PlaylistDAO::PLHT_AUTO_DJ);
    }
    if (autodj == QLatin1String("top")) {
        dao.addPlaylistToAutoDJQueue(playlistId, PlaylistDAO::AutoDJSendLoc::TOP);
    } else if (autodj == QLatin1String("bottom")) {
        dao.addPlaylistToAutoDJQueue(playlistId, PlaylistDAO::AutoDJSendLoc::BOTTOM);
    }

    QJsonObject result;
    result.insert(QStringLiteral("id"), playlistId);
    result.insert(QStringLiteral("name"), name);
    result.insert(QStringLiteral("created"), created);
    result.insert(QStringLiteral("tracks"), static_cast<int>(dao.getTrackIds(playlistId).size()));
    reply(pReply, 200, result);
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
