#include "library/prolink/prolinkfeature.h"

#include <QFile>
#include <QHash>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QtConcurrent>

#include <memory>
#include <vector>

#include <algorithm>

#include "library/basetrackcache.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/queryutil.h"
#include "library/rekordbox/rekordboxanlz.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "moc_prolinkfeature.cpp"
#include "track/cue.h"
#include "track/keyfactory.h"
#include "track/track.h"
#include "util/color/color.h"
#include "util/color/predefinedcolorpalettes.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarytextbrowser.h"

namespace {

const QString kLibraryTable = QStringLiteral("prolink_library");
const QString kPlaylistsTable = QStringLiteral("prolink_playlists");
const QString kPlaylistTracksTable = QStringLiteral("prolink_playlist_tracks");
const QString kCuesTable = QStringLiteral("prolink_cues");
const QString kHomeView = QStringLiteral("PROLINKHOME");
constexpr int kPageSize = 5000;

void applyAnalysis(const TrackPointer& track,
        const QString& location,
        const QString& anlzPath,
        const QString& key) {
    if (!track || anlzPath.isEmpty() || !QFile::exists(location)) {
        return;
    }
    const auto sampleRate = track->getSampleRate();
    const int timingOffset = mixxx::rekordbox::timingOffsetFor(location);
    // Beat grid only: cues come from the sidecar (djmdCue, or the ANLZ files).
    mixxx::rekordbox::readAnalyze(track, sampleRate, timingOffset, true, anlzPath);

    // rekordbox's analysis is the source of truth; keep Mixxx's analyzer off the key.
    track->setKeys(KeyFactory::makeBasicKeysNormalized(
            key, mixxx::track::io::key::USER));
}

void setCue(const TrackPointer& track,
        int id,
        mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition,
        const QString& label,
        mixxx::RgbColor::optional_t color) {
    CuePointer pCue;
    for (const CuePointer& trackCue : track->getCuePoints()) {
        if (trackCue->getHotCue() == id) {
            pCue = trackCue;
            break;
        }
    }
    const mixxx::CueType type = endPosition.isValid()
            ? mixxx::CueType::Loop
            : mixxx::CueType::HotCue;
    if (pCue) {
        pCue->setStartAndEndPosition(startPosition, endPosition);
    } else {
        pCue = track->createAndAddCue(
                type, id, startPosition, endPosition);
    }
    pCue->setLabel(label);
    if (color) {
        pCue->setColor(*color);
    }
}

void applyCues(const TrackPointer& track,
        int trackId,
        const QSqlDatabase& database,
        mixxx::audio::SampleRate sampleRate) {
    if (!track) {
        return;
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
            "SELECT hot_cue, time_ms, loop_end_ms, color_id, hot_cue_rgb, comment"
            " FROM prolink_cues WHERE track_id=:id ORDER BY hot_cue, time_ms"));
    query.bindValue(QStringLiteral(":id"), trackId);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query) << "cannot read the PRO DJ LINK cues";
        return;
    }

    const double framesPerMs = sampleRate / 1000.0;
    int lastHotCue = -1;
    bool mainCueFound = false;
    while (query.next()) {
        const int hotCue = query.value(0).toInt();
        const qint64 timeMs = query.value(1).toLongLong();
        const bool isLoop = !query.value(2).isNull();
        const auto position = mixxx::audio::FramePos(
                framesPerMs * static_cast<double>(timeMs));
        const auto endPosition = isLoop
                ? mixxx::audio::FramePos(framesPerMs * query.value(2).toLongLong())
                : mixxx::audio::kInvalidFramePos;

        mixxx::RgbColor::optional_t color;
        const int rgb = query.value(4).toInt();
        if (rgb >= 0) {
            color = mixxx::RgbColor(rgb);
        } else if (query.value(3).toInt() > 0) {
            color = mixxx::rekordbox::colorFromID(query.value(3).toInt());
        }
        const QString comment = query.value(5).toString();

        if (hotCue == 0) {
            CuePointer pMemoryCue = track->createAndAddCue(
                    mixxx::CueType::Memory,
                    Cue::kNoHotCue,
                    position,
                    endPosition,
                    color ? *color
                          : mixxx::PredefinedColorPalettes::kDefaultCueColor);
            if (pMemoryCue) {
                pMemoryCue->setLabel(comment);
                if (color) {
                    pMemoryCue->setColor(*color);
                }
            }
            if (!mainCueFound) {
                // Mixxx needs a main cue for the CUE button; rekordbox's first memory
                // point is the closest equivalent.
                track->setMainCuePosition(position);
                CuePointer pMainCue = track->findCueByType(mixxx::CueType::MainCue);
                pMainCue->setLabel(comment);
                if (color) {
                    pMainCue->setColor(*color);
                }
                mainCueFound = true;
            }
        } else {
            const int id = hotCue - 1;
            lastHotCue = std::max(lastHotCue, id);
            setCue(track, id, position, endPosition, comment, color);
        }
    }
}

} // namespace

