#pragma once

#include "CollabSession.hpp"

#include <QObject>
#include <QHash>
#include <QString>

class QTimer;

// Headless session host (the -server launch option): loads a VMF, hosts a
// collaborative session on it with no GUI at all, and keeps the map file on
// disk current. The CollabSession host role is already authoritative and
// self-contained — it applies every peer's deltas to its baseline and serves
// joins, resyncs, chat and presence relay — so serving needs no document
// widget, just an event loop.
class CollabServer final : public QObject
{
    Q_OBJECT

public:
    explicit CollabServer(QObject* parent = nullptr);

    // Starts hosting `mapPath` on `port`. A missing file starts a fresh
    // default map that will be created at that path by the first save.
    // customDir, when given, is mounted as loose custom content so the
    // server can share the map's custom assets with joiners.
    bool start(const QString& mapPath, quint16 port, const QString& name,
               const QString& customDir, QString* error);

    // Saves the authoritative map back to mapPath if it changed. The autosave
    // timer calls this every 30 seconds; shutdown calls it a last time.
    void saveIfChanged();

    CollabSession& session() { return session_; }

    // One console command line (kick/ban/unban/bans/status/save/help). Fed
    // from stdin on POSIX; public so tests can drive it directly too.
    void handleConsoleCommand(const QString& line);

private:
    void log(const QString& line) const;
    void loadBans();
    void saveBans() const;
    QString bansPath() const;

    CollabSession session_;
    QString mapPath_;
    QTimer* autosaveTimer_{nullptr};
    std::uint64_t savedHash_{0};
    // address -> the name that earned the ban, for the bans listing/file.
    QHash<QString, QString> bannedNames_;
    QByteArray consoleBuffer_;  // partial stdin line awaiting its newline
};
