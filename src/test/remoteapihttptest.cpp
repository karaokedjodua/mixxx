#include <gtest/gtest.h>

#include <QByteArray>

#include "network/remoteapihttp.h"

// dj-station: разбор HTTP для сетевого управления. Сокетов нет — только
// склейка байтов в запрос и обратно. Главное, что тут ловится: запрос,
// пришедший двумя кусками TCP с разрывом посреди заголовка или тела.

using namespace mixxx::remoteapi;

namespace {

TEST(RemoteApiHttp, ParsesSimpleGet) {
    HttpRequestParser parser;
    const auto status = parser.feed(
            "GET /api/decks/1?fields=bpm HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "X-Mixxx-Token: abc\r\n"
            "\r\n");
    ASSERT_EQ(status, ParseStatus::Ok);
    const HttpRequest& r = parser.request();
    EXPECT_EQ(r.method, QByteArray("GET"));
    EXPECT_EQ(r.path, QByteArray("/api/decks/1"));
    EXPECT_EQ(r.query, QByteArray("fields=bpm"));
    EXPECT_EQ(r.header("x-mixxx-token"), QByteArray("abc"));
    EXPECT_EQ(r.header("X-MIXXX-TOKEN"), QByteArray("abc"))
            << "имя заголовка обязано читаться без учёта регистра";
    EXPECT_TRUE(r.body.isEmpty());
}

TEST(RemoteApiHttp, PostSplitAcrossChunksMidHeaderAndMidBody) {
    HttpRequestParser parser;
    EXPECT_EQ(parser.feed("POST /api/control HTTP/1.1\r\nContent-Le"),
            ParseStatus::NeedMore);
    EXPECT_EQ(parser.feed("ngth: 28\r\nContent-Type: application/json\r\n\r\n{\"group\":\"[Cha"),
            ParseStatus::NeedMore);
    EXPECT_EQ(parser.feed("nnel1]\",\"v\":1}"), ParseStatus::Ok);
    const HttpRequest& r = parser.request();
    EXPECT_EQ(r.method, QByteArray("POST"));
    EXPECT_EQ(r.path, QByteArray("/api/control"));
    EXPECT_EQ(r.body, QByteArray("{\"group\":\"[Channel1]\",\"v\":1}"));
    EXPECT_EQ(r.body.size(), 28);
}

TEST(RemoteApiHttp, MissingContentLengthMeansEmptyBody) {
    HttpRequestParser parser;
    EXPECT_EQ(parser.feed("POST /api/decks/1/play HTTP/1.1\r\n\r\n"), ParseStatus::Ok);
    EXPECT_TRUE(parser.request().body.isEmpty());
}

TEST(RemoteApiHttp, LfOnlyLineEndingsAccepted) {
    HttpRequestParser parser;
    EXPECT_EQ(parser.feed("GET /api/status HTTP/1.1\nHost: x\n\n"), ParseStatus::Ok);
    EXPECT_EQ(parser.request().path, QByteArray("/api/status"));
}

TEST(RemoteApiHttp, BodyOverLimitIsRejected) {
    HttpRequestParser parser(16);
    EXPECT_EQ(parser.feed("POST /api/control HTTP/1.1\r\nContent-Length: 17\r\n\r\n"),
            ParseStatus::TooLarge);
}

TEST(RemoteApiHttp, GarbageRequestLineIsBad) {
    HttpRequestParser parser;
    EXPECT_EQ(parser.feed("HELLO\r\n\r\n"), ParseStatus::Bad);
    parser.reset();
    EXPECT_EQ(parser.feed("GET nopath HTTP/1.1\r\n\r\n"), ParseStatus::Bad);
    parser.reset();
    EXPECT_EQ(parser.feed("GET / SMTP/1.0\r\n\r\n"), ParseStatus::Bad);
}

TEST(RemoteApiHttp, ResetClearsState) {
    HttpRequestParser parser;
    ASSERT_EQ(parser.feed("GET /a HTTP/1.1\r\n\r\n"), ParseStatus::Ok);
    parser.reset();
    ASSERT_EQ(parser.feed("GET /b HTTP/1.1\r\n\r\n"), ParseStatus::Ok);
    EXPECT_EQ(parser.request().path, QByteArray("/b"));
}

TEST(RemoteApiHttp, QueryParsingDecodesPercentAndPlus) {
    const auto q = parseQuery("group=%5BChannel1%5D&item=play&note=a+b%20c&flag");
    EXPECT_EQ(q.value("group"), QByteArray("[Channel1]"));
    EXPECT_EQ(q.value("item"), QByteArray("play"));
    EXPECT_EQ(q.value("note"), QByteArray("a b c"));
    EXPECT_TRUE(q.contains("flag"));
    EXPECT_TRUE(q.value("flag").isEmpty());
    EXPECT_TRUE(parseQuery("").isEmpty());
}

TEST(RemoteApiHttp, ResponseCarriesStatusLengthAndBody) {
    const QByteArray resp = buildResponse(404, "application/json", "{\"error\":\"no\"}");
    EXPECT_TRUE(resp.startsWith("HTTP/1.1 404 Not Found\r\n"));
    EXPECT_TRUE(resp.contains("Content-Type: application/json\r\n"));
    EXPECT_TRUE(resp.contains("Content-Length: 14\r\n"));
    EXPECT_TRUE(resp.endsWith("\r\n\r\n{\"error\":\"no\"}"));
    EXPECT_STREQ(statusText(999), "Unknown");
}

TEST(RemoteApiHttp, SseHeadersAndFrames) {
    const QByteArray head = sseHeaders();
    EXPECT_TRUE(head.startsWith("HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(head.contains("Content-Type: text/event-stream\r\n"));
    EXPECT_FALSE(head.contains("Content-Length"))
            << "у потока событий не бывает длины";
    EXPECT_TRUE(head.endsWith("\r\n\r\n"));

    EXPECT_EQ(sseFrame("track", "{\"deck\":1}"),
            QByteArray("event: track\ndata: {\"deck\":1}\n\n"));
    EXPECT_EQ(sseFrame("", "a\nb"), QByteArray("data: a\ndata: b\n\n"))
            << "перевод строки в данных должен стать второй строкой data:";
}

} // namespace
