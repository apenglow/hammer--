// Headless -server mode, end to end against the REAL application binary:
// spawns `hammerminusminus -server`, joins it as a client, edits, chats, then
// terminates the server and checks the edit was saved back into the VMF.
//
//   hammer-collab-server-tests <path-to-hammerminusminus>

#include "CollabSession.hpp"
#include "GameFileSystem.hpp"
#include "VmfDocument.hpp"
#include "VmfSync.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <iostream>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using hammer::vmf::Block;
using hammer::vmf::Document;

namespace {

QProcess* serverForCleanup = nullptr;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        if (serverForCleanup) serverForCleanup->kill();
        std::exit(EXIT_FAILURE);
    }
}

const char* SampleVmf =
    "world\n"
    "{\n"
    "\t\"id\" \"1\"\n"
    "\t\"classname\" \"worldspawn\"\n"
    "\tsolid\n"
    "\t{\n"
    "\t\t\"id\" \"2\"\n"
    "\t\tside { \"id\" \"3\" \"material\" \"CUSTOM/SERVED\" }\n"
    "\t}\n"
    "}\n";

template <typename Predicate>
bool waitFor(const Predicate& done, int timeoutMs = 20000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: collab-server-tests <hammerminusminus binary>\n";
        return 2;
    }
    const QString binary = QString::fromLocal8Bit(argv[1]);

    QTemporaryDir scratch;
    require(scratch.isValid(), "scratch dir creates");
    const QString mapPath = scratch.filePath(QStringLiteral("served.vmf"));
    {
        QFile map(mapPath);
        require(map.open(QIODevice::WriteOnly), "map writes");
        map.write(SampleVmf);
    }

#ifdef Q_OS_UNIX
    const quint16 port = quint16(47400 + (getpid() % 100));
#else
    const quint16 port = 47411;
