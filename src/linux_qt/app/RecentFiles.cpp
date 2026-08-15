#include "RecentFiles.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <vector>

namespace hammer::app {

namespace {

// GLib's g_bookmark_file parses this store with GMarkup and matches element
// names LITERALLY - it looks for "bookmark:application", not for an element in
// the desktop-bookmarks namespace. Rewriting the file through a namespace-aware
// writer would be free to rename those prefixes and would silently break every
// other application's entries, so the store is parsed into the spec's model and
// written back with these exact strings.
constexpr const char* kBookmarkNamespace =
    "http://www.freedesktop.org/standards/desktop-bookmarks";
constexpr const char* kMimeNamespace =
    "http://www.freedesktop.org/standards/shared-mime-info";
constexpr const char* kFreedesktopOwner = "http://freedesktop.org";
constexpr const char* kGnomeOwner = "http://www.gnome.org";

struct BookmarkApplication
{
    QString name;
    QString exec;
    QString modified;
    // The spec's visit count. GTK sorts and ages entries with it.
    long long count{1};
};

struct Bookmark
{
    QString href;
    QString added;
    QString modified;
    QString visited;
    QString title;
    QString description;
    QString mimeType;
    std::vector<BookmarkApplication> applications;
    QStringList groups;
    bool isPrivate{false};
    // The GNOME-owned metadata block, which only ever carries an icon.
    QString iconHref;
    QString iconType;
    bool hasIcon{false};
};

QString timestamp(const QDateTime& when)
{
    // The spec asks for ISO 8601 in UTC. GLib parses fractional seconds, and
    // every entry GTK writes carries them.
    return when.toUTC().toString(Qt::ISODateWithMs);
}

// Reads one <bookmark> element, leaving the reader on its end element.
Bookmark readBookmark(QXmlStreamReader& reader)
{
    Bookmark bookmark;
    const QXmlStreamAttributes attributes = reader.attributes();
    bookmark.href = attributes.value(QStringLiteral("href")).toString();
    bookmark.added = attributes.value(QStringLiteral("added")).toString();
    bookmark.modified = attributes.value(QStringLiteral("modified")).toString();
    bookmark.visited = attributes.value(QStringLiteral("visited")).toString();

    while (reader.readNextStartElement()) {
        const QStringView name = reader.name();
        if (name == QStringLiteral("title")) {
            bookmark.title = reader.readElementText();
        } else if (name == QStringLiteral("desc")) {
            bookmark.description = reader.readElementText();
        } else if (name == QStringLiteral("info")) {
            while (reader.readNextStartElement()) {
                if (reader.name() != QStringLiteral("metadata")) {
                    reader.skipCurrentElement();
                    continue;
                }
                const QString owner =
                    reader.attributes().value(QStringLiteral("owner")).toString();
                while (reader.readNextStartElement()) {
                    const QStringView metadataName = reader.name();
                    if (owner == QLatin1String(kGnomeOwner) &&
                        metadataName == QStringLiteral("icon")) {
                        bookmark.hasIcon = true;
                        bookmark.iconHref =
                            reader.attributes().value(QStringLiteral("href")).toString();
                        bookmark.iconType =
                            reader.attributes().value(QStringLiteral("type")).toString();
                        reader.skipCurrentElement();
                    } else if (metadataName == QStringLiteral("mime-type")) {
                        bookmark.mimeType =
                            reader.attributes().value(QStringLiteral("type")).toString();
                        reader.skipCurrentElement();
                    } else if (metadataName == QStringLiteral("applications")) {
                        while (reader.readNextStartElement()) {
                            if (reader.name() != QStringLiteral("application")) {
                                reader.skipCurrentElement();
                                continue;
                            }
                            BookmarkApplication application;
                            const QXmlStreamAttributes applicationAttributes = reader.attributes();
                            application.name =
                                applicationAttributes.value(QStringLiteral("name")).toString();
                            application.exec =
                                applicationAttributes.value(QStringLiteral("exec")).toString();
                            application.modified =
                                applicationAttributes.value(QStringLiteral("modified")).toString();
                            application.count = applicationAttributes.value(
                                QStringLiteral("count")).toLongLong();
                            if (application.count <= 0) application.count = 1;
                            bookmark.applications.push_back(std::move(application));
                            reader.skipCurrentElement();
                        }
                    } else if (metadataName == QStringLiteral("groups")) {
                        while (reader.readNextStartElement()) {
                            if (reader.name() == QStringLiteral("group"))
                                bookmark.groups.append(reader.readElementText());
                            else
                                reader.skipCurrentElement();
                        }
                    } else if (metadataName == QStringLiteral("private")) {
                        bookmark.isPrivate = true;
                        reader.skipCurrentElement();
                    } else {
                        reader.skipCurrentElement();
                    }
                }
            }
        } else {
            reader.skipCurrentElement();
        }
    }
    return bookmark;
}

bool readStore(const QString& storePath, std::vector<Bookmark>& bookmarks, QString* error)
{
    QFile file(storePath);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QCoreApplication::translate("RecentFiles", "Could not read %1: %2")
                         .arg(storePath, file.errorString());
        }
        return false;
    }

