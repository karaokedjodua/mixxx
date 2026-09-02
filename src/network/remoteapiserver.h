#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

#include <atomic>

class QIODevice;
class QLocalServer;
class QTcpServer;
class QTcpSocket;
class QThread;
class QTimer;

namespace mixxx {

class RemoteApiHandler;

namespace remoteapi {
struct HttpRequest;
}

struct RemoteApiSettings {
    /// Адрес TCP-слушателя. По умолчанию только localhost; для адреса в
    /// локальной сети обязателен токен, иначе сервер отказывается стартовать.
    QString bind{QStringLiteral("127.0.0.1")};
    quint16 port{8901};
    QByteArray token;
    /// Имя локального канала (именованный канал Windows / unix-сокет).
    /// Через него ходят автотесты на самой машине: ни токена, ни брандмауэра.
    QString localName{QStringLiteral("mixxx-dj-station")};
    int maxConnections{16};
    int idleTimeoutMs{30000};
    int maxBodyBytes{64 * 1024};
    int replyTimeoutMs{3000};
    bool tcpEnabled{true};
    bool localEnabled{true};
};

/// dj-station: сетевое управление станцией. Слушатели и сокеты живут в
/// своём потоке (по образцу ControllerManager): listen() и создание сокетов
/// происходят только внутри него, иначе readyRead не придёт. Каждый запрос
/// уходит обработчику в главный поток через очередь событий; ответа ждём с
/// таймаутом — иначе подтормаживающий GUI подвесил бы и сеть.
class RemoteApiServer : public QObject {
    Q_OBJECT
  public:
    RemoteApiServer(RemoteApiHandler* pHandler,
            RemoteApiSettings settings,
            QObject* parent = nullptr);
    ~RemoteApiServer() override;

    /// Запускает поток и слушателей. false — если настройки небезопасны
    /// (адрес не loopback без токена); тогда ничего не запущено.
    bool start();
    void stop();

    bool isListening() const {
        return m_listening.load();
    }
    /// Ждёт, пока слушатели поднимутся. Для тестов.
    bool waitForListening(int timeoutMs) const;
    quint16 tcpPort() const {
        return m_tcpPort.load();
    }
    QString localName() const {
        return m_settings.localName;
    }

  signals:
    void requestListen();

  private slots:
    void slotListen();
    void slotClose();
    void slotNewTcpConnection();
    void slotNewLocalConnection();
    void slotEvent(QByteArray name, QByteArray json);
    void slotHeartbeat();

  private:
    struct Connection;

    void adoptConnection(QIODevice* pDevice, bool local);
    void onReadyRead(Connection* pConn);
    void serve(Connection* pConn, const remoteapi::HttpRequest& request);
    void respond(Connection* pConn, int status, const QByteArray& body);
    void closeConnection(Connection* pConn);
    bool authorized(Connection* pConn, const remoteapi::HttpRequest& request) const;
    bool peerAllowed(QTcpSocket* pSocket) const;

    RemoteApiHandler* m_pHandler;
    RemoteApiSettings m_settings;
    QThread* m_pThread;
    QTcpServer* m_pTcpServer;
    QLocalServer* m_pLocalServer;
    QTimer* m_pHeartbeat;
    QHash<QIODevice*, Connection*> m_connections;
    std::atomic<bool> m_listening{false};
    std::atomic<quint16> m_tcpPort{0};
};

} // namespace mixxx
