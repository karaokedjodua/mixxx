#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTcpSocket>
#include <QTest>
#include <gsl/pointers>

#include <functional>

#include "control/controlindicatortimer.h"
#include "control/controlobject.h"
#include "database/mixxxdb.h"
#include "effects/effectsmanager.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "engine/enginemixer.h"
#include "library/coverartcache.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/deck.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "network/remoteapihandler.h"
#include "network/remoteapiserver.h"
#include "sources/soundsourceproxy.h"
#include "test/mixxxdbtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/cue.h"
#include "track/track.h"
#ifdef __RUBBERBAND__
#include "engine/bufferscalers/rubberbandworkerpool.h"
#endif

// dj-station: сетевое управление на настоящем PlayerManager и библиотеке.
// Обработчик живёт в потоке теста (он же «главный»), сервер — в своём.
// Ответ сервера ждём, прокручивая цикл событий: иначе очередной вызов
// обработчика в наш поток никогда не выполнится.

namespace {

const QString kTrackLocation = QStringLiteral("id3-test-data/cover-test-øé~ł€˚-png.mp3");

void deleteTrack(Track* pTrack) {
    delete pTrack;
}

QJsonObject bodyJson(const mixxx::RemoteApiReply& reply) {
    return QJsonDocument::fromJson(reply.body).object();
}

} // namespace

class RemoteApiTest : public MixxxDbTest, SoundSourceProviderRegistration {
  public:
    RemoteApiTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        auto pChannelHandleFactory = std::make_shared<ChannelHandleFactory>();
        m_pEffectsManager = std::make_shared<EffectsManager>(m_pConfig, pChannelHandleFactory);
        m_pEngine = std::make_shared<EngineMixer>(
                m_pConfig, "[Master]", m_pEffectsManager.get(), pChannelHandleFactory, true);
        m_pSoundManager = std::make_shared<SoundManager>(m_pConfig, m_pEngine.get());
        m_pControlIndicatorTimer = std::make_shared<mixxx::ControlIndicatorTimer>(nullptr);
        m_pEngine->registerNonEngineChannelSoundIO(gsl::make_not_null(m_pSoundManager.get()));
        CoverArtCache::createInstance();

        m_pPlayerManager = std::make_shared<PlayerManager>(
                m_pConfig, m_pSoundManager.get(), m_pEffectsManager.get(), m_pEngine.get());
        m_pPlayerManager->addConfiguredDecks();
        m_pPlayerManager->addSampler();
        PlayerInfo::create();
        m_pEffectsManager->setup();

        const auto dbConnection = mixxx::DbConnectionPooled(dbConnectionPooler());
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(dbConnection));
        m_pTrackCollectionManager = std::make_unique<TrackCollectionManager>(
                nullptr, m_pConfig, dbConnectionPooler(), deleteTrack);
        m_pRecordingManager = std::make_shared<RecordingManager>(m_pConfig, m_pEngine.get());
        m_pLibrary = std::make_shared<Library>(nullptr,
                m_pConfig,
                dbConnectionPooler(),
                m_pTrackCollectionManager.get(),
                m_pPlayerManager.get(),
                m_pRecordingManager.get());
        m_pPlayerManager->bindToLibrary(m_pLibrary.get());
#ifdef __RUBBERBAND__
        RubberBandWorkerPool::createInstance();
#endif
        m_pHandler = std::make_unique<mixxx::RemoteApiHandler>(
                m_pPlayerManager.get(), m_pTrackCollectionManager.get());
    }

    void TearDown() override {
        m_pHandler.reset();
        CoverArtCache::destroy();
#ifdef __RUBBERBAND__
        RubberBandWorkerPool::destroy();
#endif
    }

    // Ответ содержит семафор и атомик — копировать его нельзя, держим в shared_ptr.
    std::shared_ptr<mixxx::RemoteApiReply> call(const char* method,
            const QByteArray& path,
            const QByteArray& query = QByteArray(),
            const QByteArray& body = QByteArray()) {
        auto pReply = std::make_shared<mixxx::RemoteApiReply>();
        m_pHandler->handleNow(method, path, query, body, pReply.get());
        return pReply;
    }

    // Без звуковой карты движок никто не крутит, а загрузка трека в деку
    // завершается только внутри process(): шагаем его сами на каждой итерации.
    bool waitUntil(const std::function<bool()>& cond, int timeoutMs = 10000) {
        for (int waited = 0; waited < timeoutMs; waited += 10) {
            if (cond()) {
                return true;
            }
            m_pEngine->process(1024);
            QTest::qWait(10);
        }
        return cond();
    }

    std::shared_ptr<EffectsManager> m_pEffectsManager;
    std::shared_ptr<EngineMixer> m_pEngine;
    std::shared_ptr<SoundManager> m_pSoundManager;
    std::shared_ptr<mixxx::ControlIndicatorTimer> m_pControlIndicatorTimer;
    std::shared_ptr<PlayerManager> m_pPlayerManager;
    std::unique_ptr<TrackCollectionManager> m_pTrackCollectionManager;
    std::shared_ptr<RecordingManager> m_pRecordingManager;
    std::shared_ptr<Library> m_pLibrary;
    std::unique_ptr<mixxx::RemoteApiHandler> m_pHandler;
};