    QXmlStreamReader reader(&file);
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement) continue;
        if (reader.name() != QStringLiteral("bookmark")) continue;
        bookmarks.push_back(readBookmark(reader));
    }
    if (reader.hasError()) {
        // Never rewrite a store that could not be understood - that would throw
        // away the user's history from every other application.
        if (error) {
            *error = QCoreApplication::translate("RecentFiles", "Could not parse %1: %2")
                         .arg(storePath, reader.errorString());
        }
        return false;
    }
    return true;
}

bool writeStore(const QString& storePath, const std::vector<Bookmark>& bookmarks, QString* error)
{
    QSaveFile file(storePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = QCoreApplication::translate("RecentFiles", "Could not write %1: %2")
                         .arg(storePath, file.errorString());
        }
        return false;
    }

    QXmlStreamWriter writer(&file);
    writer.setAutoFormatting(true);
    writer.setAutoFormattingIndent(2);
    writer.writeStartDocument(QStringLiteral("1.0"));
    writer.writeStartElement(QStringLiteral("xbel"));
    writer.writeAttribute(QStringLiteral("version"), QStringLiteral("1.0"));
    writer.writeAttribute(QStringLiteral("xmlns:bookmark"), QString::fromLatin1(kBookmarkNamespace));
    writer.writeAttribute(QStringLiteral("xmlns:mime"), QString::fromLatin1(kMimeNamespace));

    for (const Bookmark& bookmark : bookmarks) {
        if (bookmark.href.isEmpty()) continue;
        writer.writeStartElement(QStringLiteral("bookmark"));
        writer.writeAttribute(QStringLiteral("href"), bookmark.href);
        if (!bookmark.added.isEmpty())
            writer.writeAttribute(QStringLiteral("added"), bookmark.added);
        if (!bookmark.modified.isEmpty())
            writer.writeAttribute(QStringLiteral("modified"), bookmark.modified);
        if (!bookmark.visited.isEmpty())
            writer.writeAttribute(QStringLiteral("visited"), bookmark.visited);
        if (!bookmark.title.isEmpty())
            writer.writeTextElement(QStringLiteral("title"), bookmark.title);
        if (!bookmark.description.isEmpty())
            writer.writeTextElement(QStringLiteral("desc"), bookmark.description);

        writer.writeStartElement(QStringLiteral("info"));
        writer.writeStartElement(QStringLiteral("metadata"));
        writer.writeAttribute(QStringLiteral("owner"), QString::fromLatin1(kFreedesktopOwner));
        if (!bookmark.mimeType.isEmpty()) {
            writer.writeStartElement(QStringLiteral("mime:mime-type"));
            writer.writeAttribute(QStringLiteral("type"), bookmark.mimeType);
            writer.writeEndElement();
        }
        if (!bookmark.applications.empty()) {
            writer.writeStartElement(QStringLiteral("bookmark:applications"));
            for (const BookmarkApplication& application : bookmark.applications) {
                writer.writeStartElement(QStringLiteral("bookmark:application"));
                writer.writeAttribute(QStringLiteral("name"), application.name);
                writer.writeAttribute(QStringLiteral("exec"), application.exec);
                writer.writeAttribute(QStringLiteral("modified"), application.modified);
                writer.writeAttribute(QStringLiteral("count"),
                                      QString::number(application.count));
                writer.writeEndElement();
            }
            writer.writeEndElement();
        }
        if (!bookmark.groups.isEmpty()) {
            writer.writeStartElement(QStringLiteral("bookmark:groups"));
            for (const QString& group : bookmark.groups)
                writer.writeTextElement(QStringLiteral("bookmark:group"), group);
            writer.writeEndElement();
        }
        if (bookmark.isPrivate) {
            writer.writeEmptyElement(QStringLiteral("bookmark:private"));
        }
        writer.writeEndElement(); // metadata
        if (bookmark.hasIcon) {
            writer.writeStartElement(QStringLiteral("metadata"));
            writer.writeAttribute(QStringLiteral("owner"), QString::fromLatin1(kGnomeOwner));
            writer.writeStartElement(QStringLiteral("bookmark:icon"));
            writer.writeAttribute(QStringLiteral("href"), bookmark.iconHref);
            if (!bookmark.iconType.isEmpty())
                writer.writeAttribute(QStringLiteral("type"), bookmark.iconType);
            writer.writeEndElement();
            writer.writeEndElement();
        }
        writer.writeEndElement(); // info
        writer.writeEndElement(); // bookmark
    }

    writer.writeEndElement(); // xbel
    writer.writeEndDocument();

    if (!file.commit()) {
        if (error) {
            *error = QCoreApplication::translate("RecentFiles", "Could not write %1: %2")
                         .arg(storePath, file.errorString());
        }
        return false;
    }
    return true;
}

