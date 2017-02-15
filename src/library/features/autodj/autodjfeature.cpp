// autodjfeature.cpp
// FORK FORK FORK on 11/1/2009 by Albert Santoni (alberts@mixxx.org)
// Created 8/23/2009 by RJ Ryan (rryan@mit.edu)

#include <QtDebug>
#include <QMetaObject>
#include <QMenu>
#include <QScrollArea>
#include <QSplitter>

#include "library/features/autodj/autodjfeature.h"
#include "library/features/autodj/dlgautodj.h"
#include "controllers/keyboard/keyboardeventfilter.h"
#include "library/features/autodj/autodjprocessor.h"
#include "library/parser.h"
#include "library/trackcollection.h"
#include "mixer/playermanager.h"
#include "library/trackcollection.h"
#include "library/treeitem.h"
#include "library/features/crates/cratestorage.h"
#include "controllers/keyboard/keyboardeventfilter.h"
#include "sources/soundsourceproxy.h"
#include "util/dnd.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wpixmapstore.h"

namespace {
    const int kMaxRetrieveAttempts = 3;
} // anonymous namespace

AutoDJFeature::AutoDJFeature(UserSettingsPointer pConfig,
                             Library* pLibrary,
                             QObject* parent,
                             PlayerManagerInterface* pPlayerManager,
                             TrackCollection* pTrackCollection)
        : LibraryFeature(pConfig, pLibrary, pTrackCollection, parent),
          m_pTrackCollection(pTrackCollection),
          m_playlistDao(pTrackCollection->getPlaylistDAO()),
          m_iAutoDJPlaylistId(-1),
          m_pAutoDJProcessor(nullptr),
          m_pAutoDJView(nullptr),
          m_autoDjCratesDao(pTrackCollection, pConfig) {
    m_iAutoDJPlaylistId = m_playlistDao.getPlaylistIdFromName(AUTODJ_TABLE);
    // If the AutoDJ playlist does not exist yet then create it.
    if (m_iAutoDJPlaylistId < 0) {
        m_iAutoDJPlaylistId = m_playlistDao.createPlaylist(
                AUTODJ_TABLE, PlaylistDAO::PLHT_AUTO_DJ);
        VERIFY_OR_DEBUG_ASSERT(m_iAutoDJPlaylistId >= 0) {
            qWarning() << "Failed to create Auto DJ playlist!";
        }
    }
    // The AutoDJCratesDAO expects that the dedicated AutoDJ playlist
    // has already been created.
    m_autoDjCratesDao.initialize();

    qRegisterMetaType<AutoDJProcessor::AutoDJState>("AutoDJState");
    m_pAutoDJProcessor = new AutoDJProcessor(
            this, m_pConfig, pPlayerManager, m_iAutoDJPlaylistId, m_pTrackCollection);
    connect(m_pAutoDJProcessor, SIGNAL(loadTrackToPlayer(TrackPointer, QString, bool)),
            this, SIGNAL(loadTrackToPlayer(TrackPointer, QString, bool)));
    m_playlistDao.setAutoDJProcessor(m_pAutoDJProcessor);

    // Create the "Crates" tree-item under the root item.
    auto pRootItem = std::make_unique<TreeItem>(this);
    m_pCratesTreeItem = pRootItem->appendChild(tr("Crates"));
    // we set the Icon later, because the icon loader is not fully set up yet 

    // Create tree-items under "Crates".
    constructCrateChildModel();

    m_childModel.setRootItem(std::move(pRootItem));

    // Be notified when the status of crates changes.
    connect(m_pTrackCollection, SIGNAL(crateInserted(CrateId)),
            this, SLOT(slotCrateChanged(CrateId)));
    connect(m_pTrackCollection, SIGNAL(crateUpdated(CrateId)),
            this, SLOT(slotCrateChanged(CrateId)));
    connect(m_pTrackCollection, SIGNAL(crateDeleted(CrateId)),
            this, SLOT(slotCrateChanged(CrateId)));

    // Create context-menu items to allow crates to be added to, and removed
    // from, the auto-DJ queue.
    connect(&m_crateMapper, SIGNAL(mapped(int)),
            this, SLOT(slotAddCrateToAutoDj(int)));
    m_pRemoveCrateFromAutoDj = new QAction(tr("Remove Crate as Track Source"), this);
    connect(m_pRemoveCrateFromAutoDj, SIGNAL(triggered()),
            this, SLOT(slotRemoveCrateFromAutoDj()));
}

AutoDJFeature::~AutoDJFeature() {
    delete m_pRemoveCrateFromAutoDj;
    delete m_pAutoDJProcessor;
}

QVariant AutoDJFeature::title() {
    return tr("Auto DJ");
}

QString AutoDJFeature::getIconPath() {
    return ":/images/library/ic_library_autodj.png";
}

QString AutoDJFeature::getSettingsName() const {
    return "AutoDJFeature";
}

