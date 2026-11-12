cmake_minimum_required(VERSION 3.21)
project(PenReaderDesktop VERSION 0.1.0 LANGUAGES CXX)

# Optimised build unless asked otherwise (a Debug build is ~3x slower at recolouring pages).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network Sql Pdf Concurrent Svg Multimedia)
qt_standard_project_setup()

# Everything except main() lives in a library shared by the app and the self-test.
qt_add_library(penreader_core STATIC
    src/core/Models.h
    src/core/InkCodec.h src/core/InkCodec.cpp
    src/core/Database.h src/core/Database.cpp
    src/core/DocumentStore.h src/core/DocumentStore.cpp
    src/core/VocabStore.h src/core/VocabStore.cpp
    src/core/AppContext.h src/core/AppContext.cpp
    src/core/OpenUrl.h src/core/OpenUrl.cpp
    src/core/SearchIndex.h src/core/SearchIndex.cpp
    src/core/TextSearch.h src/core/TextSearch.cpp
    src/net/Http.h src/net/Http.cpp
    src/net/Dictionary.h src/net/Dictionary.cpp
    src/net/TamilDictionary.h src/net/TamilDictionary.cpp
    src/net/Translator.h src/net/Translator.cpp
    src/ui/TranslationDialog.h src/ui/TranslationDialog.cpp
    src/ui/HomeDelegates.h src/ui/HomeDelegates.cpp
    src/ui/MouseScheme.h
    src/net/GoogleAuth.h src/net/GoogleAuth.cpp
    src/net/DriveApi.h src/net/DriveApi.cpp
    src/net/SyncManager.h src/net/SyncManager.cpp
    src/net/Ollama.h src/net/Ollama.cpp
    src/ui/MainWindow.h src/ui/MainWindow.cpp
    src/ui/LibraryView.h src/ui/LibraryView.cpp
    src/ui/ReaderView.h src/ui/ReaderView.cpp
    src/ui/PageItem.h src/ui/PageItem.cpp
    src/ui/ToolSettings.h
    src/ui/Prefs.h
    src/ui/Commands.h src/ui/Commands.cpp
    src/ui/SettingsDialog.h src/ui/SettingsDialog.cpp
    src/ui/SmartSearchView.h src/ui/SmartSearchView.cpp
    src/ui/Thumbnails.h src/ui/Thumbnails.cpp
    src/ui/Theme.h src/ui/Theme.cpp
    src/ui/DefinitionPanel.h src/ui/DefinitionPanel.cpp
    src/ui/VocabWindow.h src/ui/VocabWindow.cpp
)
target_include_directories(penreader_core PUBLIC src)
target_link_libraries(penreader_core PUBLIC
    Qt6::Widgets Qt6::Network Qt6::Sql Qt6::Pdf Qt6::Concurrent Qt6::Svg Qt6::Multimedia)

# The English -> Tamil offline index is shared with the Android app.
set(EN_TA_INDEX ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/assets/en_ta.tsv)
set_source_files_properties(${EN_TA_INDEX} PROPERTIES QT_RESOURCE_ALIAS en_ta.tsv)
qt_add_resources(penreader_core "data" PREFIX "/data" FILES ${EN_TA_INDEX})

file(GLOB ICONS CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} resources/icons/*.svg)
qt_add_resources(penreader_core "icons" PREFIX "/" FILES ${ICONS})
qt_add_resources(penreader_core "chrome" PREFIX "/" FILES resources/close-light.svg resources/close-dark.svg)

# Optional: build your Google "Desktop app" OAuth client into the program, so people you give it to
# can sign in without loading a file:  cmake -DPENREADER_GOOGLE_CLIENT=/path/to/client.json …
# The JSON stays out of the source code; only builds you make with this option contain it.
set(PENREADER_GOOGLE_CLIENT "" CACHE FILEPATH "Google Desktop OAuth client JSON to embed (optional)")
if(PENREADER_GOOGLE_CLIENT)
    if(NOT EXISTS "${PENREADER_GOOGLE_CLIENT}")
        message(FATAL_ERROR "PENREADER_GOOGLE_CLIENT: file not found: ${PENREADER_GOOGLE_CLIENT}")
    endif()
    file(READ "${PENREADER_GOOGLE_CLIENT}" _client_json)
    string(FIND "${_client_json}" "\"installed\"" _installed)
    if(_installed EQUAL -1)
        message(FATAL_ERROR "PENREADER_GOOGLE_CLIENT must be a \"Desktop app\" OAuth client JSON (with an \"installed\" section)")
    endif()
    configure_file("${PENREADER_GOOGLE_CLIENT}" "${CMAKE_CURRENT_BINARY_DIR}/embedded/google_oauth_client.json" COPYONLY)
    qt_add_resources(penreader_core "google_client" PREFIX "/google" BASE "${CMAKE_CURRENT_BINARY_DIR}/embedded"
        FILES "${CMAKE_CURRENT_BINARY_DIR}/embedded/google_oauth_client.json")
    message(STATUS "Embedding Google OAuth client from ${PENREADER_GOOGLE_CLIENT}")
endif()

qt_add_executable(penreader WIN32 src/main.cpp)
target_link_libraries(penreader PRIVATE penreader_core)

if(MSVC)
    target_compile_options(penreader_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(penreader_core PRIVATE -Wall -Wextra)
endif()

option(PENREADER_SELFTEST "Build the self-test program" ON)
if(PENREADER_SELFTEST)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(penreader_selftest tests/selftest.cpp)
    target_link_libraries(penreader_selftest PRIVATE penreader_core Qt6::Test)
    qt_add_executable(penreader_xprobe tests/xprobe.cpp)
    target_link_libraries(penreader_xprobe PRIVATE penreader_core)
    qt_add_executable(penreader_bench tests/bench.cpp)
    target_link_libraries(penreader_bench PRIVATE penreader_core Qt6::Test)
    qt_add_executable(penreader_screenshot tests/screenshot.cpp)
    target_link_libraries(penreader_screenshot PRIVATE penreader_core Qt6::Test)
endif()

install(TARGETS penreader RUNTIME DESTINATION bin)
if(UNIX AND NOT APPLE)
    # Lets file managers offer "Open with PenReader" for PDFs.
    install(FILES resources/penreader.desktop DESTINATION share/applications)
    install(FILES resources/icons/app.svg DESTINATION share/icons/hicolor/scalable/apps RENAME penreader.svg)
endif()
