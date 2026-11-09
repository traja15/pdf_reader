# PenReader for Windows & Linux

A C++/Qt 6 version of the PenReader Android app. It syncs with the Android app through the same hidden Google Drive folder, so PDFs, handwritten notes, reading positions and My Words follow you between your phone, Linux and Windows.

## Features

- **Tabs:** every PDF opens in its own tab. **Home** is always the first tab. Opening a PDF that's already open switches to its tab. Your open tabs come back when you restart. Shortcuts: Ctrl+Tab / Ctrl+Shift+Tab switch, Ctrl+W closes, Alt+Home goes Home.
- **Catalogue folders (on Home):** organise PDFs into folders and subfolders (Books → Physics). Folders and each PDF's folder sync to all your devices, including Android.
  - The folder tree on the left shows **All PDFs**, **Unfiled** and your folders, with counts.
  - Drag PDFs onto a folder (select several with Ctrl/Shift), or right-click → **Move to**.
  - Right-click a folder to create a subfolder, rename, move or delete it. Deleting a folder never deletes PDFs; its contents move up a level.
  - A PDF you open while a folder is selected is filed into that folder.
  - **Search all PDFs** finds titles anywhere and shows where each one is filed.
  - Vim keys: `j`/`k` move, `l`/Enter open, `h` goes up a folder.
- **Recently learnt words** sit next to the catalogue; click one to see its definition.
- **PDF reading:** continuous scroll, zoom (Ctrl + wheel, touchpad or touch pinch), rotate, full screen, search, link following, table of contents, and vim-style keyboard control (see below).
- **Resume:** each PDF reopens at the exact spot you left it. Positions sync both ways with Android.
- **Pen notes:** pen (pressure-sensitive), highlighter and eraser, with undo/redo.
  - **Pen tablets** (Wacom, Windows Ink pens, Galaxy Book S Pen, …): write with pressure. The **eraser end** or a held **barrel button** erases, like the S Pen side button on Android.
  - **Mouse:** draws with the current tool. Turn off "Mouse draws" (M) to let only the pen draw, so the mouse drag pans.
  - **Touch:** one finger scrolls, two fingers zoom.
- **Dictionary with English and தமிழ் tabs:** right-click or double-click a word, or use the dictionary tool (D). You get pronunciation audio, examples, synonyms and Tamil meanings. The sources match Android: dictionaryapi.dev with a Wiktionary fallback for English, and Wiktionary plus the bundled offline word list for Tamil. Lookups are cached, so they work offline afterwards.
- **My Words** (Ctrl+M): saved words with English definition, Tamil meaning, the sentence it came from, and the document and page. You can search (in English or Tamil), sort, and jump back to the page.
- **Library** (on Home): open PDFs, or drop several at once (each gets a tab). PDFs opened on another device appear here and download on open. You can remove a download or delete a PDF from all devices.

## Keyboard (vim / zathura style)

Vim-style keys are on by default. Turn them off, or change any shortcut, in **File → Settings… → Keyboard** (`Ctrl+,`). **F1** shows every shortcut with its current keys.

| Keys | Action |
|---|---|
| `j` / `k` / `h` / `l` (or arrows) | Scroll down / up / left / right |
| `Ctrl+D` / `Ctrl+U` | Half a screen down / up |
| `Space` / `Shift+Space` (or PgDn/PgUp) | One screen down / up |
| `J` / `K` | Next / previous page |
| `gg` / `G` | First / last page |
| `:` or `Ctrl+G` | Go to page… |
| `/` / `?` | Search forward / backward (matches highlight as you type) |
| `n` / `N` | Next / previous match |
| `f` | Follow a link: letter hints appear on every link; type one to follow it |
| `Ctrl+O` / `Ctrl+I` | Jump back / forward (after links, searches, go-to-page, marks) |
| `m` + letter / `'` + letter | Set a mark / jump to it (saved per document) |
| `+` / `-` / `=` | Zoom in / out / fit width |
| `r` | Rotate 90° |
| `u` / `Ctrl+R` | Undo / redo |
| `1`–`4`, `5` | Pen, highlighter, eraser, dictionary tool; mouse-draws toggle |
| `o` | Open PDF |
| `gt` / `gT` / `gh` | Next tab / previous tab / Home |
| `F9` | Table of contents |
| `F11`, `Esc` | Full screen; Esc leaves full screen, closes search, cancels hints |

When a vim key collides with a standard one, the vim meaning wins (for example `Ctrl+R` is redo, so rotate uses `r`). With vim keys off, the usual desktop shortcuts apply (`Ctrl+F` search, `F3` next match, `Ctrl+R` rotate, `Ctrl+Z` undo, …). On Home, `j` / `k` move through the library and words lists and `l` or Enter opens.

