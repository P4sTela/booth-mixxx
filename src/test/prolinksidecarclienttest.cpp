#include <gtest/gtest.h>

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>

#include <future>
#include <memory>
#include <thread>

#include "library/prolink/sidecarclient.h"

namespace {

const QString kProtocol = QStringLiteral("pdl-sidecar/0");

QJsonObject makeTrack(int id, const QString& title, const QString& path, int bpm) {
    QJsonObject track;
    track.insert(QStringLiteral("id"), id);
    track.insert(QStringLiteral("title"), title);
    track.insert(QStringLiteral("artist"), QStringLiteral("Artist"));
    track.insert(QStringLiteral("album"), QStringLiteral("Album"));
    track.insert(QStringLiteral("genre"), QStringLiteral("House"));
    track.insert(QStringLiteral("key"), QStringLiteral("8A"));
    track.insert(QStringLiteral("path"), path);
    track.insert(QStringLiteral("date_added"), QStringLiteral("2026-01-02"));
    track.insert(QStringLiteral("bpm"), bpm);
    track.insert(QStringLiteral("duration_ms"), 192000);
    track.insert(QStringLiteral("bitrate"), 1411);
    return track;
}

void runFakeSidecar(const QString& path, bool failTracks, std::promise<bool>* pReady) {
    QLocalServer server;
    QLocalServer::removeServer(path);
    pReady->set_value(server.listen(path));
    if (!server.waitForNewConnection(5000)) {
        return;
    }
    std::unique_ptr<QLocalSocket> pSocket(server.nextPendingConnection());
    QByteArray buffer;
    while (true) {
        while (!buffer.contains('\n')) {
            if (!pSocket->waitForReadyRead(5000)) {
                return;
            }
            buffer.append(pSocket->readAll());
        }
        const int end = buffer.indexOf('\n');
        const QJsonObject request =
                QJsonDocument::fromJson(buffer.left(end)).object();
        buffer.remove(0, end + 1);

        const QString method = request.value(QStringLiteral("method")).toString();
        QJsonObject response;
        response.insert(QStringLiteral("id"),
                request.value(QStringLiteral("id")));
        if (method == QLatin1String("hello")) {
            QJsonObject result;
            result.insert(QStringLiteral("protocol"), kProtocol);
            result.insert(QStringLiteral("sidecar"), QStringLiteral("test"));
            result.insert(QStringLiteral("capabilities"),
                    QJsonArray{QStringLiteral("library")});
            response.insert(QStringLiteral("result"), result);
        } else if (method == QLatin1String("library.open")) {
            QJsonObject result;
            result.insert(QStringLiteral("tracks"), 3);
            result.insert(QStringLiteral("playlists"), 1);
            response.insert(QStringLiteral("result"), result);
        } else if (method == QLatin1String("library.tracks")) {
            if (failTracks) {
                QJsonObject error;
                error.insert(QStringLiteral("code"), QStringLiteral("library.locked"));
                error.insert(QStringLiteral("message"), QStringLiteral("busy"));
                response.insert(QStringLiteral("error"), error);
            } else {
                const QJsonObject params =
                        request.value(QStringLiteral("params")).toObject();
                const int offset = params.value(QStringLiteral("offset")).toInt();
                QJsonArray tracks;
                if (offset == 0) {
                    tracks.append(makeTrack(1,
                            QStringLiteral("One"),
                            QStringLiteral("/music/one.flac"),
                            12800));
                    tracks.append(makeTrack(2,
                            QStringLiteral("Two"),
                            QStringLiteral("/music/two.mp3"),
                            13000));
                } else {
                    tracks.append(makeTrack(3,
                            QStringLiteral("Three"),
                            QStringLiteral("/music/three.aiff"),
                            12400));
                }
                QJsonObject result;
                result.insert(QStringLiteral("total"), 3);
                result.insert(QStringLiteral("tracks"), tracks);
                response.insert(QStringLiteral("result"), result);
            }
        } else {
            QJsonObject error;
            error.insert(QStringLiteral("code"), QStringLiteral("unknown_method"));
            error.insert(QStringLiteral("message"), method);
            response.insert(QStringLiteral("error"), error);
        }

        QByteArray payload =
                QJsonDocument(response).toJson(QJsonDocument::Compact);
        payload.append('\n');
        pSocket->write(payload);
        pSocket->flush();
    }
}

class FakeSidecarThread {
  public:
    FakeSidecarThread(const QString& path, bool failTracks)
            : m_thread(runFakeSidecar, path, failTracks, &m_ready) {
    }
    ~FakeSidecarThread() {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
    bool waitUntilListening() {
        return m_ready.get_future().get();
    }

  private:
    std::promise<bool> m_ready;
    std::thread m_thread;
};

TEST(ProLinkSidecarClientTest, ReadsEveryPageAndScalesBpm) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString socketPath = dir.filePath(QStringLiteral("sidecar.sock"));
    FakeSidecarThread server(socketPath, false);
    ASSERT_TRUE(server.waitUntilListening());

    ProLinkSidecarClient client(socketPath);
    QString error;
    ASSERT_TRUE(client.connectToSidecar(&error)) << error.toStdString();
    ASSERT_TRUE(client.openLibrary(&error)) << error.toStdString();

    QVector<ProLinkTrack> tracks;
    int total = 0;
    ASSERT_TRUE(client.fetchTracks(2, &tracks, &total, &error))
            << error.toStdString();
    ASSERT_EQ(3, total);
    ASSERT_EQ(3, tracks.size());
    EXPECT_EQ(QStringLiteral("One"), tracks.at(0).title);
    EXPECT_DOUBLE_EQ(128.0, tracks.at(0).bpm);
    EXPECT_EQ(192000, tracks.at(0).durationMs);
    EXPECT_EQ(QStringLiteral("/music/one.flac"), tracks.at(0).location);
    EXPECT_EQ(QStringLiteral("Three"), tracks.at(2).title);
}

TEST(ProLinkSidecarClientTest, ReportsSidecarErrors) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString socketPath = dir.filePath(QStringLiteral("sidecar.sock"));
    FakeSidecarThread server(socketPath, true);
    ASSERT_TRUE(server.waitUntilListening());

    ProLinkSidecarClient client(socketPath);
    QString error;
    ASSERT_TRUE(client.connectToSidecar(&error)) << error.toStdString();
    ASSERT_TRUE(client.openLibrary(&error)) << error.toStdString();

    QVector<ProLinkTrack> tracks;
    int total = 0;
    EXPECT_FALSE(client.fetchTracks(2, &tracks, &total, &error));
    EXPECT_TRUE(error.startsWith(QStringLiteral("library.locked")))
            << error.toStdString();
}

} // namespace