// $XDG_DATA_HOME/recently-used.xbel, creating the directory if needed. Empty on
// failure, with "error" filled.
QString storeLocation(QString* error)
{
    const QString dataDirectory =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (dataDirectory.isEmpty()) {
        if (error) {
            *error = QCoreApplication::translate(
                "RecentFiles", "No writable data directory for the recent-files store");
        }
        return {};
    }
    if (!QDir().mkpath(dataDirectory)) {
        if (error) {
            *error = QCoreApplication::translate("RecentFiles", "Could not create %1")
                         .arg(dataDirectory);
        }
        return {};
    }
    return QDir(dataDirectory).filePath(QStringLiteral("recently-used.xbel"));
}

QDateTime parseTimestamp(const QString& text)
{
    // GLib writes microseconds, this file writes milliseconds, and both are
    // valid ISO 8601. Qt's parser takes either.
    return QDateTime::fromString(text, Qt::ISODateWithMs);
}

} // namespace

bool registerRecentlyUsedFile(const QString& path, QString* error)
{
    // A stale message from a previous call must not read as this call's result.
    if (error) error->clear();
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists() || !info.isFile()) {
        // Never fail silently: a caller that hands over a path the store cannot
        // take has a bug worth seeing, and "the map is missing from Recent" is
        // otherwise indistinguishable from the feature not running at all.
        if (error) {
            *error = QCoreApplication::translate("RecentFiles", "%1 is not an existing file")
                         .arg(path.isEmpty() ? QStringLiteral("(no path)") : path);
        }
        return false;
    }

    const QString storePath = storeLocation(error);
    if (storePath.isEmpty()) return false;

    std::vector<Bookmark> bookmarks;
    if (!readStore(storePath, bookmarks, error)) return false;

    const QString href =
        QString::fromUtf8(QUrl::fromLocalFile(info.absoluteFilePath()).toEncoded());
    const QString now = timestamp(QDateTime::currentDateTimeUtc());

    // The name a file manager attributes the visit to, and the command it would
    // re-open the file with. GTK stores the exec quoted, with a %u placeholder.
    const QString applicationName = QCoreApplication::applicationName();
    const QString applicationExec =
        QStringLiteral("'%1 %u'").arg(QFileInfo(QCoreApplication::applicationFilePath()).fileName());

    const auto existing = std::find_if(bookmarks.begin(), bookmarks.end(),
                                       [&href](const Bookmark& bookmark) {
                                           return bookmark.href == href;
                                       });

    Bookmark bookmark;
    if (existing != bookmarks.end()) {
        bookmark = *existing;
        bookmarks.erase(existing);
    } else {
        bookmark.href = href;
        bookmark.added = now;
    }
    if (bookmark.added.isEmpty()) bookmark.added = now;
    bookmark.modified = now;
    bookmark.visited = now;
    if (bookmark.mimeType.isEmpty()) {
        const QMimeType mimeType = QMimeDatabase().mimeTypeForFile(info);
        bookmark.mimeType = mimeType.isValid() ? mimeType.name()
                                               : QStringLiteral("application/octet-stream");
    }

    const auto application = std::find_if(bookmark.applications.begin(),
                                          bookmark.applications.end(),
                                          [&applicationName](const BookmarkApplication& entry) {
                                              return entry.name == applicationName;
                                          });
    if (application != bookmark.applications.end()) {
        ++application->count;
        application->modified = now;
        application->exec = applicationExec;
    } else {
        BookmarkApplication entry;
        entry.name = applicationName;
        entry.exec = applicationExec;
        entry.modified = now;
        entry.count = 1;
        bookmark.applications.push_back(std::move(entry));
    }

    // Most recent last, which is the order GTK itself writes.
    bookmarks.push_back(std::move(bookmark));
    return writeStore(storePath, bookmarks, error);
}

