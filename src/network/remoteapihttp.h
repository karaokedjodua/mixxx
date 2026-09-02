#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QPair>

/// dj-station: разбор HTTP/1.1 и сборка ответов для сетевого управления
/// станцией. Сокетов здесь нет намеренно — всё, что тут лежит, проверяется
/// обычными юнит-тестами: склейка запроса из кусков TCP, регистр заголовков,
/// отсутствие Content-Length, превышение размера тела.
///
/// Почему свой разбор, а не готовая библиотека: в сборке есть только
/// QtNetwork (QTcpServer/QLocalServer). Qt6HttpServer и Qt6WebSockets в
/// поставке отсутствуют, а тянуть их ради десятка эндпоинтов незачем.
/// События отдаются как Server-Sent Events — их читает и браузер телефона,
/// и тестовый скрипт построчно.
namespace mixxx {
namespace remoteapi {

struct HttpRequest {
    QByteArray method;
    QByteArray path;
    QByteArray query;
    /// Ключи приведены к нижнему регистру.
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;

    QByteArray header(const QByteArray& name) const {
        return headers.value(name.toLower());
    }
};

enum class ParseStatus {
    NeedMore,
    Ok,
    Bad,
    TooLarge,
};

/// Накопительный разбор одного запроса. Кусок за куском подаётся в feed(),
/// пока не вернётся что-то кроме NeedMore. После Ok запрос лежит в request().
class HttpRequestParser {
  public:
    explicit HttpRequestParser(int maxBodyBytes = 64 * 1024);

    ParseStatus feed(const QByteArray& chunk);
    const HttpRequest& request() const {
        return m_request;
    }
    void reset();

  private:
    ParseStatus parseHead(const QByteArray& head);

    QByteArray m_buffer;
    HttpRequest m_request;
    bool m_headersDone;
    int m_contentLength;
    int m_maxBody;
};

/// Разбор строки запроса вида a=1&b=%D0%B0+x в пары; '+' считается пробелом.
QMap<QByteArray, QByteArray> parseQuery(const QByteArray& query);

/// Текст статуса для основных кодов; неизвестный код — "Unknown".
const char* statusText(int status);

/// Полный ответ с телом: строка статуса, заголовки, пустая строка, тело.
QByteArray buildResponse(int status,
        const QByteArray& contentType,
        const QByteArray& body,
        const QList<QPair<QByteArray, QByteArray>>& extraHeaders = {});

/// Заголовок потока событий: text/event-stream без Content-Length.
QByteArray sseHeaders();

/// Один кадр потока событий: "event: имя\ndata: данные\n\n".
/// Переводы строк в данных превращаются в отдельные строки data:.
QByteArray sseFrame(const QByteArray& event, const QByteArray& data);

} // namespace remoteapi
} // namespace mixxx