ProLinkFeature::ProLinkFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : BaseExternalLibraryFeature(
                  pLibrary, pConfig, QStringLiteral("rekordbox")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)) {
    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    QSqlQuery query(database);
    const QStringList statements = {
            QStringLiteral("DROP TABLE IF EXISTS ") + kCuesTable,
            QStringLiteral("DROP TABLE IF EXISTS ") + kPlaylistTracksTable,
            QStringLiteral("DROP TABLE IF EXISTS ") + kPlaylistsTable,
            QStringLiteral("DROP TABLE IF EXISTS ") + kLibraryTable,
            QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS prolink_library ("
                    " id INTEGER PRIMARY KEY,"
                    " rb_id INTEGER, artist TEXT, title TEXT, album TEXT,"
                    " year INTEGER, genre TEXT, tracknumber TEXT, location TEXT,"
                    " comment TEXT, duration INTEGER, bitrate TEXT, bpm FLOAT,"
                    " key TEXT, rating INTEGER, analyze_path TEXT, device TEXT,"
                    " color INTEGER)"),
            QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS prolink_playlists ("
                    " id INTEGER PRIMARY KEY, name TEXT)"),
            QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS prolink_playlist_tracks ("
                    " id INTEGER PRIMARY KEY AUTOINCREMENT, playlist_id INTEGER,"
                    " track_id INTEGER, position INTEGER)"),
            QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS prolink_cues ("
                    " id INTEGER PRIMARY KEY AUTOINCREMENT, track_id INTEGER,"
                    " hot_cue INTEGER, time_ms INTEGER, loop_end_ms INTEGER,"
                    " color_id INTEGER, hot_cue_rgb INTEGER, comment TEXT)"),
    };
    for (const auto& statement : statements) {
        if (!query.exec(statement)) {
            LOG_FAILED_QUERY(query) << "cannot create the PRO DJ LINK tables";
            break;
        }
    }
    transaction.commit();

    const QString idColumn = LIBRARYTABLE_ID;
    QStringList columns = {
            LIBRARYTABLE_ID,
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_YEAR,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT,
            LIBRARYTABLE_RATING,
            LIBRARYTABLE_DURATION,
            LIBRARYTABLE_BITRATE,
            LIBRARYTABLE_BPM,
            LIBRARYTABLE_KEY,
            LIBRARYTABLE_COLOR};
    const QStringList searchColumns = {
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_GENRE,
            TRACKLOCATIONSTABLE_LOCATION};

    m_trackSource = QSharedPointer<BaseTrackCache>::create(
            m_pTrackCollection,
            kLibraryTable,
            std::move(idColumn),
            std::move(columns),
            std::move(searchColumns),
            false);
    m_pTrackModel = make_parented<ProLinkTrackModel>(
            this,
            pLibrary->trackCollectionManager(),
            "PROLINK",
            kLibraryTable,
            m_trackSource);
    m_pPlaylistModel = make_parented<ProLinkPlaylistModel>(
            this,
            pLibrary->trackCollectionManager(),
            m_trackSource);

    connect(&m_loadWatcher,
            &QFutureWatcher<void>::finished,
            this,
            &ProLinkFeature::loadFinished);
    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));
}

ProLinkFeature::~ProLinkFeature() {
    m_loadFuture.waitForFinished();
    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    QSqlQuery query(database);
    query.exec(QStringLiteral("DROP TABLE IF EXISTS ") + kCuesTable);
    query.exec(QStringLiteral("DROP TABLE IF EXISTS ") + kPlaylistTracksTable);
    query.exec(QStringLiteral("DROP TABLE IF EXISTS ") + kPlaylistsTable);
    query.exec(QStringLiteral("DROP TABLE IF EXISTS ") + kLibraryTable);
    transaction.commit();
}