parented_ptr<QWidget> AutoDJFeature::createPaneWidget(KeyboardEventFilter*, 
            int paneId, QWidget* parent) {
    auto pTrackTableView = createTableWidget(paneId, parent);
    pTrackTableView->loadTrackModel(m_pAutoDJProcessor->getTableModel());

    connect(pTrackTableView->selectionModel(),
            SIGNAL(selectionChanged(const QItemSelection&, const QItemSelection&)),
            this,
            SLOT(selectionChanged(const QItemSelection&, const QItemSelection&)));

    return pTrackTableView;
}

parented_ptr<QWidget> AutoDJFeature::createInnerSidebarWidget(
            KeyboardEventFilter* pKeyboard, QWidget* parent) {
    auto pContainer = make_parented<QTabWidget>(parent);

    // now the icon loader is set up and get an icon 
    m_pCratesTreeItem->setIcon(WPixmapStore::getLibraryIcon(
            ":/images/library/ic_library_crates.png"));

    // Add controls
    auto pAutoDJView = make_parented<DlgAutoDJ>(pContainer.get(), m_pAutoDJProcessor);
    m_pAutoDJView = pAutoDJView.toWeakRef();
    m_pAutoDJView->installEventFilter(pKeyboard);
    auto pScroll = make_parented<QScrollArea>(pContainer.get());
    pScroll->setWidget(m_pAutoDJView);
    pScroll->setWidgetResizable(true);
    pContainer->addTab(pScroll.get(), tr("Controls"));

    // Add drop target
    auto pSidebar = createLibrarySidebarWidget(pContainer.get());
    pContainer->addTab(pSidebar.get(), tr("Track source"));

    // Be informed when the user wants to add another random track.
    connect(m_pAutoDJProcessor,SIGNAL(randomTrackRequested(int)),
            this,SLOT(slotRandomQueue(int)));
    connect(m_pAutoDJView, SIGNAL(addRandomButton(bool)),
            this, SLOT(slotAddRandomTrack()));

    return pContainer;
}

TreeItemModel* AutoDJFeature::getChildModel() {
    return &m_childModel;
}

void AutoDJFeature::activate() {
    //qDebug() << "AutoDJFeature::activate()";
    VERIFY_OR_DEBUG_ASSERT(!m_pAutoDJView.isNull()) {
        return;
    }

    m_pAutoDJView->onShow();

    switchToFeature();
    showBreadCrumb();
    restoreSearch(QString()); // Null String disables search box

}

bool AutoDJFeature::dropAccept(QList<QUrl> urls, QObject* pSource) {
    // If a track is dropped onto a playlist's name, but the track isn't in the
    // library, then add the track to the library before adding it to the
    // playlist.
    QList<QFileInfo> files = DragAndDropHelper::supportedTracksFromUrls(urls, false, true);
    QList<TrackId> trackIds;
    if (pSource) {
        trackIds = m_pTrackCollection->getTrackDAO().getTrackIds(files);
        m_pTrackCollection->unhideTracks(trackIds);
    } else {
        trackIds = m_pTrackCollection->getTrackDAO().addMultipleTracks(files, true);
    }

    // remove tracks that could not be added
    for (int trackIdIndex = 0; trackIdIndex < trackIds.size(); trackIdIndex++) {
        if (!trackIds.at(trackIdIndex).isValid()) {
            trackIds.removeAt(trackIdIndex--);
        }
    }

    // Return whether the tracks were appended.
    return m_playlistDao.appendTracksToPlaylist(trackIds, m_iAutoDJPlaylistId);
}

bool AutoDJFeature::dragMoveAccept(QUrl url) {
    return SoundSourceProxy::isUrlSupported(url) ||
            Parser::isPlaylistFilenameSupported(url.toLocalFile());
}

// Add a crate to the auto-DJ queue.
void AutoDJFeature::slotAddCrateToAutoDj(int iCrateId) {
    m_pTrackCollection->updateAutoDjCrate(CrateId(iCrateId), true);
}

void AutoDJFeature::slotRemoveCrateFromAutoDj() {
    CrateId crateId(m_pRemoveCrateFromAutoDj->data());
    DEBUG_ASSERT(crateId.isValid());
    m_pTrackCollection->updateAutoDjCrate(crateId, false);
}

