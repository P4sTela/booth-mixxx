#pragma once

#include <QFuture>
#include <QFutureWatcher>
#include <QPointer>
#include <QSharedPointer>
#include <QUrl>
#include <QVector>

#include "library/baseexternallibraryfeature.h"
#include "library/baseexternaltrackmodel.h"
#include "library/prolink/sidecarclient.h"
#include "library/treeitemmodel.h"
#include "util/parented_ptr.h"

class BaseTrackCache;
class KeyboardEventFilter;
class WLibrary;
class WLibraryTextBrowser;

/// Library feature that shows the rekordbox collection through the PRO DJ LINK sidecar.
///
/// The sidecar is a separate process (booth's `pdl sidecar`); this class never links GPL code
/// with it, only talks JSON over a local socket. See docs/prolink-sidecar.md.
class ProLinkFeature : public BaseExternalLibraryFeature {
    Q_OBJECT
  public:
    ProLinkFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~ProLinkFeature() override;

    QVariant title() override;
    void bindLibraryWidget(WLibrary* libraryWidget, KeyboardEventFilter* keyboard) override;
    TreeItemModel* sidebarModel() const override;

  public slots:
    void activate() override;

  private slots:
    void loadFinished();
    void refreshClicked(const QUrl& url);

  private:
    struct LoadResult {
        QVector<ProLinkTrack> tracks;
        int total = 0;
        QString error;
        bool ok = false;
    };

    void startLoading();
    void showMessage(const QString& html);
    void writeTracks(const QVector<ProLinkTrack>& tracks);

    parented_ptr<TreeItemModel> m_pSidebarModel;
    parented_ptr<BaseExternalTrackModel> m_pTrackModel;
    QSharedPointer<BaseTrackCache> m_trackSource;
    QSharedPointer<LoadResult> m_pLoadResult;
    QFuture<void> m_loadFuture;
    QFutureWatcher<void> m_loadWatcher;
    QPointer<WLibraryTextBrowser> m_pHomeView;
};