**Settings also cover:** scroll step, link-hint letters, restoring tabs at startup, the dictionary's first tab, the auto-sync interval, and sync-before-close.

## Build on Linux (Arch)

```sh
sudo pacman -S --needed cmake ninja qt6-base qt6-webengine qt6-multimedia qt6-svg noto-fonts
cd desktop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/penreader            # or: ./build/penreader some.pdf
sudo cmake --install build   # optional: installs it plus an "Open with PenReader" entry
```

The Qt PDF module ships inside `qt6-webengine` on Arch. Other distros call the packages `qt6-pdf-dev`, `libqt6pdf6`, or similar.

## Build on Windows

1. Install **Visual Studio 2022** (the "Desktop development with C++" workload) and **CMake**.
2. Install **Qt 6.8 or newer** with the Qt Online Installer. Select *MSVC 2022 64-bit*, and under *Additional Libraries* tick **Qt PDF** and **Qt Multimedia**.
3. In the "x64 Native Tools Command Prompt for VS 2022":
   ```bat
   cd desktop
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
   cmake --build build
   C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release build\penreader.exe
   build\penreader.exe
   ```
   `windeployqt` copies the Qt DLLs next to the exe. Zip the `build` folder (minus the CMake files) to move it to another PC.

Windows Ink pens send pressure and the eraser through Qt with no extra drivers. Tamil text uses Windows' built-in Nirmala UI font.

## Google sign-in (one-time)

The desktop app needs its own OAuth client, **in the same Google Cloud project as the Android app**, because Google gives each project one shared hidden folder.

1. Go to Google Cloud Console → **APIs & Services → Credentials → Create credentials → OAuth client ID**.
2. For **Application type**, choose **Desktop app**, then download its JSON.
3. In PenReader, use **File → Google client file…**, pick that JSON, then **File → Sign in with Google…**. Your default browser opens Google's sign-in page; approve, then return to the app. A small window shows the sign-in link with a **Copy link** button, so if no browser opens you can paste the link into one yourself. On Linux the browser is launched with `xdg-open` (your default browser), because Qt's desktop-portal route silently does nothing on plain window managers such as dwm or i3.

The client file is stored as `google_oauth_client.json` in the app's config folder (`~/.config/PenReader/` or `%LOCALAPPDATA%\PenReader\PenReader\`). For a portable setup you can also put it next to `penreader.exe`.

Your sign-in (a refresh token) is kept in the app's settings file. While the consent screen is in **Testing** mode, Google expires desktop sign-ins after 7 days, so you'll be asked to sign in again. Publishing the consent screen avoids this.

## Sync details

Sync runs:
- at launch,
- every 5 minutes,
- after you open a PDF or save or remove a word,
- when you go back to the library,
- before the app closes (it waits up to 15 s).

The status bar shows the result. The cloud layout (`documents.json`, `folders.json`, `vocab.json`, `ink_*.json`, `pdf_*.pdf`) and the merge rules (last write wins per document, folder, page and word) are identical to the Android app's `SyncManager`.

## Self-test

`penreader_selftest` checks Android ink-format compatibility, the English and Tamil dictionaries (needs internet), PDF word picking, and saves screenshots. Point it at a throwaway data folder:

```sh
T=$PWD/build/testhome; mkdir -p $T
XDG_DATA_HOME=$T/data XDG_CONFIG_HOME=$T/config QT_QPA_PLATFORM=offscreen \
  ./build/penreader_selftest some.pdf $T
```

## Code map

| Path | What |
|---|---|
| `src/ui/ReaderView.cpp` | Pages, zoom, rotation, tablet/mouse/touch input, ink, word picking |
| `src/ui/PageItem.cpp` | One page: bitmap and stroke drawing |
| `src/ui/MainWindow.cpp` | Tabs, toolbar, menus, search bar, contents, opening/downloading, sync status |
| `src/ui/Commands.cpp` | Keyboard command registry (default and vim keys, user overrides) |
| `src/ui/SettingsDialog.cpp` | Settings page |
| `src/ui/DefinitionPanel.cpp` | English/Tamil dictionary panel, Save to My Words |
| `src/net/SyncManager.cpp` | Two-way sync, format-compatible with Android |
| `src/net/GoogleAuth.cpp` | Desktop OAuth (PKCE plus loopback redirect) |
| `src/net/Dictionary.cpp`, `TamilDictionary.cpp` | Dictionaries |
| `src/core/` | SQLite storage (same tables as the Android app) |
