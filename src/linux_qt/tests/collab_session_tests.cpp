#include "CollabAssets.hpp"
#include "CollabSession.hpp"
#include "GameFileSystem.hpp"
#include "VmfDocument.hpp"
#include "VmfSync.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QElapsedTimer>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <cstdlib>
#include <iostream>
#include <string>

using hammer::vmf::Block;
using hammer::vmf::Document;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
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
    "\t\tside { \"id\" \"3\" \"material\" \"BRICK/BRICKWALL001A\" }\n"
    "\t}\n"
    "}\n";

Block* worldSolid(Document& document, const std::string& id)
{
    Block* world = document.firstRoot("world");
    if (!world) return nullptr;
    for (Block* solid : world->children("solid")) {
        const std::string* value = solid->value("id");
        if (value && *value == id) return solid;
    }
    return nullptr;
}

// Spins the event loop until `done` reports true.
// Default generous: the full ctest run shares the CPU with other suites and
// a tight deadline here has produced spurious failures under load.
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

    const Document sample = *Document::parse(SampleVmf);

    CollabSession host;
    host.setLocalName(QStringLiteral("Hosty"));
    QString error;
    // Port 0: let the OS pick a free one... CollabSession takes the port as
    // given, so probe a range instead.
    quint16 port = 0;
    for (quint16 candidate = 47311; candidate < 47341; ++candidate) {
        if (host.hostSession(candidate, sample, &error)) {
            port = candidate;
            break;
        }
        host.leave();
    }
    require(port != 0, "host finds a free port");

    // The joining editor.
    CollabSession client;
    client.setLocalName(QStringLiteral("Alice"));
    Document clientDocument;
    int clientIdBase = 0;
    QObject::connect(&client, &CollabSession::bootstrapDocument,
                     [&](const Document& document, int idBase, int) {
                         clientDocument = document;
                         clientIdBase = idBase;
                     });
    // A hostname rather than an IP literal: exercises the DNS resolution path.
    require(client.joinSession(QStringLiteral("localhost"), port, &error), "client connects");
    require(waitFor([&] { return clientIdBase != 0; }), "client receives the map");
    require(clientIdBase == 2 * CollabSession::kIdSpan, "first joiner gets the second id window");
    require(hammer::vmf::diffDocuments(clientDocument, sample).empty(),
            "bootstrap document matches the host's");

    // Client edits a solid; the host should see exactly that delta.
    hammer::vmf::SyncDelta hostSeen;
    QObject::connect(&host, &CollabSession::remoteDelta,
                     [&](const hammer::vmf::SyncDelta& delta) { hostSeen = delta; });
    Document edited = clientDocument;
    edited.firstRoot("world")->children("solid")[0]->children("side")[0]->setValue(
        "material", "METAL/METALWALL001");
    client.localDocumentChanged(edited);
    require(waitFor([&] { return !hostSeen.empty(); }), "host receives the client's delta");
    require(hostSeen.upserts.size() == 1 && hostSeen.upserts[0].kind == "solid" &&
                hostSeen.upserts[0].key == "2",
            "delta carries the edited solid");

    // Host edits an entity; the client should converge.
    hammer::vmf::SyncDelta clientSeen;
    QObject::connect(&client, &CollabSession::remoteDelta,
                     [&](const hammer::vmf::SyncDelta& delta) { clientSeen = delta; });
    Document hostEdited = edited;  // the host's live document now includes the client's edit
    Block& light = hostEdited.appendRoot("entity");
    light.setValue("id", "1000001");
    light.setValue("classname", "light");
    host.localDocumentChanged(hostEdited);
    require(waitFor([&] { return !clientSeen.empty(); }), "client receives the host's delta");
    hammer::vmf::applyDelta(edited, clientSeen);
    require(hammer::vmf::diffDocuments(edited, hostEdited).empty(),
            "both editors converge on the same map");

    // A delta larger than GNS's 512K single-message cap must arrive whole
    // through the fragmentation path. Pseudo-random hex defeats qCompress, so
    // the wire payload really does exceed the cap.
    clientSeen = {};
    Document bigEdit = hostEdited;
    Block& archive = bigEdit.appendRoot("entity");
    archive.setValue("id", "1000002");
    archive.setValue("classname", "logic_case");
    std::string blob;
    blob.reserve(2 * 1024 * 1024);
    std::uint64_t state = 0x243F6A8885A308D3ull;
    for (std::size_t i = 0; i < 2 * 1024 * 1024; ++i) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        blob.push_back("0123456789abcdef"[(state >> 60) & 15]);
    }
    archive.setValue("blob", blob);
    host.localDocumentChanged(bigEdit);
    require(waitFor([&] { return !clientSeen.empty(); }, 30000),
            "client receives an oversized fragmented delta");
    hammer::vmf::applyDelta(edited, clientSeen);
    require(hammer::vmf::diffDocuments(edited, bigEdit).empty(),
            "oversized delta converges intact");

    // A big real map: the wire body (compressed + base64, inside JSON) far
    // exceeds any send-buffer cap, so the transfer only works if chunks that
    // the transport rejects (buffer full) are queued and retried instead of
    // silently dropped, and if the send rate is not left at GNS's 256KB/s
    // default. This joiner must receive the whole thing.
    clientSeen = {};
    Document bigMap = bigEdit;
    Block& world2 = *bigMap.firstRoot("world");
    std::string bigBlob;
    const std::size_t bigSize = 64 * 1024 * 1024;
    bigBlob.reserve(bigSize);
    for (std::size_t i = 0; i < bigSize; ++i) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        bigBlob.push_back("0123456789abcdef"[(state >> 60) & 15]);
    }
    world2.setValue("comment", bigBlob);
    host.localDocumentChanged(bigMap);
    // Generous deadline: under a parallel ctest run the whole suite shares
    // the CPU with compiles/links, and this transfer compresses and moves
    // ~40MB; a tight deadline made the test flaky under load.
    require(waitFor([&] { return !clientSeen.empty(); }, 180000),
            "client receives a huge (64MB source) delta");
    hammer::vmf::applyDelta(edited, clientSeen);
    require(hammer::vmf::diffDocuments(edited, bigMap).empty(), "huge delta converges intact");

    // Desync detection: both peers edit the SAME solid before either pumps.
    // Per-object last-writer-wins then picks opposite winners (each side
    // applies its own edit first and the other's second), which is exactly
    // the divergence the hash check exists to catch. The client's sync check
    // must detect the split and rebuild from the host's copy.
    Document hostSide = host.baselineDocument();
    worldSolid(hostSide, "2")->children("side")[0]->setValue("material", "HOST/WINS");
    Document clientSide = client.baselineDocument();
    worldSolid(clientSide, "2")->children("side")[0]->setValue("material", "CLIENT/WINS");
    host.localDocumentChanged(hostSide);
    client.localDocumentChanged(clientSide);
    require(waitFor([&] {
                return *worldSolid(const_cast<Document&>(host.baselineDocument()), "2")
                            ->children("side")[0]
                            ->value("material") == "CLIENT/WINS";
            }),
            "host applied the client's conflicting edit");
    require(hammer::vmf::documentSyncHash(host.baselineDocument()) !=
                hammer::vmf::documentSyncHash(client.baselineDocument()),
            "conflicting same-object edits desynced the peers");

    bool resynced = false;
    QObject::connect(&client, &CollabSession::resyncDocument,
                     [&](const Document&) { resynced = true; });
    client.requestSyncCheck();
    require(waitFor([&] { return resynced; }), "sync check triggers a resync");
    require(hammer::vmf::documentSyncHash(host.baselineDocument()) ==
                hammer::vmf::documentSyncHash(client.baselineDocument()),
            "resync converges the peers on the host's copy");
    require(*worldSolid(const_cast<Document&>(client.baselineDocument()), "2")
                 ->children("side")[0]
                 ->value("material") == "CLIENT/WINS",
            "the host's state (client edit won there) is authoritative");

    // A matching pair of baselines must NOT resync.
    resynced = false;
    client.requestSyncCheck();
    waitFor([] { return false; }, 2000);  // pump for the round trip
    require(!resynced, "no spurious resync when hashes match");

    // Asset path sanitizer: the manifest and every request/response path come
    // from an untrusted peer.
    require(hammer::collab::isSafeAssetPath("materials/custom/wall.vmt"), "plain path is safe");
    require(hammer::collab::isSafeAssetPath("models/props/crate.mdl"), "model path is safe");
    require(!hammer::collab::isSafeAssetPath("../../.ssh/authorized_keys"), "dotdot rejected");
    require(!hammer::collab::isSafeAssetPath("materials/../models/x.mdl"),
            "embedded dotdot rejected");
    require(!hammer::collab::isSafeAssetPath("/etc/passwd"), "absolute rejected");
    require(!hammer::collab::isSafeAssetPath("c:/windows/system32/evil.dll"),
            "drive letter rejected");
    require(!hammer::collab::isSafeAssetPath("materials\\custom\\wall.vmt"),
            "backslashes rejected");
    require(!hammer::collab::isSafeAssetPath("maps/secret.vmf"), "outside asset roots rejected");
    require(!hammer::collab::isSafeAssetPath(""), "empty rejected");

    // Custom asset sharing end to end: the host mounts a folder of loose
    // custom content its map uses; a fresh joiner without those files gets an
    // offer, accepts, and the files land in its download folder intact.
    QTemporaryDir hostAssetsDir;
    QTemporaryDir clientDownloadDir;
    require(hostAssetsDir.isValid() && clientDownloadDir.isValid(), "temp dirs create");
    const QDir hostAssets(hostAssetsDir.path());
    QDir().mkpath(hostAssets.filePath("materials/custom"));
    const QByteArray vmtBytes =
        "\"LightmappedGeneric\"\n{\n\t\"$basetexture\" \"custom/wall\"\n}\n";
    const QByteArray vtfBytes = "VTF-BYTES-STAND-IN";
    {
        QFile vmt(hostAssets.filePath("materials/custom/wall.vmt"));
        require(vmt.open(QIODevice::WriteOnly), "write host vmt");
        vmt.write(vmtBytes);
    }
    {
        QFile vtf(hostAssets.filePath("materials/custom/wall.vtf"));
        require(vtf.open(QIODevice::WriteOnly), "write host vtf");
        vtf.write(vtfBytes);
    }
    auto hostFs = std::make_shared<hammer::assets::GameFileSystem>();
    require(hostFs->mountOverrideDirectory(
                std::filesystem::path(hostAssetsDir.path().toStdString())),
            "host mounts custom content");
    host.setSharedFileSystem(hostFs);

    // Make the shared map actually use the custom material.
    Document withCustom = host.baselineDocument();
    worldSolid(withCustom, "2")->children("side")[0]->setValue("material", "CUSTOM/WALL");
    host.localDocumentChanged(withCustom);

    require(waitFor([&] {
                Document snapshot = host.baselineDocument();
                Block* solid = worldSolid(snapshot, "2");
                return solid && *solid->children("side")[0]->value("material") == "CUSTOM/WALL";
            }),
            "host baseline carries the custom material");
    require(hammer::collab::collectCustomAssetPaths(host.baselineDocument(), *hostFs).size() == 2,
            "collector finds the vmt and vtf");

    CollabSession client2;
    client2.setLocalName(QStringLiteral("  Bob\tthe builder whose name is far too long to fit  "));
    client2.setSharedFileSystem(std::make_shared<hammer::assets::GameFileSystem>());
    client2.setAssetDownloadDirectory(clientDownloadDir.path());
    int offeredCount = 0;
    QObject::connect(&client2, &CollabSession::assetsOffered,
                     [&](int count, qint64) { offeredCount = count; });
    int downloadedCount = 0;
    QObject::connect(&client2, &CollabSession::assetsDownloaded,
                     [&](int count, const QString&) { downloadedCount = count; });
    bool client2Booted = false;
    QObject::connect(&client2, &CollabSession::bootstrapDocument,
                     [&](const Document&, int, int) { client2Booted = true; });
    require(client2.joinSession(QStringLiteral("127.0.0.1"), port, &error), "second joiner connects");
    require(waitFor([&] { return client2Booted; }, 120000), "second joiner receives the map");
    require(waitFor([&] { return offeredCount != 0; }, 30000),
            "joiner is offered the custom assets");
    require(offeredCount == 2, "offer covers the vmt and its texture");
    client2.acceptAssetOffer();
    require(waitFor([&] { return downloadedCount != 0; }), "accepted assets download");
    require(downloadedCount == 2, "both files downloaded");
    {
        QFile got(QDir(clientDownloadDir.path()).filePath("materials/custom/wall.vmt"));
        require(got.open(QIODevice::ReadOnly) && got.readAll() == vmtBytes,
                "downloaded vmt matches the host's");
    }
    {
        QFile got(QDir(clientDownloadDir.path()).filePath("materials/custom/wall.vtf"));
        require(got.open(QIODevice::ReadOnly) && got.readAll() == vtfBytes,
                "downloaded vtf matches the host's");
    }
    // Names: the host sanitized and recorded both joiners.
    {
        const auto peers = host.connectedPeers();
        require(peers.size() == 2, "host sees two collaborators");
        require(peers[0].first == QStringLiteral("Alice"), "first joiner name kept");
        require(peers[1].first.startsWith(QStringLiteral("Bob the builder")) &&
                    peers[1].first.size() <= 24,
                "second joiner name sanitized and capped");
    }

    // Presence: poses fan out through the host at the broadcast tick, keyed
    // by the host-stamped id, and never include the receiver's own.
    QList<CollabPeerPose> hostPoses;
    QObject::connect(&host, &CollabSession::peerPosesChanged,
                     [&](const QList<CollabPeerPose>& poses) { hostPoses = poses; });
    QList<CollabPeerPose> client2Poses;
    QObject::connect(&client2, &CollabSession::peerPosesChanged,
                     [&](const QList<CollabPeerPose>& poses) { client2Poses = poses; });
    host.updateLocalPose(1.0, 2.0, 3.0, -10.0, 90.0);
    client.updateLocalPose(100.0, 200.0, 300.0, 5.0, 45.0);
    require(waitFor([&] {
                return std::any_of(hostPoses.begin(), hostPoses.end(),
                                   [](const CollabPeerPose& pose) {
                                       return pose.name == QStringLiteral("Alice") &&
                                              pose.id == 2 * CollabSession::kIdSpan &&
                                              pose.x == 100.0 && pose.yaw == 45.0;
                                   });
            }),
            "host aggregates the client's pose under its id");
    require(waitFor([&] {
                const bool hasAlice = std::any_of(
                    client2Poses.begin(), client2Poses.end(),
                    [](const CollabPeerPose& pose) { return pose.name == "Alice"; });
                const bool hasHost = std::any_of(
                    client2Poses.begin(), client2Poses.end(),
                    [](const CollabPeerPose& pose) { return pose.name == "Hosty"; });
                const bool hasSelf = std::any_of(
                    client2Poses.begin(), client2Poses.end(),
                    [&](const CollabPeerPose& pose) { return pose.id == client2.localId(); });
                return hasAlice && hasHost && !hasSelf;
            }),
            "other peers see host and Alice but never themselves");

    // Kick: Bob gets the explicit notice, drops from membership, and the
    // remaining client still receives deltas afterwards (callback routing
    // must survive membership churn).
    QString client2End;
    QObject::connect(&client2, &CollabSession::sessionEnded,
                     [&](const QString& reason) { client2End = reason; });
    const auto peersBeforeKick = host.connectedPeers();
    host.kickPeer(peersBeforeKick[1].second);
    require(waitFor([&] { return !client2End.isEmpty(); }), "kicked client learns about it");
    require(client2End.contains(QStringLiteral("removed")), "kick reason names the kick");
    require(host.connectedPeers().size() == 1, "host membership shrinks on kick");

    clientSeen = {};
    Document afterKick = host.baselineDocument();
    Block& lamp = afterKick.appendRoot("entity");
    lamp.setValue("id", "1000900");
    lamp.setValue("classname", "light_environment");
    host.localDocumentChanged(afterKick);
    require(waitFor([&] { return !clientSeen.empty(); }),
            "remaining client still receives deltas after the kick");

    // Chat: relays through the host with host-stamped names both directions,
    // sanitized, with a local echo for the sender.
    QStringList clientChat;
    QObject::connect(&client, &CollabSession::chatMessageReceived,
                     [&](const QString& from, const QString& text) {
                         clientChat << from + QStringLiteral("|") + text;
                     });
    QStringList hostChat;
    QObject::connect(&host, &CollabSession::chatMessageReceived,
                     [&](const QString& from, const QString& text) {
                         hostChat << from + QStringLiteral("|") + text;
                     });
    client.sendChat(QStringLiteral("  hello \t there  "));
    require(waitFor([&] { return hostChat.contains(QStringLiteral("Alice|hello there")); }),
            "host receives the client's chat, sanitized and name-stamped");
    require(clientChat.contains(QStringLiteral("Alice|hello there")),
            "sender gets a local echo");
    host.sendChat(QStringLiteral("welcome"));
    require(waitFor([&] { return clientChat.contains(QStringLiteral("Hosty|welcome")); }),
            "client receives the host's chat");
    client.sendChat(QStringLiteral("   "));
    require(!hostChat.contains(QStringLiteral("Alice|")), "blank chat is dropped");

    client.leave();
    host.leave();
    std::cout << "collab session tests passed\n";
    return 0;
}