QVariant ProLinkFeature::title() {
    return tr("PRO DJ LINK");
}

TreeItemModel* ProLinkFeature::sidebarModel() const {
    return m_pSidebarModel.get();
}

void ProLinkFeature::bindLibraryWidget(
        WLibrary* pLibraryWidget,
        KeyboardEventFilter* keyboard) {
    Q_UNUSED(keyboard);
    auto pEdit = make_parented<WLibraryTextBrowser>(pLibraryWidget);
    pEdit->setOpenLinks(false);
    connect(pEdit,
            &WLibraryTextBrowser::anchorClicked,
            this,
            &ProLinkFeature::refreshClicked);
    pEdit->setHtml(QStringLiteral("<h2>%1</h2>").arg(tr("PRO DJ LINK")));
    pLibraryWidget->registerView(kHomeView, pEdit);
    m_pHomeView = pEdit.get();
}

void ProLinkFeature::activate() {
    emit enableCoverArtDisplay(true);
    emit switchToView(kHomeView);
    emit disableSearch();
    startLoading();
}

void ProLinkFeature::activateChild(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    auto* item = static_cast<TreeItem*>(index.internalPointer());
    if (!item) {
        return;
    }
    const QVariantList data = item->getData().toList();
    if (data.size() < 2) {
        return;
    }
    const int playlistId = data.at(0).toInt();
    const bool isFolder = data.at(1).toBool();
    if (isFolder) {
        return;
    }
    m_pPlaylistModel->setPlaylistById(playlistId);
    emit showTrackModel(m_pPlaylistModel.get());
}

void ProLinkFeature::refreshClicked(const QUrl& url) {
    if (url.path() == QLatin1String("refresh")) {
        startLoading();
    }
}

void ProLinkFeature::startLoading() {
    if (m_loadWatcher.isRunning()) {
        return;
    }
    m_pLoadResult = QSharedPointer<LoadResult>::create();
    showMessage(QStringLiteral("<h2>%1</h2><p>%2</p>")
                        .arg(tr("PRO DJ LINK"),
                                tr("Reading the rekordbox library through the "
                                   "sidecar…")));
    emit featureIsLoading(this, true);

    auto pResult = m_pLoadResult;
    m_loadFuture = QtConcurrent::run([pResult]() {
        ProLinkSidecarClient client(ProLinkSidecarClient::defaultSocketPath());
        QString error;
        if (!client.connectToSidecar(&error) || !client.openLibrary(&error)) {
            pResult->error = error;
            return;
        }
        if (!client.fetchTracks(kPageSize, -1, &pResult->tracks, &pResult->total, &error) ||
                !client.fetchPlaylists(&pResult->playlists, &error)) {
            pResult->error = error;
            return;
        }

        // rekordbox IDs do not fit in the ints Mixxx uses for playlist IDs, so give every
        // track and playlist a local ID and rewrite the references to it.
        QHash<qint64, int> trackIds;
        for (int i = 0; i < pResult->tracks.size(); ++i) {
            pResult->tracks[i].localId = i + 1;
            trackIds.insert(pResult->tracks[i].id, i + 1);
        }
        QHash<qint64, int> playlistIds;
        for (int i = 0; i < pResult->playlists.size(); ++i) {
            pResult->playlists[i].localId = i + 1;
            playlistIds.insert(pResult->playlists[i].id, i + 1);
        }
        for (auto& playlist : pResult->playlists) {
            playlist.parentId = playlistIds.value(playlist.parentId, 0);
            QVector<qint64> tracks;
            for (const qint64 trackId : playlist.trackIds) {
                if (trackIds.contains(trackId)) {
                    tracks.append(trackIds.value(trackId));
                }
            }
            playlist.trackIds = tracks;
        }
        pResult->ok = true;
    });
    m_loadWatcher.setFuture(m_loadFuture);
}

