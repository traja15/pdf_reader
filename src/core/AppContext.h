#pragma once

#include <memory>

class DocumentStore;
class VocabStore;
class Dictionary;
class TamilDictionary;
class GoogleAuth;
class SyncManager;
class QObject;

/// Owns the app's services. Created once in main(); passed to the UI.
class AppContext {
public:
    AppContext();
    ~AppContext();

    DocumentStore *docs;
    VocabStore *vocab;
    GoogleAuth *auth;
    SyncManager *sync;
    std::unique_ptr<Dictionary> dictionary;   ///< Blocking; use from worker threads.
    std::unique_ptr<TamilDictionary> tamil;   ///< Blocking; use from worker threads.

private:
    std::unique_ptr<QObject> m_owner; ///< Parent of the QObject services.
};