#endif

    // Custom content the server should offer to joiners.
    QDir(scratch.path()).mkpath(QStringLiteral("assets/materials/custom"));
    {
        QFile vmt(scratch.filePath(QStringLiteral("assets/materials/custom/served.vmt")));
        require(vmt.open(QIODevice::WriteOnly), "asset vmt writes");
        vmt.write("\"LightmappedGeneric\"\n{\n\t\"$basetexture\" \"custom/served\"\n}\n");
    }
    {
        QFile vtf(scratch.filePath(QStringLiteral("assets/materials/custom/served.vtf")));
        require(vtf.open(QIODevice::WriteOnly), "asset vtf writes");
        vtf.write("SERVED-VTF");
    }

    QProcess server;
    serverForCleanup = &server;
    server.setProcessChannelMode(QProcess::MergedChannels);
    server.start(binary, {QStringLiteral("-server"), mapPath, QStringLiteral("-port"),
                          QString::number(port), QStringLiteral("-name"),
                          QStringLiteral("Dedicated"), QStringLiteral("-customdir"),
                          scratch.filePath(QStringLiteral("assets"))});
    require(server.waitForStarted(10000), "server process starts");
    QByteArray serverLog;
    QObject::connect(&server, &QProcess::readyReadStandardOutput,
                     [&] { serverLog += server.readAllStandardOutput(); });
    require(waitFor([&] { return serverLog.contains("serving"); }),
            "server reports it is hosting");

    // Join as a real client.
    CollabSession client;
    client.setLocalName(QStringLiteral("Alice"));
    Document received;
    bool booted = false;
    QObject::connect(&client, &CollabSession::bootstrapDocument,
                     [&](const Document& document, int, int) {
                         received = document;
                         booted = true;
                     });
    QTemporaryDir downloads;
    client.setSharedFileSystem(std::make_shared<hammer::assets::GameFileSystem>());
    client.setAssetDownloadDirectory(downloads.path());
    int offered = 0;
    QObject::connect(&client, &CollabSession::assetsOffered,
                     [&](int count, qint64) { offered = count; });
    int downloaded = 0;
    QObject::connect(&client, &CollabSession::assetsDownloaded,
                     [&](int count, const QString&) { downloaded = count; });
    QString error;
    require(client.joinSession(QStringLiteral("127.0.0.1"), port, &error), "client connects");
    require(waitFor([&] { return booted; }), "client receives the served map");
    require(waitFor([&] { return offered == 2; }), "headless server offers its custom assets");
    client.acceptAssetOffer();
    require(waitFor([&] { return downloaded == 2; }), "assets download from the headless server");
    require(hammer::vmf::documentSyncHash(received) ==
                hammer::vmf::documentSyncHash(*Document::parse(SampleVmf)),
            "served map matches the file");

    // An edit reaches the server's authoritative copy.
    Document edited = received;
    Block& lamp = edited.appendRoot("entity");
    lamp.setValue("id", "2000001");
    lamp.setValue("classname", "light");
    lamp.setValue("origin", "32 32 32");
    client.localDocumentChanged(edited);

    // Chat lands in the server log with the stamped name.
    client.sendChat(QStringLiteral("hello server"));
    require(waitFor([&] { return serverLog.contains("Alice: hello server"); }),
            "chat reaches the server log");

    // Console: status lists Alice with her address.
    const auto command = [&](const char* text) {
        server.write(text);
        server.write("\n");
        server.waitForBytesWritten(2000);
    };
    command("status");
    require(waitFor([&] { return serverLog.contains("Alice  127.0.0.1"); }),
            "console status lists the peer and address");

    // Banning by name kicks Alice and blocks rejoins from that address.
    QString aliceEnd;
    QObject::connect(&client, &CollabSession::sessionEnded,
                     [&](const QString& reason) { aliceEnd = reason; });
    command("ban Alice");
    require(waitFor([&] { return !aliceEnd.isEmpty(); }), "banned peer is kicked");
    require(waitFor([&] { return serverLog.contains("banned address 127.0.0.1"); }),
            "console reports the ban");
    require(QFile::exists(mapPath + QStringLiteral(".bans")), "ban list persisted next to map");

    CollabSession blocked;
    blocked.setLocalName(QStringLiteral("Mallory"));
    bool blockedBooted = false;
    QObject::connect(&blocked, &CollabSession::bootstrapDocument,
                     [&](const Document&, int, int) { blockedBooted = true; });
    require(blocked.joinSession(QStringLiteral("127.0.0.1"), port, &error),
            "banned address can still dial");
    require(waitFor([&] { return serverLog.contains("Rejected banned address 127.0.0.1"); }),
            "server rejects the banned address at accept");
    require(!blockedBooted, "banned address never receives the map");
    // The rejected client must get a terminal signal, not hang at
    // "Connecting..." forever (GNS connect timeout -> ProblemDetectedLocally).
    QString blockedEnd;
    QObject::connect(&blocked, &CollabSession::sessionEnded,
                     [&](const QString& reason) { blockedEnd = reason; });
    require(waitFor([&] { return !blockedEnd.isEmpty(); }, 30000),
            "banned client's join fails with a terminal error");
    blocked.leave();

    // Unban lets a new join through again.
    server.write("bans\nunban 127.0.0.1\n");  // one write, two commands
    server.waitForBytesWritten(2000);
    require(waitFor([&] { return serverLog.contains("# was: Alice"); }),
            "bans listing remembers who earned the ban");
    require(waitFor([&] { return serverLog.contains("unbanned 127.0.0.1"); }),
            "second command in the same write also executes");
    CollabSession returning;
    returning.setLocalName(QStringLiteral("Carol"));
    bool carolBooted = false;
    QObject::connect(&returning, &CollabSession::bootstrapDocument,
                     [&](const Document&, int, int) { carolBooted = true; });
    require(returning.joinSession(QStringLiteral("127.0.0.1"), port, &error),
            "unbanned address dials");
    require(waitFor([&] { return carolBooted; }), "unbanned address joins again");
    returning.leave();
    require(!QFile::exists(mapPath + QStringLiteral(".bans")),
            "empty ban list removes the file");

    // Orderly shutdown must save the edit back into the map file.
    client.leave();
    server.terminate();
    require(server.waitForFinished(15000), "server exits on SIGTERM");
    const auto saved = Document::load(std::filesystem::path(mapPath.toStdString()));
    require(saved.has_value(), "saved map parses");
    bool hasLamp = false;
    for (const Block& root : saved->roots()) {
        const std::string* id = root.value("id");
        if (root.name == "entity" && id && *id == "2000001") hasLamp = true;
    }
    require(hasLamp, "the client's edit was saved to disk on shutdown");

    std::cout << "collab server tests passed\n";
    return 0;
}
