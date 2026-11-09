#include "library/prolink/sidecarclient.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>

#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("ProLinkSidecarClient");

constexpr int kConnectTimeoutMs = 10000;
constexpr int kResponseTimeoutMs = 30000;
constexpr int kMaxResponseBytes = 16 * 1024 * 1024;
const QString kProtocol = QStringLiteral("pdl-sidecar/0");

} // namespace

ProLinkSidecarClient::ProLinkSidecarClient(QString socketPath)
        : m_socketPath(std::move(socketPath)),
          m_pSocket(new QLocalSocket()) {
}

ProLinkSidecarClient::~ProLinkSidecarClient() {
    m_pSocket->disconnectFromServer();
    delete m_pSocket;
}

QString ProLinkSidecarClient::defaultSocketPath() {
    const QString fromEnvironment = qEnvironmentVariable("PDL_SIDECAR_SOCKET");
    if (!fromEnvironment.isEmpty()) {
        return fromEnvironment;
    }
    return QDir(QDir::tempPath()).filePath(QStringLiteral("pdl-sidecar.sock"));
}

bool ProLinkSidecarClient::connectToSidecar(QString* pError) {
    if (m_pSocket->state() == QLocalSocket::ConnectedState) {
        return true;
    }
    m_pSocket->connectToServer(m_socketPath);
    if (!m_pSocket->waitForConnected(kConnectTimeoutMs)) {
        *pError = QStringLiteral("cannot reach the PRO DJ LINK sidecar on %1: %2")
                          .arg(m_socketPath, m_pSocket->errorString());
        return false;
    }

    QJsonObject params;
    params.insert(QStringLiteral("protocol"), kProtocol);
    QJsonObject result;
    if (!call(QStringLiteral("hello"), params, &result, pError)) {
        m_pSocket->disconnectFromServer();
        return false;
    }
    const QString protocol = result.value(QStringLiteral("protocol")).toString();
    if (protocol != kProtocol) {
        *pError = QStringLiteral("the sidecar speaks %1, not %2")
                          .arg(protocol, kProtocol);
        m_pSocket->disconnectFromServer();
        return false;
    }
    kLogger.info() << "connected to the sidecar on" << m_socketPath;
    return true;
}

bool ProLinkSidecarClient::openLibrary(QString* pError) {
    QJsonObject result;
    return call(QStringLiteral("library.open"), QJsonObject(), &result, pError);
}

bool ProLinkSidecarClient::fetchPlaylists(
        QVector<ProLinkPlaylist>* pPlaylists,
        QString* pError) {
    pPlaylists->clear();
    QJsonObject result;
    if (!call(QStringLiteral("library.playlists"), QJsonObject(), &result, pError)) {
        return false;
    }
    const QJsonArray playlists =
            result.value(QStringLiteral("playlists")).toArray();
    for (const auto& value : playlists) {
        const QJsonObject object = value.toObject();
        ProLinkPlaylist playlist;
        playlist.id = object.value(QStringLiteral("id")).toInteger();
        playlist.name = object.value(QStringLiteral("name")).toString();
        playlist.parentId = object.value(QStringLiteral("parent_id")).toInteger();
        playlist.isFolder = object.value(QStringLiteral("is_folder")).toBool();
        playlist.sortOrder = object.value(QStringLiteral("sort_order")).toInt();
        const QJsonArray trackIds = object.value(QStringLiteral("tracks")).toArray();
        for (const auto& trackId : trackIds) {
            playlist.trackIds.append(trackId.toInteger());
        }
        pPlaylists->append(playlist);
    }
    return true;
}

