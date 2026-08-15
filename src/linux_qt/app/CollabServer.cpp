#include "CollabServer.hpp"

#include "GameFileSystem.hpp"
#include "VmfSync.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <QSocketNotifier>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
#include <QTimer>

#include <cstdio>
#include <memory>
#include <filesystem>

CollabServer::CollabServer(QObject* parent) : QObject(parent)
{
    connect(&session_, &CollabSession::statusMessage, this,
            [this](const QString& message) { log(message); });
    connect(&session_, &CollabSession::chatMessageReceived, this,
            [this](const QString& from, const QString& text) {
        log(QStringLiteral("chat  %1: %2").arg(from, text));
    });
    connect(&session_, &CollabSession::peerListChanged, this, [this] {
        QStringList names;
        for (const auto& [name, id] : session_.connectedPeers()) names << name;
        log(QStringLiteral("peers %1").arg(names.isEmpty() ? QStringLiteral("(none)")
                                                           : names.join(QStringLiteral(", "))));
    });

    // A dead session must not leave a zombie process hosting nothing:
    // exit non-zero so a supervisor (systemd) can restart it.
    connect(&session_, &CollabSession::sessionEnded, this, [this](const QString& reason) {
        log(QStringLiteral("session ended: %1").arg(reason));
        saveIfChanged();
        QCoreApplication::exit(1);
    });

    // 30s: the server's whole job is holding the map, so the crash-loss
    // window stays small. Shutdown saves a final time.
    autosaveTimer_ = new QTimer(this);
    autosaveTimer_->setInterval(30 * 1000);
    connect(autosaveTimer_, &QTimer::timeout, this, [this] { saveIfChanged(); });
}

bool CollabServer::start(const QString& mapPath, quint16 port, const QString& name,
                         const QString& customDir, QString* error)
{
    mapPath_ = mapPath;
    if (!customDir.isEmpty()) {
        auto fileSystem = std::make_shared<hammer::assets::GameFileSystem>();
        if (fileSystem->mountOverrideDirectory(
                std::filesystem::path(customDir.toStdString()))) {
            session_.setSharedFileSystem(std::move(fileSystem));
            log(QStringLiteral("sharing custom content from %1").arg(customDir));
        } else {
            if (error) *error = QStringLiteral("custom dir is not a directory: %1").arg(customDir);
            return false;
        }
    }
    hammer::vmf::Document document;
    if (QFileInfo::exists(mapPath)) {
        hammer::vmf::ParseError parseError;
        std::string ioError;
        auto loaded = hammer::vmf::Document::load(
            std::filesystem::path(mapPath.toStdString()), &parseError, &ioError);
        if (!loaded) {
            if (error) {
                *error = !ioError.empty()
                             ? QString::fromStdString(ioError)
                             : QStringLiteral("VMF parse error at line %1: %2")
                                   .arg(parseError.line)
                                   .arg(QString::fromStdString(parseError.message));
            }
            return false;
        }
        document = std::move(*loaded);
    } else {
        log(QStringLiteral("map %1 does not exist yet; hosting a fresh map").arg(mapPath));
        document = hammer::vmf::Document::createDefault();
    }

    session_.setLocalName(name);
    if (!session_.hostSession(port, document, error)) return false;

    savedHash_ = hammer::vmf::documentSyncHash(session_.baselineDocument());
    autosaveTimer_->start();
    loadBans();

#ifdef Q_OS_UNIX
    // The server console: one command per stdin line. POSIX only — the
    // Windows GUI-subsystem exe has no reliably readable stdin.
    auto* stdinNotifier = new QSocketNotifier(0, QSocketNotifier::Read, this);
    connect(stdinNotifier, &QSocketNotifier::activated, this, [this] {
        // Raw read, not QTextStream: its buffering either drops lines that
        // arrived together (readLine once per event) or blocks the whole
        // event loop probing a still-open pipe (atEnd()). One read() after a
        // readiness event never blocks and returns everything buffered; a
        // partial trailing line waits in consoleBuffer_ for its newline.
        char chunk[4096];
        const ssize_t got = ::read(0, chunk, sizeof(chunk));
        if (got <= 0) return;
        consoleBuffer_.append(chunk, int(got));
        int newline = -1;
        while ((newline = consoleBuffer_.indexOf('\n')) >= 0) {
            const QByteArray line = consoleBuffer_.left(newline);
            consoleBuffer_.remove(0, newline + 1);
            handleConsoleCommand(QString::fromUtf8(line));
        }
    });
#endif

    log(QStringLiteral("serving %1 on UDP port %2 as \"%3\"").arg(mapPath).arg(port).arg(name));
    log(QStringLiteral("console ready — type 'help' for commands"));
    return true;
}