TEST_F(RemoteApiTest, StatusAndDecks) {
    auto status = call("GET", "/api/status");
    EXPECT_EQ(status->status, 200);
    EXPECT_EQ(bodyJson(*status).value("decks").toInt(), m_pPlayerManager->numberOfDecks());

    auto decks = call("GET", "/api/decks");
    EXPECT_EQ(decks->status, 200);
    EXPECT_EQ(bodyJson(*decks).value("decks").toArray().size(),
            m_pPlayerManager->numberOfDecks());

    auto deck1 = call("GET", "/api/decks/1");
    EXPECT_EQ(deck1->status, 200);
    const QJsonObject d = bodyJson(*deck1);
    EXPECT_EQ(d.value("group").toString(), QStringLiteral("[Channel1]"));
    EXPECT_FALSE(d.value("loaded").toBool(true));
}

TEST_F(RemoteApiTest, UnknownControlIs404) {
    EXPECT_EQ(call("GET", "/api/control", "group=%5BNope%5D&item=zzz")->status, 404);
    EXPECT_EQ(call("POST", "/api/control", "",
                      "{\"group\":\"[Nope]\",\"item\":\"zzz\",\"value\":1}")
                      ->status,
            404);
    // Обращение к несуществующему ключу не должно было его создать.
    EXPECT_FALSE(ControlObject::exists(ConfigKey(QStringLiteral("[Nope]"), QStringLiteral("zzz"))));
    EXPECT_EQ(call("GET", "/api/control", "group=%5BChannel1%5D")->status, 400);
}

TEST_F(RemoteApiTest, ControlRoundTrip) {
    auto set = call("POST", "/api/control", "",
            "{\"group\":\"[Channel1]\",\"item\":\"rate\",\"value\":0.25}");
    ASSERT_EQ(set->status, 200);
    EXPECT_DOUBLE_EQ(bodyJson(*set).value("value").toDouble(), 0.25);

    auto get = call("GET", "/api/control", "group=%5BChannel1%5D&item=rate");
    ASSERT_EQ(get->status, 200);
    EXPECT_DOUBLE_EQ(bodyJson(*get).value("value").toDouble(), 0.25);
    EXPECT_DOUBLE_EQ(ControlObject::get(ConfigKey(QStringLiteral("[Channel1]"),
                             QStringLiteral("rate"))),
            0.25);

    auto batch = call("POST", "/api/controls", "",
            "[{\"group\":\"[Channel1]\",\"item\":\"rate\",\"value\":0},"
            "{\"group\":\"[Nope]\",\"item\":\"x\",\"value\":1}]");
    ASSERT_EQ(batch->status, 200);
    const QJsonArray results = bodyJson(*batch).value("results").toArray();
    ASSERT_EQ(results.size(), 2);
    EXPECT_DOUBLE_EQ(results[0].toObject().value("value").toDouble(), 0.0);
    EXPECT_TRUE(results[1].toObject().contains("error"));
}

TEST_F(RemoteApiTest, BadRequests) {
    EXPECT_EQ(call("GET", "/nothing")->status, 404);
    EXPECT_EQ(call("GET", "/api/decks/99")->status, 404);
    EXPECT_EQ(call("POST", "/api/decks/1/nope")->status, 404);
    EXPECT_EQ(call("GET", "/api/decks/1/play")->status, 405);
    EXPECT_EQ(call("POST", "/api/decks/1/load", "", "{}")->status, 400);
    EXPECT_EQ(call("POST", "/api/decks/1/load", "", "{\"track_id\":987654321}")->status, 404);
}

