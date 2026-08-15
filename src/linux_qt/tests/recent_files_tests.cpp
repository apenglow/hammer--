// Registration of opened maps with the XDG desktop-bookmark store, which is
// what a file manager's "Recent" tab and a GTK file dialog's "Recently Used"
// both read. Runs against a scratch XDG_DATA_HOME - it must never touch the
// developer's own history.
#include "RecentFiles.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// An excerpt of a real store, including another application's entry that must
// survive untouched.
const char* ExistingStore =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<xbel version=\"1.0\"\n"
    "      xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\"\n"
    "      xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-info\"\n"
    ">\n"
    "  <bookmark href=\"file:///home/someone/Documents/song.sf2\" added=\"2026-06-13T03:30:50.068849Z\""
    " modified=\"2026-07-09T05:38:31.085084Z\" visited=\"2026-06-13T03:30:50.068849Z\">\n"
    "    <info>\n"
    "      <metadata owner=\"http://freedesktop.org\">\n"
    "        <mime:mime-type type=\"application/octet-stream\"/>\n"
    "        <bookmark:applications>\n"
    "          <bookmark:application name=\"Ardour\" exec=\"&apos;ardour-8.12.0 %u&apos;\""
    " modified=\"2026-07-09T05:38:31.085082Z\" count=\"2\"/>\n"
    "        </bookmark:applications>\n"
    "      </metadata>\n"
    "    </info>\n"
    "  </bookmark>\n"
    "</xbel>\n";

QString readAll(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("hammerminusminus"));

    QTemporaryDir scratch;
    require(scratch.isValid(), "temporary directory created");
    const QString dataHome = QDir(scratch.path()).filePath(QStringLiteral("data"));
    require(QDir().mkpath(dataHome), "scratch XDG_DATA_HOME created");
    qputenv("XDG_DATA_HOME", dataHome.toUtf8());

    const QString mapPath = QDir(scratch.path()).filePath(QStringLiteral("test map.vmf"));
    {
        QFile map(mapPath);
        require(map.open(QIODevice::WriteOnly), "test map written");
        map.write("world\n{\n\"id\" \"1\"\n}\n");
    }
    const QString storePath = QDir(dataHome).filePath(QStringLiteral("recently-used.xbel"));

    // A store that does not exist yet is created.
    QString error;
    require(hammer::app::registerRecentlyUsedFile(mapPath, &error), "first registration succeeds");
    require(error.isEmpty(), "first registration reports no error");
    QString store = readAll(storePath);
    require(!store.isEmpty(), "store written");
    // A space in the path has to be percent-encoded in the href.
    require(store.contains(QStringLiteral("test%20map.vmf")), "href is percent-encoded");
    require(store.contains(QStringLiteral("count=\"1\"")), "first visit counts once");
    // GLib matches these element names literally, so the prefixes are fixed.
    require(store.contains(QStringLiteral("<bookmark:application ")), "literal bookmark: prefix");
    require(store.contains(QStringLiteral("<mime:mime-type ")), "literal mime: prefix");
    require(store.contains(QStringLiteral("name=\"hammerminusminus\"")), "application recorded");

    // Opening the same map again bumps the visit count rather than duplicating.
    require(hammer::app::registerRecentlyUsedFile(mapPath, &error), "second registration succeeds");
    store = readAll(storePath);
    require(store.count(QStringLiteral("test%20map.vmf")) == 1, "no duplicate bookmark");
    require(store.contains(QStringLiteral("count=\"2\"")), "second visit increments the count");

    // Another application's entry survives a rewrite unchanged.
    {
        QFile file(storePath);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "fixture store written");
        file.write(ExistingStore);
    }
    require(hammer::app::registerRecentlyUsedFile(mapPath, &error), "registration into a store");
    store = readAll(storePath);
    require(store.contains(QStringLiteral("file:///home/someone/Documents/song.sf2")),
            "existing bookmark kept");
    require(store.contains(QStringLiteral("name=\"Ardour\"")), "existing application kept");
    require(store.contains(QStringLiteral("count=\"2\"")), "existing visit count kept");
    require(store.contains(QStringLiteral("added=\"2026-06-13T03:30:50.068849Z\"")),
            "existing timestamps kept");
    require(store.contains(QStringLiteral("application/octet-stream")),
            "existing mime type kept");
    require(store.contains(QStringLiteral("test%20map.vmf")), "new bookmark added alongside");

    // A store that cannot be parsed is never rewritten: that would destroy the
    // user's history from every other application.
    {
        QFile file(storePath);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "damaged store written");
        file.write("<xbel><bookmark href=\"file:///a\"></xbel");
    }
    const QString damaged = readAll(storePath);
    error.clear();
    require(!hammer::app::registerRecentlyUsedFile(mapPath, &error),
            "registration fails on an unparseable store");
    require(!error.isEmpty(), "the failure is reported");
    require(readAll(storePath) == damaged, "the unparseable store is left alone");

    // A path that is not a file is not registered.
    require(!hammer::app::registerRecentlyUsedFile(
                QDir(scratch.path()).filePath(QStringLiteral("missing.vmf")), &error),
            "a missing file is not registered");

    require(!error.isEmpty(), "the missing file is reported rather than failing silently");

    // Reading back what the Recent Files menu shows.
    {
        QFile file(storePath);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "store reset");
        file.write(ExistingStore);
    }
    const QString secondMapPath = QDir(scratch.path()).filePath(QStringLiteral("second.vmf"));
    {
        QFile map(secondMapPath);
        require(map.open(QIODevice::WriteOnly), "second test map written");
        map.write("world\n{\n\"id\" \"1\"\n}\n");
    }
    require(hammer::app::registerRecentlyUsedFile(mapPath, &error), "first map registered");
    require(hammer::app::registerRecentlyUsedFile(secondMapPath, &error), "second map registered");

    std::vector<hammer::app::RecentlyUsedFile> recent = hammer::app::recentlyUsedFiles(10, &error);
    require(error.isEmpty(), "reading the store reports no error");
    // Only this application's entries: the fixture's Ardour bookmark is not ours.
    require(recent.size() == 2, "both maps are listed and nothing else");
    require(recent[0].path == QFileInfo(secondMapPath).absoluteFilePath(),
            "most recently visited comes first");
    require(recent[1].path == QFileInfo(mapPath).absoluteFilePath(), "older map comes second");
    require(recent[0].visited.isValid(), "the visit timestamp round-trips");
    require(recent[0].visited >= recent[1].visited, "entries are ordered newest first");

    require(hammer::app::recentlyUsedFiles(1, &error).size() == 1, "the limit is honoured");
    require(hammer::app::recentlyUsedFiles(0, &error).empty(), "a zero limit lists nothing");

    // A map that has been deleted since is not offered.
    require(QFile::remove(secondMapPath), "second map removed");
    recent = hammer::app::recentlyUsedFiles(10, &error);
    require(recent.size() == 1, "a deleted map drops out of the list");
    require(recent[0].path == QFileInfo(mapPath).absoluteFilePath(), "the surviving map remains");

    std::cout << "recent files tests passed\n";
    return EXIT_SUCCESS;
}