std::vector<RecentlyUsedFile> recentlyUsedFiles(int limit, QString* error)
{
    if (error) error->clear();
    std::vector<RecentlyUsedFile> recent;
    if (limit <= 0) return recent;

    const QString storePath = storeLocation(error);
    if (storePath.isEmpty()) return recent;

    std::vector<Bookmark> bookmarks;
    if (!readStore(storePath, bookmarks, error)) return recent;

    const QString applicationName = QCoreApplication::applicationName();
    for (const Bookmark& bookmark : bookmarks) {
        const auto application = std::find_if(bookmark.applications.begin(),
                                              bookmark.applications.end(),
                                              [&applicationName](const BookmarkApplication& entry) {
                                                  return entry.name == applicationName;
                                              });
        if (application == bookmark.applications.end()) continue;

        const QUrl url(bookmark.href);
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) continue;

        // The bookmark's "visited" is the honest answer when this application
        // wrote the entry, but GTK - which registers the file when its own
        // chooser hands it over - only ever bumps the per-application
        // "modified". Taking the later of the two keeps both kinds of visit in
        // one ordering instead of pinning GTK-registered maps to the day they
        // were first opened.
        const QDateTime visited = parseTimestamp(bookmark.visited);
        const QDateTime touched = parseTimestamp(application->modified);
        RecentlyUsedFile entry;
        entry.path = info.absoluteFilePath();
        entry.visited = (visited.isValid() && (!touched.isValid() || visited > touched))
                            ? visited
                            : touched;
        entry.count = application->count;
        recent.push_back(std::move(entry));
    }

    // Newest first. An entry with no readable timestamp sorts last rather than
    // to the top, which is where a default-constructed QDateTime would land it.
    //
    // Two maps opened inside the same millisecond tie on the timestamp, so
    // position in the store breaks it: both this writer and GTK append the
    // entry they just touched, which makes a later position the more recent
    // visit. Reversing the run first turns std::stable_sort's "keep the earlier
    // element" rule into exactly that.
    std::reverse(recent.begin(), recent.end());
    std::stable_sort(recent.begin(), recent.end(),
                     [](const RecentlyUsedFile& left, const RecentlyUsedFile& right) {
                         if (left.visited.isValid() != right.visited.isValid())
                             return left.visited.isValid();
                         return left.visited > right.visited;
                     });
    if (recent.size() > static_cast<std::size_t>(limit)) recent.resize(limit);
    return recent;
}

} // namespace hammer::app
