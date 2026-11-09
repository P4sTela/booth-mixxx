#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

/// A memory point, hot cue or loop from rekordbox.
struct ProLinkCue {
    /// 0 for memory points, 1 = hot cue A, 2 = B, …
    int hotCue = 0;
    qint64 timeMs = 0;
    /// Set for loops.
    qint64 loopEndMs = -1;
    int colorId = 0;
    /// 0xRRGGBB of the hot cue colour, -1 when rekordbox has none.
    int hotCueRgb = -1;
    QString comment;
};

/// One track from the PRO DJ LINK sidecar (the `library.tracks` method).
struct ProLinkTrack {
    /// rekordbox's content ID.
    qint64 id = 0;
    /// ID used in the Mixxx-side tables (assigned after fetching).
    int localId = 0;
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString key;
    QString location;
    QString dateAdded;
    /// Absolute paths of the ANLZ analysis files, empty when rekordbox has none.
    QString anlzDat;
    QString anlzExt;
    QVector<ProLinkCue> cues;
    /// BPM, already divided by 100 (the sidecar reports rekordbox's value scaled by 100).
    double bpm = 0.0;
    qint64 durationMs = 0;
    int bitrate = 0;
};

/// One playlist or folder (the `library.playlists` method).
struct ProLinkPlaylist {
    /// rekordbox's playlist ID.
    qint64 id = 0;
    /// ID used in the Mixxx-side tables (assigned after fetching).
    int localId = 0;
    QString name;
    qint64 parentId = 0;
    bool isFolder = false;
    int sortOrder = 0;
    /// Track IDs in playlist order (rekordbox IDs until remapped, then local IDs).
    QVector<qint64> trackIds;
};

/// Blocking client for the sidecar protocol (docs/prolink-sidecar.md in booth-mixxx).
///
/// Use it from a worker thread: every call waits for the reply. The socket is a local Unix
/// socket; the sidecar is a separate MIT/Apache-licensed process, so no GPL code is shared.
class ProLinkSidecarClient {
  public:
    explicit ProLinkSidecarClient(QString socketPath);
    ~ProLinkSidecarClient();

    ProLinkSidecarClient(const ProLinkSidecarClient&) = delete;
    ProLinkSidecarClient& operator=(const ProLinkSidecarClient&) = delete;

    /// `PDL_SIDECAR_SOCKET`, else `$TMPDIR/pdl-sidecar.sock`.
    static QString defaultSocketPath();

    /// Connect and perform the `hello` handshake.
    bool connectToSidecar(QString* pError);

    /// Open the rekordbox library (the sidecar copies and decrypts it).
    bool openLibrary(QString* pError);

    /// Read every playlist and folder, parents before children.
    bool fetchPlaylists(QVector<ProLinkPlaylist>* pPlaylists, QString* pError);

    /// Read every track, `limit` at a time. `playlistId >= 0` limits it to that playlist.
    bool fetchTracks(int limit,
            int playlistId,
            QVector<ProLinkTrack>* pTracks,
            int* pTotal,
            QString* pError);

  private:
    bool call(const QString& method,
            const QJsonObject& params,
            QJsonObject* pResult,
            QString* pError);

    QString m_socketPath;
    class QLocalSocket* m_pSocket;
    qint64 m_nextId = 1;
};