void CollabServer::handleConsoleCommand(const QString& line)
{
    const QStringList parts = line.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return;
    const QString command = parts.first().toLower();
    const QString argument = parts.size() > 1 ? parts.mid(1).join(QLatin1Char(' ')) : QString();

    if (command == QLatin1String("help")) {
        log(QStringLiteral("commands: status | kick <name> | ban <name-or-ip> | "
                           "unban <ip> | bans | save | help"));
        return;
    }
    if (command == QLatin1String("status")) {
        const auto peers = session_.connectedPeers();
        log(QStringLiteral("%1 collaborator(s) connected").arg(peers.size()));
        for (const auto& [name, id] : peers)
            log(QStringLiteral("  %1  %2").arg(name, session_.peerAddress(id)));
        return;
    }
    if (command == QLatin1String("kick")) {
        for (const auto& [name, id] : session_.connectedPeers()) {
            if (name.compare(argument, Qt::CaseInsensitive) == 0) {
                session_.kickPeer(id);
                log(QStringLiteral("kicked %1").arg(name));
                return;
            }
        }
        log(QStringLiteral("no collaborator named \"%1\"").arg(argument));
        return;
    }
    if (command == QLatin1String("ban")) {
        if (argument.isEmpty()) {
            log(QStringLiteral("usage: ban <name-or-ip>"));
            return;
        }
        // A connected name bans (and kicks) that peer's address — and every
        // other peer on it; an unknown argument is treated as an IP literal.
        QString address;
        for (const auto& [name, id] : session_.connectedPeers()) {
            if (name.compare(argument, Qt::CaseInsensitive) == 0) {
                address = session_.peerAddress(id);
                break;
            }
        }
        if (address.isEmpty()) address = argument;
        session_.banAddress(address);
        bannedNames_[address] = argument;
        int kicked = 0;
        for (const auto& [name, id] : session_.connectedPeers()) {
            if (session_.peerAddress(id) == address) {
                session_.kickPeer(id);
                log(QStringLiteral("kicked %1 (%2)").arg(name, address));
                ++kicked;
            }
        }
        saveBans();
        // Bans are per ADDRESS: everyone behind that IP (shared NAT) is out.
        log(QStringLiteral("banned address %1 (%2 peer(s) kicked; bans apply to the "
                           "address, not the name)")
                .arg(address).arg(kicked));
        return;
    }
    if (command == QLatin1String("unban")) {
        if (session_.unbanAddress(argument)) {
            bannedNames_.remove(argument);
            saveBans();
            log(QStringLiteral("unbanned %1").arg(argument));
        } else {
            log(QStringLiteral("%1 was not banned").arg(argument));
        }
        return;
    }
    if (command == QLatin1String("bans")) {
        const QStringList bans = session_.bannedAddresses();
        if (bans.isEmpty()) {
            log(QStringLiteral("no banned addresses"));
            return;
        }
        for (const QString& address : bans) {
            const QString who = bannedNames_.value(address);
            log(who.isEmpty() ? QStringLiteral("  %1").arg(address)
                              : QStringLiteral("  %1  # was: %2").arg(address, who));
        }
        return;
    }
    if (command == QLatin1String("save")) {
        saveIfChanged();
        log(QStringLiteral("save requested"));
        return;
    }
    log(QStringLiteral("unknown command \"%1\" — type 'help'").arg(command));
}

QString CollabServer::bansPath() const
{
    return mapPath_ + QStringLiteral(".bans");
}

void CollabServer::loadBans()
{
    QFile file(bansPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    int count = 0;
    while (!file.atEnd()) {
        // "ip # who-it-was" — the comment remembers what name earned the ban.
        const QString line = QString::fromUtf8(file.readLine());
        const QString address = line.section(QLatin1Char('#'), 0, 0).trimmed();
        const QString who = line.section(QLatin1Char('#'), 1).trimmed();
        if (!address.isEmpty()) {
            session_.banAddress(address);
            if (!who.isEmpty()) bannedNames_[address] = who;
            ++count;
        }
    }
    if (count > 0) log(QStringLiteral("loaded %1 banned address(es) from %2")
                           .arg(count).arg(bansPath()));
}

void CollabServer::saveBans() const
{
    const QStringList bans = session_.bannedAddresses();
    if (bans.isEmpty()) {
        QFile::remove(bansPath());
        return;
    }
    QFile file(bansPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        log(QStringLiteral("could not write %1").arg(bansPath()));
        return;
    }
    for (const QString& address : bans) {
        file.write(address.toUtf8());
        const QString who = bannedNames_.value(address);
        if (!who.isEmpty()) {
            file.write(" # ", 3);
            file.write(who.toUtf8());
        }
        file.write("\n", 1);
    }
}

void CollabServer::saveIfChanged()
{
    if (!session_.active() || mapPath_.isEmpty()) return;
    const std::uint64_t hash = hammer::vmf::documentSyncHash(session_.baselineDocument());
    if (hash == savedHash_) return;
    // The baseline is const; save through a copy (Document::save marks clean).
    hammer::vmf::Document copy = session_.baselineDocument();
    std::string saveError;
    if (copy.save(std::filesystem::path(mapPath_.toStdString()), &saveError)) {
        savedHash_ = hash;
        log(QStringLiteral("saved %1").arg(mapPath_));
    } else {
        log(QStringLiteral("SAVE FAILED for %1: %2")
                .arg(mapPath_, QString::fromStdString(saveError)));
    }
}

void CollabServer::log(const QString& line) const
{
    std::printf("[%s] %s\n",
                qPrintable(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss"))),
                qPrintable(line));
    std::fflush(stdout);
}
