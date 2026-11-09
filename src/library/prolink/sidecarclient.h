#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

/// One track from the PRO DJ LINK sidecar (the `library.tracks` method).
struct ProLinkTrack {
    int id = 0;
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString key;
    QString location;
    QString dateAdded;
    /// BPM, already divided by 100 (the sidecar reports rekordbox's value scaled by 100).
    double bpm = 0.0;
    qint64 durationMs = 0;
    int bitrate = 0;
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

    /// Read every track, `limit` at a time.
    bool fetchTracks(int limit, QVector<ProLinkTrack>* pTracks, int* pTotal, QString* pError);

  private:
    bool call(const QString& method,
            const QJsonObject& params,
            QJsonObject* pResult,
            QString* pError);

    QString m_socketPath;
    class QLocalSocket* m_pSocket;
    qint64 m_nextId = 1;
};