TEST_F(RemoteApiTest, LoadPlayPauseEject) {
    const QString location = getTestDir().filePath(kTrackLocation);
    const QByteArray body = QJsonDocument(QJsonObject{{"path", location}}).toJson();
    ASSERT_EQ(call("POST", "/api/decks/1/load", "", body)->status, 202);

    Deck* pDeck = m_pPlayerManager->getDeck(0);
    ASSERT_NE(pDeck, nullptr);
    ASSERT_TRUE(waitUntil([&]() {
        return pDeck->getEngineDeck()->getEngineBuffer()->isTrackLoaded() &&
                pDeck->getLoadedTrack() != nullptr;
    })) << "трек не загрузился в деку";

    auto loaded = call("GET", "/api/decks/1");
    EXPECT_TRUE(bodyJson(*loaded).value("loaded").toBool());
    EXPECT_EQ(bodyJson(*loaded).value("track").toObject().value("location").toString(), location);

    EXPECT_TRUE(bodyJson(*call("POST", "/api/decks/1/play")).value("play").toBool());
    EXPECT_FALSE(bodyJson(*call("POST", "/api/decks/1/pause")).value("play").toBool());
    EXPECT_TRUE(bodyJson(*call("POST", "/api/decks/1/toggle")).value("play").toBool());

    // Eject сразу после загрузки Mixxx считает отменой загрузки и возвращает
    // трек обратно — выдерживаем ту же паузу, что и штатный тест.
    QTest::qWait(600);
    ASSERT_EQ(call("POST", "/api/decks/1/eject")->status, 200);
    EXPECT_TRUE(waitUntil([&]() { return pDeck->getLoadedTrack() == nullptr; }))
            << "eject не снял трек";
}

TEST_F(RemoteApiTest, LocalChannelServesStatus) {
    mixxx::RemoteApiSettings settings;
    settings.tcpEnabled = false;
    settings.localName = QStringLiteral("mixxx-dj-station-test-%1")
                                 .arg(QCoreApplication::applicationPid());
    mixxx::RemoteApiServer server(m_pHandler.get(), settings);
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.waitForListening(3000));

    QLocalSocket sock;
    sock.connectToServer(settings.localName);
    ASSERT_TRUE(sock.waitForConnected(3000)) << sock.errorString().toStdString();
    sock.write("GET /api/status HTTP/1.1\r\nHost: x\r\n\r\n");
    sock.flush();
    ASSERT_TRUE(waitUntil([&]() { return sock.bytesAvailable() > 0; }, 5000));
    QTest::qWait(50);
    const QByteArray resp = sock.readAll();
    EXPECT_TRUE(resp.startsWith("HTTP/1.1 200 OK\r\n")) << resp.left(80).toStdString();
    EXPECT_TRUE(resp.contains("\"decks\""));
    server.stop();
}

TEST_F(RemoteApiTest, TcpRequiresTokenWhenConfigured) {
    mixxx::RemoteApiSettings settings;
    settings.localEnabled = false;
    settings.bind = QStringLiteral("127.0.0.1");
    settings.port = 0; // свободный порт
    settings.token = QByteArrayLiteral("secret");
    mixxx::RemoteApiServer server(m_pHandler.get(), settings);
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.waitForListening(3000));
    ASSERT_GT(server.tcpPort(), 0);

    auto request = [&](const QByteArray& raw) {
        QTcpSocket sock;
        sock.connectToHost(QHostAddress::LocalHost, server.tcpPort());
        EXPECT_TRUE(sock.waitForConnected(3000));
        sock.write(raw);
        sock.flush();
        EXPECT_TRUE(waitUntil([&]() { return sock.bytesAvailable() > 0; }, 5000));
        QTest::qWait(50);
        return sock.readAll();
    };

    EXPECT_TRUE(request("GET /api/status HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 401"));
    EXPECT_TRUE(request("GET /api/status HTTP/1.1\r\nX-Mixxx-Token: wrong\r\n\r\n")
                        .startsWith("HTTP/1.1 401"));
    EXPECT_TRUE(request("GET /api/status HTTP/1.1\r\nX-Mixxx-Token: secret\r\n\r\n")
                        .startsWith("HTTP/1.1 200"));
    EXPECT_TRUE(request("GET /api/status?token=secret HTTP/1.1\r\n\r\n")
                        .startsWith("HTTP/1.1 200"));
    EXPECT_TRUE(request("BROKEN\r\n\r\n").startsWith("HTTP/1.1 400"));
    server.stop();
}