void ProLinkFeature::loadFinished() {
    emit featureLoadingFinished(this);
    const auto pResult = m_pLoadResult;
    m_pLoadResult.clear();
    if (!pResult) {
        return;
    }
    if (!pResult->ok) {
        showMessage(QStringLiteral("<h2>%1</h2><p>%2</p>")
                            .arg(tr("PRO DJ LINK"), pResult->error.toHtmlEscaped()));
        emit switchToView(kHomeView);
        return;
    }
    writeTracks(pResult->tracks);
    writePlaylists(pResult->playlists);
    updateSidebar(pResult->playlists);
    emit showTrackModel(m_pTrackModel.get());
}

void ProLinkFeature::writeTracks(const QVector<ProLinkTrack>& tracks) {
    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("DELETE FROM ") + kLibraryTable) ||
            !query.exec(QStringLiteral("DELETE FROM ") + kCuesTable)) {
        LOG_FAILED_QUERY(query) << "cannot clear the PRO DJ LINK library table";
    }
    query.prepare(QStringLiteral(
            "INSERT INTO prolink_library"
            " (id, rb_id, artist, title, album, genre, location, duration,"
            " bitrate, bpm, key, analyze_path)"
            " VALUES (:id, :rb_id, :artist, :title, :album, :genre, :location,"
            " :duration, :bitrate, :bpm, :key, :analyze_path)"));
    QSqlQuery cueQuery(database);
    cueQuery.prepare(QStringLiteral(
            "INSERT INTO prolink_cues"
            " (track_id, hot_cue, time_ms, loop_end_ms, color_id, hot_cue_rgb,"
            " comment)"
            " VALUES (:track_id, :hot_cue, :time_ms, :loop_end_ms, :color_id,"
            " :hot_cue_rgb, :comment)"));
    for (const auto& track : tracks) {
        query.bindValue(QStringLiteral(":id"), track.localId);
        query.bindValue(QStringLiteral(":rb_id"), track.id);
        query.bindValue(QStringLiteral(":artist"), track.artist);
        query.bindValue(QStringLiteral(":title"), track.title);
        query.bindValue(QStringLiteral(":album"), track.album);
        query.bindValue(QStringLiteral(":genre"), track.genre);
        query.bindValue(QStringLiteral(":location"), track.location);
        query.bindValue(
                QStringLiteral(":duration"),
                static_cast<int>(track.durationMs / 1000));
        query.bindValue(QStringLiteral(":bitrate"), track.bitrate);
        query.bindValue(QStringLiteral(":bpm"), track.bpm);
        query.bindValue(QStringLiteral(":key"), track.key);
        query.bindValue(QStringLiteral(":analyze_path"), track.anlzDat);
        if (!query.exec()) {
            LOG_FAILED_QUERY(query) << "cannot add track" << track.location;
            continue;
        }
        for (const auto& cue : track.cues) {
            cueQuery.bindValue(QStringLiteral(":track_id"), track.localId);
            cueQuery.bindValue(QStringLiteral(":hot_cue"), cue.hotCue);
            cueQuery.bindValue(QStringLiteral(":time_ms"), cue.timeMs);
            cueQuery.bindValue(QStringLiteral(":loop_end_ms"),
                    cue.loopEndMs >= 0 ? QVariant(cue.loopEndMs) : QVariant());
            cueQuery.bindValue(QStringLiteral(":color_id"), cue.colorId);
            cueQuery.bindValue(QStringLiteral(":hot_cue_rgb"), cue.hotCueRgb);
            cueQuery.bindValue(QStringLiteral(":comment"), cue.comment);
            if (!cueQuery.exec()) {
                LOG_FAILED_QUERY(cueQuery)
                        << "cannot add cue for" << track.location;
            }
        }
    }
    transaction.commit();
    m_pTrackModel->select();
}

