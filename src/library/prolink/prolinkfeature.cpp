#include "library/prolink/prolinkfeature.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QtConcurrent>

#include "library/basetrackcache.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/queryutil.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "moc_prolinkfeature.cpp"
#include "widget/wlibrary.h"
#include "widget/wlibrarytextbrowser.h"

namespace {

const QString kLibraryTable = QStringLiteral("prolink_library");
const QString kHomeView = QStringLiteral("PROLINKHOME");
constexpr int kPageSize = 5000;

} // namespace

ProLinkFeature::ProLinkFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : BaseExternalLibraryFeature(
                  pLibrary, pConfig, QStringLiteral("rekordbox")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)) {
    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("DROP TABLE IF EXISTS ") + kLibraryTable) ||
            !query.exec(QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS prolink_library ("
                    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    " rb_id INTEGER, artist TEXT, title TEXT, album TEXT,"
                    " year INTEGER, genre TEXT, tracknumber TEXT, location TEXT,"
                    " comment TEXT, duration INTEGER, bitrate TEXT, bpm FLOAT,"
                    " key TEXT, rating INTEGER, analyze_path TEXT, device TEXT,"
                    " color INTEGER)"))) {
        LOG_FAILED_QUERY(query) << "cannot create the PRO DJ LINK library table";
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
    m_pTrackModel = make_parented<BaseExternalTrackModel>(
            this,
            pLibrary->trackCollectionManager(),
            "PROLINK",
            kLibraryTable,
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
        if (!client.fetchTracks(kPageSize, &pResult->tracks, &pResult->total, &error)) {
            pResult->error = error;
            return;
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
    emit showTrackModel(m_pTrackModel.get());
}

void ProLinkFeature::writeTracks(const QVector<ProLinkTrack>& tracks) {
    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("DELETE FROM ") + kLibraryTable)) {
        LOG_FAILED_QUERY(query) << "cannot clear the PRO DJ LINK library table";
    }
    query.prepare(QStringLiteral(
            "INSERT INTO prolink_library"
            " (rb_id, artist, title, album, genre, location, duration,"
            " bitrate, bpm, key)"
            " VALUES (:rb_id, :artist, :title, :album, :genre, :location,"
            " :duration, :bitrate, :bpm, :key)"));
    for (const auto& track : tracks) {
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
        if (!query.exec()) {
            LOG_FAILED_QUERY(query) << "cannot add track" << track.location;
        }
    }
    transaction.commit();
    m_pTrackModel->select();
}

void ProLinkFeature::showMessage(const QString& html) {
    if (m_pHomeView) {
        m_pHomeView->setHtml(html);
    }
}