TEST_F(RemoteApiTest, LoopbackWithoutTokenIsAllowed) {
    mixxx::RemoteApiSettings settings;
    settings.localEnabled = false;
    settings.bind = QStringLiteral("127.0.0.1");
    settings.port = 0;
    mixxx::RemoteApiServer server(m_pHandler.get(), settings);
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.waitForListening(3000));

    QTcpSocket sock;
    sock.connectToHost(QHostAddress::LocalHost, server.tcpPort());
    ASSERT_TRUE(sock.waitForConnected(3000));
    sock.write("GET /api/decks/1 HTTP/1.1\r\n\r\n");
    sock.flush();
    ASSERT_TRUE(waitUntil([&]() { return sock.bytesAvailable() > 0; }, 5000));
    QTest::qWait(50);
    const QByteArray resp = sock.readAll();
    EXPECT_TRUE(resp.startsWith("HTTP/1.1 200")) << resp.left(80).toStdString();
    EXPECT_TRUE(resp.contains("[Channel1]"));
    server.stop();
}

// dj-station: разбор извне — BPM с якорем сетки, тональность, горячие метки.
// Так на станцию попадают данные VirtualDJ без пересчёта на планшете.
TEST_F(RemoteApiTest, LibraryAnalysisImport) {
    const QString location = getTestDir().filePath(kTrackLocation);
    // Частота дискретизации известна после загрузки — грузим один раз.
    const QByteArray loadBody = QJsonDocument(QJsonObject{{"path", location}}).toJson();
    ASSERT_EQ(call("POST", "/api/decks/1/load", "", loadBody)->status, 202);
    Deck* pDeck = m_pPlayerManager->getDeck(0);
    ASSERT_TRUE(waitUntil([&]() {
        return pDeck->getEngineDeck()->getEngineBuffer()->isTrackLoaded() &&
                pDeck->getLoadedTrack() != nullptr;
    }));

    QJsonObject body;
    body.insert("path", location);
    body.insert("bpm", 128.0);
    body.insert("beat_anchor_sec", 0.5);
    body.insert("key", "Am");
    body.insert("cues",
            QJsonArray{QJsonObject{{"num", 1}, {"pos_sec", 1.0}, {"name", "drop"}},
                    QJsonObject{{"num", 3}, {"pos_sec", 2.0}}});
    auto r = call("POST", "/api/library/analysis", "", QJsonDocument(body).toJson());
    ASSERT_EQ(r->status, 200) << r->body.toStdString();
    const QJsonObject res = bodyJson(*r);
    EXPECT_TRUE(res.value("beats_set").toBool());
    EXPECT_EQ(res.value("cues_written").toInt(), 2);

    TrackPointer pTrack = pDeck->getLoadedTrack();
    ASSERT_NE(pTrack, nullptr);
    EXPECT_NEAR(pTrack->getBpm(), 128.0, 0.05);
    EXPECT_FALSE(pTrack->getKeyText().isEmpty());
    int hotcues = 0;
    bool labelled = false;
    for (const CuePointer& pCue : pTrack->getCuePoints()) {
        if (pCue->getType() == mixxx::CueType::HotCue) {
            hotcues++;
            if (pCue->getLabel() == QStringLiteral("drop")) {
                labelled = true;
            }
        }
    }
    EXPECT_EQ(hotcues, 2);
    EXPECT_TRUE(labelled);

    // Повтор с теми же номерами не плодит дубли, а двигает метки.
    body.insert("cues", QJsonArray{QJsonObject{{"num", 1}, {"pos_sec", 1.5}}});
    ASSERT_EQ(call("POST", "/api/library/analysis", "", QJsonDocument(body).toJson())->status, 200);
    hotcues = 0;
    for (const CuePointer& pCue : pTrack->getCuePoints()) {
        if (pCue->getType() == mixxx::CueType::HotCue) {
            hotcues++;
        }
    }
    EXPECT_EQ(hotcues, 2);

    EXPECT_EQ(call("POST", "/api/library/analysis", "", "{\"track_id\":987654321,\"bpm\":120}")->status, 404);
    EXPECT_EQ(call("POST", "/api/library/analysis", "",
                      QJsonDocument(QJsonObject{{"path", location}, {"bpm", 9999.0}}).toJson())
                      ->status,
            400);
    EXPECT_EQ(call("GET", "/api/library/analysis")->status, 405);
}

TEST_F(RemoteApiTest, RefusesLanBindWithoutToken) {
    mixxx::RemoteApiSettings settings;
    settings.localEnabled = false;
    settings.bind = QStringLiteral("0.0.0.0");
    mixxx::RemoteApiServer server(m_pHandler.get(), settings);
    EXPECT_FALSE(server.start());
    EXPECT_FALSE(server.isListening());
}
