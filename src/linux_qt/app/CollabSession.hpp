#pragma once

#include "VmfDocument.hpp"
#include "VmfSync.hpp"

#include <QObject>
#include <QElapsedTimer>
#include <QSet>
#include <QString>

#include <cstdint>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

class QTimer;

namespace hammer::assets { class GameFileSystem; }

// One collaborator's camera, as shown in other editors' 3D views. Identity is
// the id window base the host assigned (never the display name, which can
// repeat); angles are Source QAngle degrees.
struct CollabPeerPose
{
    int id{0};
    QString name;
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double pitch{0.0};
    double yaw{0.0};
};

// One collaborative editing session over Valve GameNetworkingSockets: one
// peer hosts, others connect to its address (peer-to-peer in the
// no-central-server sense). Connections are encrypted and reliable-ordered
// over UDP; messages arrive whole, so there is no stream framing here. The
// host is authoritative for ordering: every edit travels as an object-level
// SyncDelta (VmfSync), clients send theirs to the host, and the host applies
// and rebroadcasts. Each peer allocates object ids from its own window of the
// id space (kIdSpan apart) so concurrent creation never collides.
//
// The open-source GNS build does no NAT traversal (that is Steam
// infrastructure): joiners need a route to the host's UDP port, i.e. LAN,
// VPN, or a forwarded port.
class CollabSession final : public QObject
{
    Q_OBJECT

public:
    enum class Role { None, Host, Client };

    // Peer 0 is the host; joiner N gets window [(N+1)*kIdSpan, (N+2)*kIdSpan).
    static constexpr int kIdSpan = 1'000'000;
    static constexpr quint16 kDefaultPort = 26900;

    explicit CollabSession(QObject* parent = nullptr);
    ~CollabSession() override;

    Role role() const { return role_; }
    bool active() const { return role_ != Role::None; }
    int peerCount() const;

    // Starts listening and adopts `current` as the shared baseline.
    bool hostSession(quint16 port, const hammer::vmf::Document& current, QString* error);
    bool joinSession(const QString& address, quint16 port, QString* error);
    void leave();

    // Call after every committed local edit with the live document. Diffs it
    // against the session baseline; a non-empty delta advances the baseline
    // and goes out on the wire. Cheap when nothing changed.
    void localDocumentChanged(const hammer::vmf::Document& current);

    // Desync detection (client role): asks the host for its baseline hash and
    // compares at reply time. On a confirmed mismatch the client requests a
    // full document from the host, which arrives as resyncDocument. A timer
    // runs this periodically during quiet spells; tests call it directly.
    void requestSyncCheck();

    // Test access: the shared baseline this peer believes the session is at.
    const hammer::vmf::Document& baselineDocument() const { return baseline_; }

    // Custom asset sharing. The host advertises the map's loose custom files
    // (materials/models a joiner cannot get from the stock game) after each
    // join; a joiner that is missing some gets assetsOffered and answers with
    // accept or decline. Both sides need the file system; the joiner also
    // needs a directory to download into (a dedicated custom folder — files
    // are never written into the user's own content).
    void setSharedFileSystem(std::shared_ptr<hammer::assets::GameFileSystem> fileSystem);
    void setAssetDownloadDirectory(const QString& directory);
    void acceptAssetOffer();
    void declineAssetOffer();

    // Display name carried in the hello and shown on this peer's avatar.
    // Set before hosting/joining. The host sanitizes every name it receives.
    void setLocalName(const QString& name);
    // This peer's id window base: the host's own is kIdSpan; a client learns
    // its own from the welcome. Used to filter one's own avatar out.
    int localId() const { return localId_; }

    // Host only: the connected collaborators, as (display name, id base).
    std::vector<std::pair<QString, int>> connectedPeers() const;
    // Host only: removes a collaborator. They receive a "kicked" notice
    // (reliable, flushed before the close) and their avatar drops instantly.
    void kickPeer(int idBase);
    // Host only: the remote IP (no port) of a connected collaborator.
    QString peerAddress(int idBase) const;
    // Host only: IP bans, enforced when a connection arrives — banned
    // addresses are rejected before the accept. Bans do not kick by
    // themselves; callers kick the matching peers.
    void banAddress(const QString& address);
    bool unbanAddress(const QString& address);
    QStringList bannedAddresses() const;

    // The local editor camera, polled by the UI at a low rate. Fans out to
    // the other peers over the UNRELIABLE channel: a lost pose is replaced by
    // the next tick, and reliable delivery would queue poses behind map
    // transfers.
    void updateLocalPose(double x, double y, double z, double pitchDegrees, double yawDegrees);