bool ProLinkSidecarClient::fetchTracks(
        int limit,
        int playlistId,
        QVector<ProLinkTrack>* pTracks,
        int* pTotal,
        QString* pError) {
    pTracks->clear();
    int offset = 0;
    int total = 0;
    do {
        QJsonObject params;
        params.insert(QStringLiteral("offset"), offset);
        params.insert(QStringLiteral("limit"), limit);
        if (playlistId >= 0) {
            params.insert(QStringLiteral("playlist"), playlistId);
        }
        QJsonObject result;
        if (!call(QStringLiteral("library.tracks"), params, &result, pError)) {
            return false;
        }
        total = result.value(QStringLiteral("total")).toInt();
        const QJsonArray tracks = result.value(QStringLiteral("tracks")).toArray();
        for (const auto& value : tracks) {
            const QJsonObject track = value.toObject();
            ProLinkTrack parsed;
            parsed.id = track.value(QStringLiteral("id")).toInteger();
            parsed.title = track.value(QStringLiteral("title")).toString();
            parsed.artist = track.value(QStringLiteral("artist")).toString();
            parsed.album = track.value(QStringLiteral("album")).toString();
            parsed.genre = track.value(QStringLiteral("genre")).toString();
            parsed.key = track.value(QStringLiteral("key")).toString();
            parsed.location = track.value(QStringLiteral("path")).toString();
            parsed.dateAdded = track.value(QStringLiteral("date_added")).toString();
            const QJsonObject analysis =
                    track.value(QStringLiteral("analysis")).toObject();
            parsed.anlzDat = analysis.value(QStringLiteral("dat")).toString();
            parsed.anlzExt = analysis.value(QStringLiteral("ext")).toString();
            const QJsonArray cues = track.value(QStringLiteral("cues")).toArray();
            for (const auto& cueValue : cues) {
                const QJsonObject object = cueValue.toObject();
                ProLinkCue cue;
                cue.hotCue = object.value(QStringLiteral("hot_cue")).toInt();
                cue.timeMs = object.value(QStringLiteral("time_ms")).toInteger();
                cue.loopEndMs =
                        object.value(QStringLiteral("loop_end_ms")).toInteger(-1);
                cue.colorId = object.value(QStringLiteral("color_id")).toInt();
                cue.comment = object.value(QStringLiteral("comment")).toString();
                const QJsonObject hotCueColor =
                        object.value(QStringLiteral("hot_cue_color")).toObject();
                const QJsonArray rgb = hotCueColor.value(QStringLiteral("rgb")).toArray();
                if (rgb.size() == 3) {
                    cue.hotCueRgb = (rgb.at(0).toInt() << 16) |
                            (rgb.at(1).toInt() << 8) | rgb.at(2).toInt();
                }
                parsed.cues.append(cue);
            }
            parsed.bpm = track.value(QStringLiteral("bpm")).toDouble() / 100.0;
            parsed.durationMs = static_cast<qint64>(
                    track.value(QStringLiteral("duration_ms")).toDouble());
            parsed.bitrate = track.value(QStringLiteral("bitrate")).toInt();
            pTracks->append(parsed);
        }
        if (tracks.isEmpty()) {
            break;
        }
        offset += tracks.size();
    } while (offset < total);
    *pTotal = total;
    return true;
}

bool ProLinkSidecarClient::call(
        const QString& method,
        const QJsonObject& params,
        QJsonObject* pResult,
        QString* pError) {
    if (m_pSocket->state() != QLocalSocket::ConnectedState) {
        *pError = QStringLiteral("the PRO DJ LINK sidecar is not connected");
        return false;
    }

    const qint64 id = m_nextId++;
    QJsonObject request;
    request.insert(QStringLiteral("id"), id);
    request.insert(QStringLiteral("method"), method);
    if (!params.isEmpty()) {
        request.insert(QStringLiteral("params"), params);
    }
    QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    payload.append('\n');
    if (m_pSocket->write(payload) != payload.size()) {
        *pError = QStringLiteral("cannot send %1 to the sidecar: %2")
                          .arg(method, m_pSocket->errorString());
        return false;
    }

    QByteArray buffer;
    while (!buffer.contains('\n')) {
        if (!m_pSocket->waitForReadyRead(kResponseTimeoutMs)) {
            *pError = QStringLiteral("the sidecar did not answer %1: %2")
                              .arg(method, m_pSocket->errorString());
            return false;
        }
        buffer.append(m_pSocket->readAll());
        if (buffer.size() > kMaxResponseBytes) {
            *pError = QStringLiteral("the sidecar answer to %1 is too large").arg(method);
            return false;
        }
    }

    QJsonParseError parseError;
    const QJsonDocument document =
            QJsonDocument::fromJson(buffer.left(buffer.indexOf('\n')), &parseError);
    if (!document.isObject()) {
        *pError = QStringLiteral("the sidecar sent invalid JSON for %1: %2")
                          .arg(method, parseError.errorString());
        return false;
    }
    const QJsonObject response = document.object();
    if (response.value(QStringLiteral("id")).toInteger(-1) != id) {
        *pError = QStringLiteral("the sidecar answered %1 out of order").arg(method);
        return false;
    }
    if (response.contains(QStringLiteral("error"))) {
        const QJsonObject error = response.value(QStringLiteral("error")).toObject();
        *pError = QStringLiteral("%1: %2")
                          .arg(error.value(QStringLiteral("code")).toString(),
                                  error.value(QStringLiteral("message")).toString());
        return false;
    }
    *pResult = response.value(QStringLiteral("result")).toObject();
    return true;
}
