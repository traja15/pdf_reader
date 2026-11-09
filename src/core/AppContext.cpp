#include "core/AppContext.h"

#include "core/Database.h"
#include "core/DocumentStore.h"
#include "core/VocabStore.h"
#include "net/Dictionary.h"
#include "net/GoogleAuth.h"
#include "net/SyncManager.h"
#include "net/TamilDictionary.h"

#include <QDir>
#include <QStandardPaths>

AppContext::AppContext() : m_owner(std::make_unique<QObject>())
{
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    Database::init(dataDir + "/penreader.db");

    docs = new DocumentStore(dataDir + "/documents", m_owner.get());
    vocab = new VocabStore(m_owner.get());
    auth = new GoogleAuth(m_owner.get());
    sync = new SyncManager(auth, docs, vocab, m_owner.get());
    dictionary = std::make_unique<Dictionary>();
    tamil = std::make_unique<TamilDictionary>();

    // Changes other devices should see are pushed a few seconds later.
    QObject::connect(docs, &DocumentStore::needsSync, sync, &SyncManager::requestSync);
    QObject::connect(vocab, &VocabStore::needsSync, sync, &SyncManager::requestSync);
}

AppContext::~AppContext() = default;
