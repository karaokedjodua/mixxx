#include "network/remoteapihttp.h"

#include <QList>

namespace mixxx {
namespace remoteapi {

namespace {

const QByteArray kHeadEnd = QByteArrayLiteral("\r\n\r\n");
const QByteArray kHeadEndLf = QByteArrayLiteral("\n\n");

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

QByteArray percentDecode(const QByteArray& in) {
    QByteArray out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); i++) {
        const char c = in[i];
        if (c == '+') {
            out.append(' ');
        } else if (c == '%' && i + 2 < in.size()) {
            const int hi = hexValue(in[i + 1]);
            const int lo = hexValue(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.append(static_cast<char>(hi * 16 + lo));
                i += 2;
            } else {
                out.append(c);
            }
        } else {
            out.append(c);
        }
    }
    return out;
}

} // anonymous namespace

HttpRequestParser::HttpRequestParser(int maxBodyBytes)
        : m_headersDone(false),
          m_contentLength(0),
          m_maxBody(maxBodyBytes) {
}

void HttpRequestParser::reset() {
    m_buffer.clear();
    m_request = HttpRequest();
    m_headersDone = false;
    m_contentLength = 0;
}

ParseStatus HttpRequestParser::parseHead(const QByteArray& head) {
    // Строки могут приходить и с CRLF, и с одним LF — принимаем оба.
    QList<QByteArray> lines = head.split('\n');
    for (QByteArray& line : lines) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
    }
    while (!lines.isEmpty() && lines.first().isEmpty()) {
        lines.removeFirst();
    }
    if (lines.isEmpty()) {
        return ParseStatus::Bad;
    }

    const QList<QByteArray> requestLine = lines.first().split(' ');
    if (requestLine.size() != 3 || !requestLine[2].startsWith("HTTP/1.")) {
        return ParseStatus::Bad;
    }
    m_request.method = requestLine[0].toUpper();
    const QByteArray target = requestLine[1];
    if (target.isEmpty() || target[0] != '/') {
        return ParseStatus::Bad;
    }
    const int q = target.indexOf('?');
    if (q >= 0) {
        m_request.path = target.left(q);
        m_request.query = target.mid(q + 1);
    } else {
        m_request.path = target;
        m_request.query.clear();
    }

    for (int i = 1; i < lines.size(); i++) {
        const QByteArray& line = lines[i];
        if (line.isEmpty()) {
            continue;
        }
        const int colon = line.indexOf(':');
        if (colon <= 0) {
            return ParseStatus::Bad;
        }
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        m_request.headers.insert(name, value);
    }

    const QByteArray lengthHeader = m_request.header("content-length");
    if (!lengthHeader.isEmpty()) {
        bool ok = false;
        const int length = lengthHeader.toInt(&ok);
        if (!ok || length < 0) {
            return ParseStatus::Bad;
        }
        if (length > m_maxBody) {
            return ParseStatus::TooLarge;
        }
        m_contentLength = length;
    } else {
        m_contentLength = 0;
    }
    return ParseStatus::Ok;
}

ParseStatus HttpRequestParser::feed(const QByteArray& chunk) {
    m_buffer.append(chunk);

    if (!m_headersDone) {
        // Заголовки без тела не должны расти бесконечно.
        if (m_buffer.size() > m_maxBody + 8 * 1024 && m_buffer.indexOf(kHeadEnd) < 0) {
            return ParseStatus::TooLarge;
        }
        int headEnd = m_buffer.indexOf(kHeadEnd);
        int sepLen = kHeadEnd.size();
        if (headEnd < 0) {
            headEnd = m_buffer.indexOf(kHeadEndLf);
            sepLen = kHeadEndLf.size();
        }
        if (headEnd < 0) {
            return ParseStatus::NeedMore;
        }
        const ParseStatus status = parseHead(m_buffer.left(headEnd));
        if (status != ParseStatus::Ok) {
            return status;
        }
        m_headersDone = true;
        m_buffer.remove(0, headEnd + sepLen);
    }

    if (m_buffer.size() < m_contentLength) {
        return ParseStatus::NeedMore;
    }
    m_request.body = m_buffer.left(m_contentLength);
    m_buffer.remove(0, m_contentLength);
    return ParseStatus::Ok;
}

QMap<QByteArray, QByteArray> parseQuery(const QByteArray& query) {
    QMap<QByteArray, QByteArray> result;
    if (query.isEmpty()) {
        return result;
    }
    const QList<QByteArray> parts = query.split('&');
    for (const QByteArray& part : parts) {
        if (part.isEmpty()) {
            continue;
        }
        const int eq = part.indexOf('=');
        if (eq < 0) {
            result.insert(percentDecode(part), QByteArray());
        } else {
            result.insert(percentDecode(part.left(eq)), percentDecode(part.mid(eq + 1)));
        }
    }
    return result;
}

const char* statusText(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "Unknown";
    }
}

QByteArray buildResponse(int status,
        const QByteArray& contentType,
        const QByteArray& body,
        const QList<QPair<QByteArray, QByteArray>>& extraHeaders) {
    QByteArray out;
    out.append("HTTP/1.1 ");
    out.append(QByteArray::number(status));
    out.append(' ');
    out.append(statusText(status));
    out.append("\r\n");
    if (!contentType.isEmpty()) {
        out.append("Content-Type: ");
        out.append(contentType);
        out.append("\r\n");
    }
    out.append("Content-Length: ");
    out.append(QByteArray::number(body.size()));
    out.append("\r\n");
    out.append("Cache-Control: no-store\r\n");
    out.append("Access-Control-Allow-Origin: *\r\n");
    out.append("Connection: close\r\n");
    for (const auto& header : extraHeaders) {
        out.append(header.first);
        out.append(": ");
        out.append(header.second);
        out.append("\r\n");
    }
    out.append("\r\n");
    out.append(body);
    return out;
}

QByteArray sseHeaders() {
    return QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: keep-alive\r\n"
            "\r\n");
}

QByteArray sseFrame(const QByteArray& event, const QByteArray& data) {
    QByteArray out;
    if (!event.isEmpty()) {
        out.append("event: ");
        out.append(event);
        out.append('\n');
    }
    const QList<QByteArray> lines = data.split('\n');
    for (const QByteArray& line : lines) {
        out.append("data: ");
        out.append(line);
        out.append('\n');
    }
    out.append('\n');
    return out;
}

} // namespace remoteapi
} // namespace mixxx