    // Session chat. Messages relay through the host, which stamps the
    // sender's name itself (a peer cannot speak as someone else). The sender
    // gets its own message back through chatMessageReceived for a uniform
    // local echo.
    void sendChat(const QString& text);

signals:
    // Client only: the host's full document on join. The receiver should
    // adopt it and set the id range it is handed.
    void bootstrapDocument(const hammer::vmf::Document& document, int idBase, int idSpan);
    // A peer's edit, already folded into the session baseline. Apply it to
    // the live document (EditorModel::applyExternalEdit).
    void remoteDelta(const hammer::vmf::SyncDelta& delta);
    // The scene desynced and the host sent a fresh full document (already the
    // new baseline). The receiver should adopt it wholesale (full rebuild).
    void resyncDocument(const hammer::vmf::Document& document);
    void statusMessage(const QString& message);
    // Client: the host shares custom assets this peer is missing. Ask the
    // user, then acceptAssetOffer()/declineAssetOffer().
    void assetsOffered(int fileCount, qint64 totalBytes);
    // Client: an accepted offer finished downloading into `directory`.
    void assetsDownloaded(int fileCount, const QString& directory);
    // Everyone: the collaborators' cameras (never includes the receiver's
    // own). Arrives at the pose broadcast rate (~4 Hz).
    void peerPosesChanged(const QList<CollabPeerPose>& poses);
    // Host: membership changed (join/leave/kick); names for the kick picker.
    void peerListChanged();
    // A chat line from anyone in the session, the local editor included.
    void chatMessageReceived(const QString& from, const QString& text);
    void peerCountChanged(int count);
    // The session is over (host gone, network error). leave() was already run.
    void sessionEnded(const QString& reason);

private:
    struct Peer
    {
        std::uint32_t connection{0};  // HSteamNetConnection
        int idBase{0};
        bool welcomed{false};
        QByteArray assembly;  // partially received fragmented message
        std::deque<QByteArray> outgoing;  // chunks awaiting send-buffer room
        QString name;
        bool hasPose{false};
        CollabPeerPose pose;
    };

    // GNS delivers connection state changes through a static callback during
    // the pump; these route them to the owning session.
    friend struct CollabSessionGnsBridge;
    void onConnectionStatusChanged(const void* info);
    bool ownsConnection(std::uint32_t connection) const;

    void pump();
    void handleHostMessage(Peer& peer, const QByteArray& body);
    void sendAssetManifest(Peer& peer);
    void maybeOfferNewAssets(const hammer::vmf::SyncDelta& delta);
    void handleClientMessage(const QByteArray& body);
    void dropPeer(std::uint32_t connection, bool announce);
    void broadcast(const QByteArray& body, std::uint32_t except);
    void send(std::uint32_t connection, const QByteArray& body);
    void sendUnreliable(std::uint32_t connection, const QByteArray& body);
    void broadcastPoses();
    void flushOutgoing(std::uint32_t connection, std::deque<QByteArray>& queue);
    void announcePeerCount();

    Role role_{Role::None};
    QTimer* pumpTimer_{nullptr};
    std::uint32_t listenSocket_{0};  // host: HSteamListenSocket
    std::uint32_t pollGroup_{0};     // host: HSteamNetPollGroup
    std::uint32_t hostConnection_{0};  // client: the connection to the host
    std::vector<Peer> peers_;          // host role
    QByteArray hostAssembly_;          // client: partially received message
    std::deque<QByteArray> hostOutgoing_;  // client: chunks awaiting room
    hammer::vmf::Document baseline_;
    int nextPeerIndex_{1};
    bool leaving_{false};
    bool pumping_{false};
    // Client-side desync-check state. sendCount_ ticks per outgoing delta so
    // a hash round crossed by a local edit can be discarded instead of
    // misread as a desync.
    QTimer* syncCheckTimer_{nullptr};
    bool hashRoundActive_{false};
    bool resyncPending_{false};
    quint64 sendCount_{0};
    quint64 sendsAtHashq_{0};
    QElapsedTimer lastTraffic_;
    // Asset sharing state.
    std::shared_ptr<hammer::assets::GameFileSystem> sharedFileSystem_;
    QString assetDownloadDirectory_;
    QSet<QString> bannedAddresses_;           // host: rejected at accept time
    std::set<std::string> servableAssets_;    // host: advertised, may be served
    // Asset names already probed on disk. The scan is filesystem-bound, so an
    // edit that introduces no new reference must not pay for it.
    std::set<std::string> probedAssetRefs_;
    std::vector<std::string> offeredAssets_;  // client: missing, awaiting the user
    std::set<std::string> expectedAssets_;    // client: accepted, awaiting data
    int downloadedAssets_{0};
    // Presence.
    QString localName_;
    int localId_{0};
    CollabPeerPose localPose_;
    bool localPoseValid_{false};
    QElapsedTimer lastPoseSent_;
    bool posePending_{false};  // client: movement swallowed by the rate limit
    QTimer* poseTimer_{nullptr};  // host: aggregate broadcast tick
};
