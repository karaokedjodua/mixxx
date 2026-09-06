#include "network/remoteapiserver.h"

#include "moc_remoteapiserver.cpp"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaObject>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <memory>

#include "network/remoteapihandler.h"
#include "network/remoteapihttp.h"
#include "util/logger.h"

namespace mixxx {

namespace {

const Logger kLogger("RemoteApiServer");
constexpr int kHeartbeatMs = 15000;

bool constantTimeEquals(const QByteArray& a, const QByteArray& b) {
    const int n = std::max(a.size(), b.size());
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    for (int i = 0; i < n; i++) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

bool isPrivateOrLoopback(const QHostAddress& addressIn) {
    QHostAddress address = addressIn;
    if (address.isLoopback()) {
        return true;
    }
    bool isV4 = false;
    const quint32 v4 = address.toIPv4Address(&isV4);
    if (isV4) {
        // 10/8, 172.16/12, 192.168/16, 169.254/16
        return (v4 & 0xFF000000u) == 0x0A000000u ||
                (v4 & 0xFFF00000u) == 0xAC100000u ||
                (v4 & 0xFFFF0000u) == 0xC0A80000u ||
                (v4 & 0xFFFF0000u) == 0xA9FE0000u;
    }
    const Q_IPV6ADDR v6 = address.toIPv6Address();
    // fc00::/7 (уникальные локальные) и fe80::/10 (канальные)
    return (v6[0] & 0xFE) == 0xFC || (v6[0] == 0xFE && (v6[1] & 0xC0) == 0x80);
}

void flushDevice(QIODevice* pDevice) {
    if (auto* pTcp = qobject_cast<QTcpSocket*>(pDevice)) {
        pTcp->flush();
    } else if (auto* pLocal = qobject_cast<QLocalSocket*>(pDevice)) {
        pLocal->flush();
    }
}

} // anonymous namespace

struct RemoteApiServer::Connection {
    Connection(QIODevice* pDevice, bool local, int maxBody)
            : pDevice(pDevice),
              local(local),
              parser(maxBody),
              sse(false),
              all(false),
              pIdle(nullptr) {
    }
    QIODevice* pDevice;
    bool local;
    remoteapi::HttpRequestParser parser;
    bool sse;
    bool all;
    QSet<QString> filter;
    QTimer* pIdle;
};

RemoteApiServer::RemoteApiServer(RemoteApiHandler* pHandler,
        RemoteApiSettings settings,
        QObject* parent)
        : QObject(parent),
          m_pHandler(pHandler),
          m_settings(std::move(settings)),
          m_pThread(new QThread),
          m_pTcpServer(nullptr),
          m_pLocalServer(nullptr),
          m_pHeartbeat(nullptr) {
    m_pThread->setObjectName(QStringLiteral("RemoteApi"));
    moveToThread(m_pThread);
    connect(this, &RemoteApiServer::requestListen, this, &RemoteApiServer::slotListen);
    // Событие рождается в главном потоке, а сокеты живут в нашем — соединение
    // само станет очередным.
    connect(m_pHandler, &RemoteApiHandler::event, this, &RemoteApiServer::slotEvent);
    m_pThread->start();
}

RemoteApiServer::~RemoteApiServer() {
    stop();
    delete m_pThread;
}

bool RemoteApiServer::start() {
    if (m_settings.tcpEnabled) {
        const QHostAddress bind(m_settings.bind);
        if (bind.isNull()) {
            kLogger.warning() << "invalid bind address" << m_settings.bind;
            return false;
        }
        if (!bind.isLoopback() && m_settings.token.isEmpty()) {
            kLogger.warning() << "refusing to listen on" << m_settings.bind
                              << "without a token: set [RemoteApi] Token";
            return false;
        }
    }
    emit requestListen();
    return true;
}

bool RemoteApiServer::waitForListening(int timeoutMs) const {
    for (int waited = 0; waited < timeoutMs; waited += 10) {
        if (m_listening.load()) {
            return true;
        }
        QThread::msleep(10);
    }
    return m_listening.load();
}

void RemoteApiServer::stop() {
    if (!m_pThread->isRunning()) {
        return;
    }
    if (QThread::currentThread() == m_pThread) {
        slotClose();
    } else {
        QMetaObject::invokeMethod(this, "slotClose", Qt::BlockingQueuedConnection);
    }
    m_pThread->quit();
    m_pThread->wait(3000);
}

void RemoteApiServer::slotListen() {
    bool anyListening = false;

    if (m_settings.localEnabled && !m_settings.localName.isEmpty()) {
        // Имя могло остаться от упавшего экземпляра.
        QLocalServer::removeServer(m_settings.localName);
        m_pLocalServer = new QLocalServer(this);
        if (m_pLocalServer->listen(m_settings.localName)) {
            connect(m_pLocalServer,
                    &QLocalServer::newConnection,
                    this,
                    &RemoteApiServer::slotNewLocalConnection);
            anyListening = true;
            kLogger.info() << "listening on local channel" << m_settings.localName;
        } else {
            kLogger.warning() << "local channel failed:" << m_pLocalServer->errorString();
            delete m_pLocalServer;
            m_pLocalServer = nullptr;
        }
    }

    if (m_settings.tcpEnabled) {
        m_pTcpServer = new QTcpServer(this);
        if (m_pTcpServer->listen(QHostAddress(m_settings.bind), m_settings.port)) {
            connect(m_pTcpServer,
                    &QTcpServer::newConnection,
                    this,
                    &RemoteApiServer::slotNewTcpConnection);
            m_tcpPort.store(m_pTcpServer->serverPort());
            anyListening = true;
            kLogger.info() << "listening on" << m_settings.bind << m_pTcpServer->serverPort();
        } else {
            kLogger.warning() << "tcp listen failed:" << m_pTcpServer->errorString();
            delete m_pTcpServer;
            m_pTcpServer = nullptr;
        }
    }

    m_pHeartbeat = new QTimer(this);
    m_pHeartbeat->setInterval(kHeartbeatMs);
    connect(m_pHeartbeat, &QTimer::timeout, this, &RemoteApiServer::slotHeartbeat);
    m_pHeartbeat->start();

    m_listening.store(anyListening);
}

void RemoteApiServer::slotClose() {
    m_listening.store(false);
    const QList<Connection*> conns = m_connections.values();
    for (Connection* pConn : conns) {
        closeConnection(pConn);
    }
    if (m_pHeartbeat) {
        m_pHeartbeat->stop();
        delete m_pHeartbeat;
        m_pHeartbeat = nullptr;
    }
    if (m_pTcpServer) {
        m_pTcpServer->close();
        delete m_pTcpServer;
        m_pTcpServer = nullptr;
    }
    if (m_pLocalServer) {
        m_pLocalServer->close();
        delete m_pLocalServer;
        m_pLocalServer = nullptr;
    }
}

bool RemoteApiServer::peerAllowed(QTcpSocket* pSocket) const {
    return isPrivateOrLoopback(pSocket->peerAddress());
}

void RemoteApiServer::slotNewTcpConnection() {
    while (m_pTcpServer && m_pTcpServer->hasPendingConnections()) {
        QTcpSocket* pSocket = m_pTcpServer->nextPendingConnection();
        if (!pSocket) {
            break;
        }
        if (!peerAllowed(pSocket)) {
            kLogger.warning() << "rejected peer" << pSocket->peerAddress().toString();
            pSocket->close();
            pSocket->deleteLater();
            continue;
        }
        if (m_connections.size() >= m_settings.maxConnections) {
            pSocket->write(remoteapi::buildResponse(503,
                    "application/json",
                    "{\"error\":\"too many connections\"}"));
            pSocket->flush();
            pSocket->close();
            pSocket->deleteLater();
            continue;
        }
        adoptConnection(pSocket, false);
    }
}

void RemoteApiServer::slotNewLocalConnection() {
    while (m_pLocalServer && m_pLocalServer->hasPendingConnections()) {
        QLocalSocket* pSocket = m_pLocalServer->nextPendingConnection();
        if (!pSocket) {
            break;
        }
        if (m_connections.size() >= m_settings.maxConnections) {
            pSocket->write(remoteapi::buildResponse(503,
                    "application/json",
                    "{\"error\":\"too many connections\"}"));
            pSocket->flush();
            pSocket->close();
            pSocket->deleteLater();
            continue;
        }
        adoptConnection(pSocket, true);
    }
}

void RemoteApiServer::adoptConnection(QIODevice* pDevice, bool local) {
    auto* pConn = new Connection(pDevice, local, m_settings.maxBodyBytes);
    pConn->pIdle = new QTimer(this);
    pConn->pIdle->setSingleShot(true);
    pConn->pIdle->setInterval(m_settings.idleTimeoutMs);
    connect(pConn->pIdle, &QTimer::timeout, this, [this, pConn]() {
        if (m_connections.value(pConn->pDevice) == pConn) {
            closeConnection(pConn);
        }
    });
    pConn->pIdle->start();
    m_connections.insert(pDevice, pConn);

    connect(pDevice, &QIODevice::readyRead, this, [this, pConn]() {
        if (m_connections.value(pConn->pDevice) == pConn) {
            onReadyRead(pConn);
        }
    });
    auto onGone = [this, pConn]() {
        if (m_connections.value(pConn->pDevice) == pConn) {
            closeConnection(pConn);
        }
    };
    if (auto* pTcp = qobject_cast<QTcpSocket*>(pDevice)) {
        connect(pTcp, &QTcpSocket::disconnected, this, onGone);
    } else if (auto* pLocal = qobject_cast<QLocalSocket*>(pDevice)) {
        connect(pLocal, &QLocalSocket::disconnected, this, onGone);
    }
}

void RemoteApiServer::closeConnection(Connection* pConn) {
    m_connections.remove(pConn->pDevice);
    if (pConn->pIdle) {
        pConn->pIdle->stop();
        pConn->pIdle->deleteLater();
    }
    QIODevice* pDevice = pConn->pDevice;
    pDevice->disconnect(this);
    flushDevice(pDevice);
    pDevice->close();
    pDevice->deleteLater();
    delete pConn;
}

void RemoteApiServer::respond(Connection* pConn, int status, const QByteArray& body) {
    pConn->pDevice->write(remoteapi::buildResponse(status, "application/json", body));
    flushDevice(pConn->pDevice);
}

bool RemoteApiServer::authorized(Connection* pConn, const remoteapi::HttpRequest& request) const {
    if (pConn->local) {
        return true;
    }
    if (m_settings.token.isEmpty()) {
        auto* pTcp = qobject_cast<QTcpSocket*>(pConn->pDevice);
        return pTcp && pTcp->peerAddress().isLoopback();
    }
    QByteArray presented = request.header("x-mixxx-token");
    if (presented.isEmpty()) {
        // Запасной путь только для EventSource: у него нельзя задать заголовок.
        presented = remoteapi::parseQuery(request.query).value("token");
    }
    return constantTimeEquals(presented, m_settings.token);
}

void RemoteApiServer::onReadyRead(Connection* pConn) {
    const QByteArray chunk = pConn->pDevice->readAll();
    if (pConn->sse) {
        return; // поток событий входящих данных не ждёт
    }
    const remoteapi::ParseStatus status = pConn->parser.feed(chunk);
    switch (status) {
    case remoteapi::ParseStatus::NeedMore:
        pConn->pIdle->start();
        return;
    case remoteapi::ParseStatus::Bad:
        respond(pConn, 400, "{\"error\":\"bad request\"}");
        closeConnection(pConn);
        return;
    case remoteapi::ParseStatus::TooLarge:
        respond(pConn, 413, "{\"error\":\"body too large\"}");
        closeConnection(pConn);
        return;
    case remoteapi::ParseStatus::Ok:
        break;
    }
    serve(pConn, pConn->parser.request());
}

void RemoteApiServer::serve(Connection* pConn, const remoteapi::HttpRequest& request) {
    if (!authorized(pConn, request)) {
        respond(pConn, 401, "{\"error\":\"unauthorized\"}");
        closeConnection(pConn);
        return;
    }

    if (request.path == "/api/events") {
        const QByteArray subscribe = remoteapi::parseQuery(request.query).value("subscribe");
        QStringList keys;
        for (const QByteArray& k : subscribe.split(';')) {
            if (!k.trimmed().isEmpty()) {
                keys.append(QString::fromUtf8(k.trimmed()));
            }
        }
        RemoteApiHandler* pHandler = m_pHandler;
        QMetaObject::invokeMethod(
                pHandler,
                [pHandler, keys]() { pHandler->subscribeNow(keys); },
                Qt::QueuedConnection);
        pConn->sse = true;
        pConn->all = keys.isEmpty();
        pConn->filter = QSet<QString>(keys.cbegin(), keys.cend());
        pConn->pIdle->stop();
        pConn->pDevice->write(remoteapi::sseHeaders());
        pConn->pDevice->write(": ok\n\n");
        flushDevice(pConn->pDevice);
        return;
    }

    auto reply = std::make_shared<RemoteApiReply>();
    RemoteApiHandler* pHandler = m_pHandler;
    const QByteArray method = request.method;
    const QByteArray path = request.path;
    const QByteArray query = request.query;
    const QByteArray body = request.body;
    QMetaObject::invokeMethod(
            pHandler,
            [pHandler, method, path, query, body, reply]() {
                if (!reply->abandoned.load()) {
                    pHandler->handleNow(method, path, query, body, reply.get());
                }
                reply->done.release();
            },
            Qt::QueuedConnection);

    // devices/rescan?force=1 переоткрывает звуковые карты в главном потоке:
    // ASIO-инициализация на планшете станции занимает секунды. Общий таймаут
    // ответа (3 с) превращал УСПЕШНОЕ переоткрытие в «main thread busy», и
    // вызывающий принимал успех за провал. Для этого пути ждём дольше;
    // каждый запрос живёт в потоке своего соединения, так что остальные
    // клиенты не задерживаются.
    const int waitMs = path.startsWith("/api/devices/rescan")
            ? std::max(m_settings.replyTimeoutMs, 45000)
            : m_settings.replyTimeoutMs;
    if (!reply->done.tryAcquire(1, waitMs)) {
        // Главный поток занят. Бросаем ответ — он допишет его в буфер,
        // который никто больше не читает.
        reply->abandoned.store(true);
        respond(pConn, 504, "{\"error\":\"main thread busy\"}");
    } else {
        respond(pConn, reply->status, reply->body);
    }
    closeConnection(pConn);
}

void RemoteApiServer::slotEvent(QByteArray name, QByteArray json) {
    if (m_connections.isEmpty()) {
        return;
    }
    QSet<QString> changed;
    if (name == "control") {
        const QJsonObject o = QJsonDocument::fromJson(json).object();
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
            changed.insert(it.key());
        }
    }
    const QByteArray frame = remoteapi::sseFrame(name, json);
    const QList<Connection*> conns = m_connections.values();
    for (Connection* pConn : conns) {
        if (!pConn->sse) {
            continue;
        }
        bool wanted = pConn->all || name == "track";
        if (!wanted) {
            for (const QString& key : changed) {
                if (pConn->filter.contains(key)) {
                    wanted = true;
                    break;
                }
            }
        }
        if (wanted) {
            pConn->pDevice->write(frame);
            flushDevice(pConn->pDevice);
        }
    }
}

void RemoteApiServer::slotHeartbeat() {
    const QList<Connection*> conns = m_connections.values();
    for (Connection* pConn : conns) {
        if (pConn->sse) {
            pConn->pDevice->write(": ping\n\n");
            flushDevice(pConn->pDevice);
        }
    }
}

} // namespace mixxx