void ProLinkFeature::writePlaylists(const QVector<ProLinkPlaylist>& playlists) {
    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("DELETE FROM ") + kPlaylistTracksTable) ||
            !query.exec(QStringLiteral("DELETE FROM ") + kPlaylistsTable)) {
        LOG_FAILED_QUERY(query) << "cannot clear the PRO DJ LINK playlist tables";
    }

    QSqlQuery insertPlaylist(database);
    insertPlaylist.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO prolink_playlists (id, name)"
            " VALUES (:id, :name)"));
    QSqlQuery insertEntry(database);
    insertEntry.prepare(QStringLiteral(
            "INSERT INTO prolink_playlist_tracks"
            " (playlist_id, track_id, position)"
            " VALUES (:playlist_id, :track_id, :position)"));

    for (const auto& playlist : playlists) {
        insertPlaylist.bindValue(QStringLiteral(":id"), playlist.localId);
        insertPlaylist.bindValue(QStringLiteral(":name"), playlist.name);
        if (!insertPlaylist.exec()) {
            LOG_FAILED_QUERY(insertPlaylist) << "cannot add playlist" << playlist.name;
        }
        for (int position = 0; position < playlist.trackIds.size(); ++position) {
            insertEntry.bindValue(QStringLiteral(":playlist_id"), playlist.localId);
            insertEntry.bindValue(QStringLiteral(":track_id"), playlist.trackIds.at(position));
            insertEntry.bindValue(QStringLiteral(":position"), position);
            if (!insertEntry.exec()) {
                LOG_FAILED_QUERY(insertEntry)
                        << "cannot add track" << playlist.trackIds.at(position)
                        << "to" << playlist.name;
            }
        }
    }
    transaction.commit();
}

void ProLinkFeature::updateSidebar(const QVector<ProLinkPlaylist>& playlists) {
    TreeItem* root = m_pSidebarModel->getRootItem();
    if (!root) {
        return;
    }
    if (root->childRows() > 0) {
        m_pSidebarModel->removeRows(0, root->childRows());
    }

    QHash<int, TreeItem*> items;
    std::vector<std::unique_ptr<TreeItem>> topLevel;
    for (const auto& playlist : playlists) {
        const QVariant data = QVariantList{playlist.localId, playlist.isFolder};
        TreeItem* parent = items.value(static_cast<int>(playlist.parentId), nullptr);
        if (playlist.parentId == 0 || !parent) {
            topLevel.push_back(std::make_unique<TreeItem>(playlist.name, data));
            items.insert(playlist.localId, topLevel.back().get());
        } else {
            items.insert(playlist.localId, parent->appendChild(playlist.name, data));
        }
    }
    m_pSidebarModel->insertTreeItemRows(std::move(topLevel), 0);
}

void ProLinkFeature::showMessage(const QString& html) {
    if (m_pHomeView) {
        m_pHomeView->setHtml(html);
    }
}

ProLinkTrackModel::ProLinkTrackModel(QObject* parent,
        TrackCollectionManager* pTrackCollectionManager,
        const char* settingsNamespace,
        const QString& trackTable,
        QSharedPointer<BaseTrackCache> trackSource)
        : BaseExternalTrackModel(parent,
                  pTrackCollectionManager,
                  settingsNamespace,
                  trackTable,
                  trackSource) {
}

TrackPointer ProLinkTrackModel::getTrack(const QModelIndex& index) const {
    TrackPointer track = BaseExternalTrackModel::getTrack(index);
    applyAnalysis(track,
            getTrackLocation(index),
            getFieldVariant(index, ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH).toString(),
            getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_KEY).toString());
    if (track) {
        applyCues(track,
                getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_ID).toInt(),
                m_database,
                track->getSampleRate());
        track->setColor(mixxx::RgbColor::fromQVariant(
                getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_COLOR)));
    }
    return track;
}

bool ProLinkTrackModel::isColumnInternal(int column) {
    return column == fieldIndex(ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH) ||
            BaseExternalTrackModel::isColumnInternal(column);
}

ProLinkPlaylistModel::ProLinkPlaylistModel(
        QObject* parent,
        TrackCollectionManager* pTrackCollectionManager,
        QSharedPointer<BaseTrackCache> trackSource)
        : BaseExternalPlaylistModel(parent,
                  pTrackCollectionManager,
                  "PROLINK_PLAYLIST",
                  kPlaylistsTable,
                  kPlaylistTracksTable,
                  trackSource) {
}

TrackPointer ProLinkPlaylistModel::getTrack(const QModelIndex& index) const {
    TrackPointer track = BaseExternalPlaylistModel::getTrack(index);
    applyAnalysis(track,
            getTrackLocation(index),
            getFieldVariant(index, ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH).toString(),
            getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_KEY).toString());
    if (track) {
        applyCues(track,
                getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_ID).toInt(),
                m_database,
                track->getSampleRate());
    }
    return track;
}

bool ProLinkPlaylistModel::isColumnInternal(int column) {
    return column == fieldIndex(ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH) ||
            BaseExternalPlaylistModel::isColumnInternal(column);
}
