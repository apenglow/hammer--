#pragma once

#include <QDateTime>
#include <QString>

#include <vector>

namespace hammer::app {

// --- XDG "Recently Used" registration ---------------------------------------
//
// The Recent list a file manager or a GTK file dialog shows is not something
// the desktop derives by watching processes: an application has to add the file
// itself, by appending to the desktop-bookmark store at
// $XDG_DATA_HOME/recently-used.xbel. Qt has no API for this, so opening a map
// left no trace in Files' "Recent" tab.
//
// This writes that store the way the freedesktop Desktop Bookmark Spec (and
// GLib's g_bookmark_file, which is what actually parses it) expects: the
// bookmark's own timestamps, plus a per-application record whose visit count
// increases each time the same file is opened again.
//
// Returns false and fills "error" when the store exists but could not be read
// or written. A store that is missing entirely is created.
bool registerRecentlyUsedFile(const QString& path, QString* error = nullptr);

// One map this application opened before, as the store remembers it.
struct RecentlyUsedFile
{
    QString path;
    // The bookmark's own visit time. This is the timestamp GVfs publishes as
    // "recent::modified" and the one GTK's Recent views sort on - not the
    // file's atime, which a relatime mount will not bother to update.
    QDateTime visited;
    // How many times this application has opened the file.
    long long count{1};
};

// The files this application registered, most recently visited first. Entries
// whose file has since been deleted or moved are left out: a Recent menu that
// offers a map that is no longer there is worse than a shorter menu.
//
// Reads the same store registerRecentlyUsedFile() writes, so the desktop's
// Recent list and the application's own stay one list rather than two that
// drift apart. Returns an empty list (and fills "error") if the store cannot
// be read; a missing store is simply empty.
std::vector<RecentlyUsedFile> recentlyUsedFiles(int limit, QString* error = nullptr);

} // namespace hammer::app