void AutoDJFeature::slotCrateChanged(CrateId crateId) {
    Crate crate;
    if (m_pTrackCollection->crates().readCrateById(crateId, &crate) && crate.isAutoDjSource()) {
        // Crate exists and is already a source for AutoDJ
        // -> Find and update the corresponding child item
        for (int i = 0; i < m_crateList.length(); ++i) {
            if (m_crateList[i].getId() == crateId) {
                QModelIndex parentIndex = m_childModel.index(0, 0);
                QModelIndex childIndex = parentIndex.child(i, 0);
                m_childModel.setData(childIndex, crate.getName(), Qt::DisplayRole);
                m_crateList[i] = crate;
                return; // early exit
            }
        }
        // No child item for crate found
        // -> Create and append a new child item for this crate
        QList<TreeItem*> rows;
        rows.append(new TreeItem(this, crate.getName(), crate.getId().toVariant()));
        QModelIndex parentIndex = m_childModel.index(0, 0);
        m_childModel.insertTreeItemRows(rows, m_crateList.length(), parentIndex);
        DEBUG_ASSERT(rows.isEmpty()); // ownership passed to m_childModel
        m_crateList.append(crate);
    } else {
        // Crate does not exist or is not a source for AutoDJ
        // -> Find and remove the corresponding child item
        for (int i = 0; i < m_crateList.length(); ++i) {
            if (m_crateList[i].getId() == crateId) {
                QModelIndex parentIndex = m_childModel.index(0, 0);
                m_childModel.removeRows(i, 1, parentIndex);
                m_crateList.removeAt(i);
                return; // early exit
            }
        }
    }
}

void AutoDJFeature::slotAddRandomTrack() {
    if (m_iAutoDJPlaylistId >= 0) {
        TrackPointer pRandomTrack;
        for (int failedRetrieveAttempts = 0;
                !pRandomTrack &&
                (failedRetrieveAttempts < 2 * kMaxRetrieveAttempts); // 2 rounds
                ++failedRetrieveAttempts) {
            TrackId randomTrackId;
            if (failedRetrieveAttempts < kMaxRetrieveAttempts) {
                // 1st round: from crates
                randomTrackId = m_autoDjCratesDao.getRandomTrackId();
            } else {
                // 2nd round: from whole library
                randomTrackId = m_autoDjCratesDao.getRandomTrackIdFromLibrary(m_iAutoDJPlaylistId);
            }
            if (randomTrackId.isValid()) {
                pRandomTrack = m_pTrackCollection->getTrackDAO().getTrack(randomTrackId);
                VERIFY_OR_DEBUG_ASSERT(pRandomTrack) {
                    qWarning() << "Track does not exist:"
                            << randomTrackId;
                    continue;
                }
                if (!pRandomTrack->exists()) {
                    qWarning() << "Track does not exist:"
                            << pRandomTrack->getInfo()
                            << pRandomTrack->getLocation();
                    pRandomTrack.reset();
                }
            }
        }
        if (pRandomTrack) {
            m_pTrackCollection->getPlaylistDAO().appendTrackToPlaylist(
                    pRandomTrack->getId(), m_iAutoDJPlaylistId);
            m_pAutoDJView->onShow();
            return; // success
        }
    }
    qWarning() << "Could not load random track.";
}

void AutoDJFeature::constructCrateChildModel() {
    m_crateList.clear();
    CrateSelectResult autoDjCrates(m_pTrackCollection->crates().selectAutoDjCrates(true));
    Crate crate;
    while (autoDjCrates.populateNext(&crate)) {
        // Create the TreeItem for this crate.
        m_pCratesTreeItem->appendChild(crate.getName(), crate.getId().toVariant());
        m_crateList.append(crate);
    }
}

void AutoDJFeature::onRightClickChild(const QPoint& globalPos,
                                      QModelIndex index) {
    TreeItem* pClickedItem = static_cast<TreeItem*>(index.internalPointer());
    if (m_pCratesTreeItem == pClickedItem) {
        // The "Crates" parent item was right-clicked.
        // Bring up the context menu.
        QMenu crateMenu;
        crateMenu.setTitle(tr("Add Crate as Track Source"));
        CrateSelectResult nonAutoDjCrates(m_pTrackCollection->crates().selectAutoDjCrates(false));
        Crate crate;
        while (nonAutoDjCrates.populateNext(&crate)) {
            auto pAction = std::make_unique<QAction>(crate.getName(), &crateMenu);
            m_crateMapper.setMapping(pAction.get(), crate.getId().toInt());
            connect(pAction.get(), SIGNAL(triggered()), &m_crateMapper, SLOT(map()));
            crateMenu.addAction(pAction.get());
            pAction.release();
        }
        QMenu contextMenu;
        contextMenu.addMenu(&crateMenu);
        contextMenu.exec(globalPos);
    } else {
        // A crate child item was right-clicked.
        // Bring up the context menu.
        m_pRemoveCrateFromAutoDj->setData(pClickedItem->getData()); // the selected CrateId
        QMenu contextMenu;
        contextMenu.addAction(m_pRemoveCrateFromAutoDj);
        contextMenu.exec(globalPos);
    }
}

void AutoDJFeature::slotRandomQueue(int numTracksToAdd) {
    for (int addCount = 0; addCount < numTracksToAdd; ++addCount) {
        slotAddRandomTrack();
    }
}

void AutoDJFeature::selectionChanged(const QItemSelection&, const QItemSelection&) {
    QPointer<WTrackTableView> pTable = getFocusedTable();
    VERIFY_OR_DEBUG_ASSERT(!m_pAutoDJView.isNull() && !pTable.isNull()) {
        return;
    }
    
    m_pAutoDJView->setSelectedRows(pTable->selectionModel()->selectedRows());
}