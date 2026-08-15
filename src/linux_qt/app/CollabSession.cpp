#include "CollabSession.hpp"

#include "CollabAssets.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTimer>

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <algorithm>
#include <vector>

namespace {

// Messages are single JSON objects; GNS preserves message boundaries, so
// there is no length framing. Document and delta payloads ride inside as
// base64 of qCompress'ed VMF text.
QByteArray encodeMessage(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString packPayload(const std::string& text)
{
    const QByteArray raw(text.data(), qsizetype(text.size()));
    return QString::fromLatin1(qCompress(raw, 6).toBase64());
}

std::string unpackPayload(const QJsonObject& object)
{
    const QByteArray compressed = QByteArray::fromBase64(object.value("p").toString().toLatin1());
    const QByteArray raw = qUncompress(compressed);
    return std::string(raw.constData(), std::size_t(raw.size()));
}

// GNS is initialized once per process and left up: sessions come and go
// (the tests run two in one process) and re-init/kill cycles buy nothing.
bool ensureGnsInitialized(QString* error)
{
    static bool initialized = false;
    static QString initError;
    if (!initialized && initError.isEmpty()) {
        SteamDatagramErrMsg message;
        if (GameNetworkingSockets_Init(nullptr, message)) {
            initialized = true;
            // GNS's defaults are tuned for game state, not map transfers: a
            // 512K send buffer, a receive buffer sized to match, and a send
            // rate clamp of 256KB/s would make a large map's welcome payload
            // take minutes or never complete. The buffers are caps, not
            // allocations, so large values cost nothing until used.
            ISteamNetworkingUtils* utils = SteamNetworkingUtils();
            constexpr int32 kBufferCap = 512 * 1024 * 1024;
            constexpr int32 kSendRate = 512 * 1024 * 1024;  // bytes/sec
            utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendBufferSize, kBufferCap);
            utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_RecvBufferSize, kBufferCap);
            utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_RecvBufferMessages, 65536);
            utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMin, kSendRate);
            utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMax, kSendRate);
        } else {
            initError = QString::fromUtf8(message);
        }
    }
    if (!initialized && error) *error = initError;
    return initialized;
}

// The status-changed callback is a bare function pointer with no user data,
// so live sessions register here and the callback routes by connection /
// listen-socket ownership.
std::vector<CollabSession*>& liveSessions()
{
    static std::vector<CollabSession*> sessions;
    return sessions;
}

QString sanitizeChat(QString text)
{
    text = text.simplified();
    text.removeIf([](QChar c) { return c.category() == QChar::Other_Control; });
    text.truncate(500);
    return text;
}

QString sanitizeName(QString name)
{
    name = name.simplified();
    name.removeIf([](QChar c) { return c.category() == QChar::Other_Control; });
    name.truncate(24);
    return name;
}

} // namespace

struct CollabSessionGnsBridge
{
    static void statusChanged(SteamNetConnectionStatusChangedCallback_t* info)
    {
        for (CollabSession* session : liveSessions()) {
            // A client session's listenSocket_ is 0 and so is a client-side
            // event's m_hListenSocket; matching those would route one
            // client's events to whichever client registered first.
            const bool viaListen = info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid &&
                                   session->listenSocket_ == info->m_info.m_hListenSocket;
            if (session->ownsConnection(info->m_hConn) || viaListen) {
                session->onConnectionStatusChanged(info);
                return;
            }
        }
    }
};

CollabSession::CollabSession(QObject* parent) : QObject(parent)
{
    pumpTimer_ = new QTimer(this);
    pumpTimer_->setInterval(15);
    connect(pumpTimer_, &QTimer::timeout, this, [this] { pump(); });
    // Periodic desync sweep, client side only. Skipped while traffic is
    // flowing: an in-flight delta would trivially explain a mismatch, and the
    // per-edit path already keeps the peers converged.
    syncCheckTimer_ = new QTimer(this);
    syncCheckTimer_->setInterval(20000);
    connect(syncCheckTimer_, &QTimer::timeout, this, [this] {
        if (role_ != Role::Client) return;
        if (lastTraffic_.isValid() && lastTraffic_.elapsed() < 5000) return;
        requestSyncCheck();
    });
    // Host-side presence tick: rebuilds the pose list from live membership
    // every time (a cached list would outlive kicks/leaves) and fans it out.
    poseTimer_ = new QTimer(this);
    poseTimer_->setInterval(250);
    connect(poseTimer_, &QTimer::timeout, this, [this] {
        if (role_ == Role::Host) broadcastPoses();
    });
    liveSessions().push_back(this);
}

CollabSession::~CollabSession()
{
    leave();
    std::erase(liveSessions(), this);
}

int CollabSession::peerCount() const
{
    if (role_ == Role::Host) return int(peers_.size()) + 1;
    if (role_ == Role::Client) return hostConnection_ ? 2 : 1;  // at least us and the host
    return 0;
}

void CollabSession::setSharedFileSystem(std::shared_ptr<hammer::assets::GameFileSystem> fileSystem)
{
    sharedFileSystem_ = std::move(fileSystem);
}

void CollabSession::setAssetDownloadDirectory(const QString& directory)
{
    assetDownloadDirectory_ = directory;
}

void CollabSession::setLocalName(const QString& name)
{
    localName_ = sanitizeName(name);
}

QString CollabSession::peerAddress(int idBase) const
{
    const auto it = std::find_if(peers_.begin(), peers_.end(), [idBase](const Peer& peer) {
        return peer.idBase == idBase;
    });
    if (it == peers_.end()) return {};
    SteamNetConnectionInfo_t info{};
    if (!SteamNetworkingSockets()->GetConnectionInfo(it->connection, &info)) return {};
    char buffer[SteamNetworkingIPAddr::k_cchMaxString];
    info.m_addrRemote.ToString(buffer, sizeof(buffer), false);
    return QString::fromLatin1(buffer);
}

void CollabSession::banAddress(const QString& address)
{
    bannedAddresses_.insert(address.trimmed());
}

bool CollabSession::unbanAddress(const QString& address)
{
    return bannedAddresses_.remove(address.trimmed());
}

QStringList CollabSession::bannedAddresses() const
{
    QStringList list(bannedAddresses_.begin(), bannedAddresses_.end());
    list.sort();
    return list;
}

std::vector<std::pair<QString, int>> CollabSession::connectedPeers() const
{
    std::vector<std::pair<QString, int>> result;
    for (const Peer& peer : peers_) {
        if (peer.welcomed) result.emplace_back(peer.name, peer.idBase);
    }
    return result;
}

void CollabSession::kickPeer(int idBase)
{
    if (role_ != Role::Host) return;
    const auto it = std::find_if(peers_.begin(), peers_.end(), [idBase](const Peer& peer) {
        return peer.idBase == idBase;
    });
    if (it == peers_.end()) return;
    const std::uint32_t connection = it->connection;
    send(connection, encodeMessage({{"t", "kicked"}}));
    // Linger on close so the queued "kicked" notice flushes; without it the
    // peer just sees a generic disconnect.
    SteamNetworkingSockets()->CloseConnection(connection, 0, "kicked", true);
    dropPeer(connection, true);
}

void CollabSession::updateLocalPose(double x, double y, double z, double pitchDegrees,
                                    double yawDegrees)
{
    if (!active()) return;
    const bool moved = !localPoseValid_ || std::abs(localPose_.x - x) > 0.5 ||
                       std::abs(localPose_.y - y) > 0.5 || std::abs(localPose_.z - z) > 0.5 ||
                       std::abs(localPose_.pitch - pitchDegrees) > 0.25 ||
                       std::abs(localPose_.yaw - yawDegrees) > 0.25;
    localPose_ = {localId_, localName_, x, y, z, pitchDegrees, yawDegrees};
    localPoseValid_ = true;
    if (!moved) return;
    if (role_ == Role::Client && hostConnection_) {
        if (lastPoseSent_.isValid() && lastPoseSent_.elapsed() < 200) {
            // Movement inside the rate window: mark it so pump() sends the
            // RESTING position once the window expires — otherwise the last
            // flick before stopping is silently off by one update forever.
            posePending_ = true;
            return;
        }
        posePending_ = false;
        lastPoseSent_.restart();
        // No id on the wire: the host stamps the sender's, so one client
        // cannot puppet another's avatar.
        sendUnreliable(hostConnection_,
                       encodeMessage({{"t", "pose"},
                                      {"x", x},
                                      {"y", y},
                                      {"z", z},
                                      {"pt", pitchDegrees},
                                      {"yw", yawDegrees}}));
    }
    // Host: the aggregate timer picks localPose_ up on its next tick.
}

bool CollabSession::ownsConnection(std::uint32_t connection) const
{
    if (role_ == Role::Client) return hostConnection_ == connection;
    return std::any_of(peers_.begin(), peers_.end(),
                       [connection](const Peer& peer) { return peer.connection == connection; });
}

bool CollabSession::hostSession(quint16 port, const hammer::vmf::Document& current, QString* error)
{
    if (active()) {
        if (error) *error = tr("A session is already active.");
        return false;
    }
    if (!ensureGnsInitialized(error)) return false;

    SteamNetworkingIPAddr address;
    address.Clear();
    address.m_port = port;
    SteamNetworkingConfigValue_t option;
    option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                  reinterpret_cast<void*>(&CollabSessionGnsBridge::statusChanged));
    listenSocket_ = SteamNetworkingSockets()->CreateListenSocketIP(address, 1, &option);
    if (listenSocket_ == k_HSteamListenSocket_Invalid) {
        listenSocket_ = 0;
        if (error) *error = tr("Could not listen on UDP port %1.").arg(port);
        return false;
    }
    pollGroup_ = SteamNetworkingSockets()->CreatePollGroup();

    role_ = Role::Host;
    localId_ = kIdSpan;
    baseline_ = current;
    nextPeerIndex_ = 1;
    pumpTimer_->start();
    poseTimer_->start();
    emit statusMessage(tr("Hosting collaborative session on UDP port %1 (encrypted)").arg(port));
    return true;
}

bool CollabSession::joinSession(const QString& address, quint16 port, QString* error)
{
    if (active()) {
        if (error) *error = tr("A session is already active.");
        return false;
    }
    if (!ensureGnsInitialized(error)) return false;

    SteamNetworkingIPAddr target;
    target.Clear();
    const QString spec = QStringLiteral("%1:%2").arg(address).arg(port);
    if (!target.ParseString(spec.toUtf8().constData())) {
        // GNS only parses IP literals; anything else goes through DNS. The
        // lookup blocks, but only for the dialog's OK click and with the
        // resolver's own timeout.
        const QHostInfo info = QHostInfo::fromName(address);
        bool resolved = false;
        if (info.error() == QHostInfo::NoError) {
            // Prefer IPv4: it is what LAN/port-forward setups overwhelmingly
            // route, and an IPv6-first pick would fail on hosts that publish
            // an AAAA record but listen behind a v4-only forward.
            QList<QHostAddress> candidates = info.addresses();
            std::stable_sort(candidates.begin(), candidates.end(),
                             [](const QHostAddress& a, const QHostAddress& b) {
                                 return (a.protocol() == QAbstractSocket::IPv4Protocol) >
                                        (b.protocol() == QAbstractSocket::IPv4Protocol);
                             });
            for (const QHostAddress& candidate : candidates) {
                const QString literal =
                    candidate.protocol() == QAbstractSocket::IPv6Protocol
                        ? QStringLiteral("[%1]:%2").arg(candidate.toString()).arg(port)
                        : QStringLiteral("%1:%2").arg(candidate.toString()).arg(port);
                target.Clear();
                if (target.ParseString(literal.toUtf8().constData())) {
                    resolved = true;
                    break;
                }
            }
        }
        if (!resolved) {
            if (error) {
                *error = info.error() == QHostInfo::NoError
                             ? tr("\"%1\" did not resolve to a usable address.").arg(address)
                             : tr("Could not resolve \"%1\": %2").arg(address, info.errorString());
            }
            return false;
        }
    }
    SteamNetworkingConfigValue_t option;
    option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                  reinterpret_cast<void*>(&CollabSessionGnsBridge::statusChanged));
    hostConnection_ = SteamNetworkingSockets()->ConnectByIPAddress(target, 1, &option);
    if (hostConnection_ == k_HSteamNetConnection_Invalid) {
        hostConnection_ = 0;
        if (error) *error = tr("Could not start a connection to %1.").arg(spec);
        return false;
    }
    role_ = Role::Client;
    pumpTimer_->start();
    syncCheckTimer_->start();
    emit statusMessage(tr("Connecting to %1, waiting for the map...").arg(spec));
    return true;
}

void CollabSession::leave()
{
    leaving_ = true;
    ISteamNetworkingSockets* sockets =
        role_ != Role::None ? SteamNetworkingSockets() : nullptr;
    if (sockets) {
        for (const Peer& peer : peers_)
            sockets->CloseConnection(peer.connection, 0, "session closed", false);
        if (hostConnection_) sockets->CloseConnection(hostConnection_, 0, "leaving", false);
        if (pollGroup_) sockets->DestroyPollGroup(pollGroup_);
        if (listenSocket_) sockets->CloseListenSocket(listenSocket_);
    }
    peers_.clear();
    hostAssembly_.clear();
    hostOutgoing_.clear();
    hostConnection_ = 0;
    pollGroup_ = 0;
    listenSocket_ = 0;
    if (pumpTimer_) pumpTimer_->stop();
    if (syncCheckTimer_) syncCheckTimer_->stop();
    if (poseTimer_) poseTimer_->stop();
    localId_ = 0;
    localPoseValid_ = false;
    hashRoundActive_ = false;
    resyncPending_ = false;
    servableAssets_.clear();
    probedAssetRefs_.clear();
    offeredAssets_.clear();
    expectedAssets_.clear();
    downloadedAssets_ = 0;
    role_ = Role::None;
    leaving_ = false;
}

void CollabSession::localDocumentChanged(const hammer::vmf::Document& current)
{
    if (!active()) return;
    // Between fullreq and the resync arriving, a local delta would apply on
    // the host but be erased locally by the adopt - a permanent one-sided
    // ghost. Holding the edit loses at most a sub-second of work instead.
    if (resyncPending_) return;
    const hammer::vmf::SyncDelta delta = hammer::vmf::diffDocuments(baseline_, current);
    if (delta.empty()) return;
    baseline_ = current;
    const QByteArray body =
        encodeMessage({{"t", "delta"}, {"p", packPayload(hammer::vmf::serializeDelta(delta))}});
    if (role_ == Role::Host) {
        broadcast(body, 0);
        maybeOfferNewAssets(delta);
    } else if (hostConnection_) {
        ++sendCount_;
        send(hostConnection_, body);
    }
}

void CollabSession::sendChat(const QString& text)
{
    if (!active()) return;
    const QString clean = sanitizeChat(text);
    if (clean.isEmpty()) return;
    if (role_ == Role::Host) {
        broadcast(encodeMessage({{"t", "chat"}, {"from", localName_}, {"msg", clean}}), 0);
        emit chatMessageReceived(localName_, clean);
    } else if (hostConnection_) {
        // No sender name on the wire: the host stamps it from the connection.
        send(hostConnection_, encodeMessage({{"t", "chat"}, {"msg", clean}}));
        emit chatMessageReceived(localName_, clean);
    }
}

void CollabSession::requestSyncCheck()
{
    if (role_ != Role::Client || !hostConnection_ || hashRoundActive_ || resyncPending_) return;
    hashRoundActive_ = true;
    sendsAtHashq_ = sendCount_;
    send(hostConnection_, encodeMessage({{"t", "hashq"}}));
}

void CollabSession::onConnectionStatusChanged(const void* rawInfo)
{
    const auto* info = static_cast<const SteamNetConnectionStatusChangedCallback_t*>(rawInfo);
    ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
    switch (info->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting:
        if (role_ == Role::Host && info->m_info.m_hListenSocket == listenSocket_) {
            char remote[SteamNetworkingIPAddr::k_cchMaxString];
            info->m_info.m_addrRemote.ToString(remote, sizeof(remote), false);
            if (bannedAddresses_.contains(QString::fromLatin1(remote))) {
                sockets->CloseConnection(info->m_hConn, 0, "banned", false);
                emit statusMessage(tr("Rejected banned address %1")
                                       .arg(QString::fromLatin1(remote)));
                break;
            }
            if (sockets->AcceptConnection(info->m_hConn) == k_EResultOK) {
                sockets->SetConnectionPollGroup(info->m_hConn, pollGroup_);
                peers_.push_back({info->m_hConn, 0, false});
            } else {
                sockets->CloseConnection(info->m_hConn, 0, "accept failed", false);
            }
        }
        break;
    case k_ESteamNetworkingConnectionState_Connected:
        // Client side: the encrypted channel is up; introduce ourselves.
        if (role_ == Role::Client && info->m_hConn == hostConnection_)
            send(hostConnection_,
                 encodeMessage({{"t", "hello"}, {"v", 1}, {"name", localName_}}));
        break;
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
        if (leaving_) break;
        if (role_ == Role::Client && info->m_hConn == hostConnection_) {
            sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
            hostConnection_ = 0;
            const QString reason =
                info->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer
                    ? tr("The session host disconnected.")
                    : tr("Lost the connection to the session host.");
            leave();
            emit sessionEnded(reason);
        } else if (role_ == Role::Host) {
            sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
            dropPeer(info->m_hConn, true);
        }
        break;
    }
    default:
        break;
    }
}

void CollabSession::pump()
{
    // The handlers below reach MainWindow code that may call processEvents
    // (progress dialogs), which would fire the pump timer re-entrantly and
    // drain messages into handlers that are mid-flight.
    if (pumping_) return;
    pumping_ = true;
    const auto pumpGuard = qScopeGuard([this] { pumping_ = false; });
    ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
    sockets->RunCallbacks();
    if (role_ == Role::None) return;

    // Flush a pose the rate limiter swallowed, now that the window is open.
    if (role_ == Role::Client && hostConnection_ && posePending_ && localPoseValid_ &&
        (!lastPoseSent_.isValid() || lastPoseSent_.elapsed() >= 200)) {
        posePending_ = false;
        lastPoseSent_.restart();
        sendUnreliable(hostConnection_,
                       encodeMessage({{"t", "pose"},
                                      {"x", localPose_.x}, {"y", localPose_.y},
                                      {"z", localPose_.z}, {"pt", localPose_.pitch},
                                      {"yw", localPose_.yaw}}));
    }

    // Retry chunks the transport's send buffer rejected earlier.
    if (role_ == Role::Client && hostConnection_ && !hostOutgoing_.empty())
        flushOutgoing(hostConnection_, hostOutgoing_);
    for (Peer& peer : peers_) {
        if (!peer.outgoing.empty()) flushOutgoing(peer.connection, peer.outgoing);
    }

    SteamNetworkingMessage_t* messages[16];
    for (;;) {
        int count = 0;
        if (role_ == Role::Host && pollGroup_) {
            count = sockets->ReceiveMessagesOnPollGroup(pollGroup_, messages, 16);
        } else if (role_ == Role::Client && hostConnection_) {
            count = sockets->ReceiveMessagesOnConnection(hostConnection_, messages, 16);
        }
        if (count <= 0) break;
        lastTraffic_.restart();
        for (int i = 0; i < count; ++i) {
            // First byte is the fragmentation tag (see send()): 'J' complete,
            // 'F' fragment with more to come, 'L' last fragment.
            const char* data = static_cast<const char*>(messages[i]->m_pData);
            const qsizetype size = qsizetype(messages[i]->m_cbSize);
            const char tag = size > 0 ? data[0] : '\0';
            const QByteArray piece(data + 1, size > 0 ? size - 1 : 0);
            const std::uint32_t connection = messages[i]->m_conn;
            messages[i]->Release();

            QByteArray* assembly = &hostAssembly_;
            Peer* peer = nullptr;
            if (role_ == Role::Host) {
                const auto it = std::find_if(peers_.begin(), peers_.end(),
                                             [connection](const Peer& candidate) {
                                                 return candidate.connection == connection;
                                             });
                if (it == peers_.end()) continue;
                peer = &*it;
                assembly = &peer->assembly;
            }

            if (tag == 'F') {
                assembly->append(piece);
                continue;
            }
            QByteArray body;
            if (tag == 'L') {
                assembly->append(piece);
                body = std::move(*assembly);
                assembly->clear();
            } else if (tag == 'J') {
                body = piece;
            } else {
                continue;  // unknown tag; drop
            }
            if (peer) handleHostMessage(*peer, body);
            else handleClientMessage(body);
            // A message can end the session (unreadable map); stop draining.
            if (role_ == Role::None) return;
        }
    }
}

void CollabSession::handleHostMessage(Peer& peer, const QByteArray& body)
{
    const QJsonObject message = QJsonDocument::fromJson(body).object();
    const QString type = message.value("t").toString();
    if (type == QLatin1String("hello")) {
        peer.idBase = ++nextPeerIndex_ * kIdSpan;  // peer 1 gets 2'000'000...
        peer.welcomed = true;
        peer.name = sanitizeName(message.value("name").toString());
        if (peer.name.isEmpty()) peer.name = tr("Editor %1").arg(nextPeerIndex_);
        emit peerListChanged();
        send(peer.connection, encodeMessage({{"t", "welcome"},
                                             {"idBase", peer.idBase},
                                             {"idSpan", kIdSpan},
                                             {"p", packPayload(baseline_.serialize(false))}}));
        emit statusMessage(tr("A collaborator joined (%1 editors connected)").arg(peerCount()));
        announcePeerCount();
        sendAssetManifest(peer);
        return;
    }
    if (type == QLatin1String("delta") && peer.welcomed) {
        const std::optional<hammer::vmf::SyncDelta> delta =
            hammer::vmf::parseDelta(unpackPayload(message));
        if (!delta) return;
        hammer::vmf::applyDelta(baseline_, *delta);
        broadcast(body, peer.connection);
        emit remoteDelta(*delta);
        maybeOfferNewAssets(*delta);
        return;
    }
    if (type == QLatin1String("chat") && peer.welcomed) {
        const QString clean = sanitizeChat(message.value("msg").toString());
        if (clean.isEmpty()) return;
        broadcast(encodeMessage({{"t", "chat"}, {"from", peer.name}, {"msg", clean}}),
                  peer.connection);
        emit chatMessageReceived(peer.name, clean);
        return;
    }
    if (type == QLatin1String("pose") && peer.welcomed) {
        peer.pose = {peer.idBase,          peer.name,
                     message.value("x").toDouble(),  message.value("y").toDouble(),
                     message.value("z").toDouble(),  message.value("pt").toDouble(),
                     message.value("yw").toDouble()};
        peer.hasPose = true;
        return;
    }
    if (type == QLatin1String("hashq") && peer.welcomed) {
        // Reliable-ordered channel: every delta sent before this reply lands
        // before it, so the client compares against a fully caught-up state.
        send(peer.connection,
             encodeMessage(
                 {{"t", "hashr"},
                  {"h", QString::number(quint64(hammer::vmf::documentSyncHash(baseline_)), 16)}}));
        return;
    }
    if (type == QLatin1String("assetreq") && peer.welcomed) {
        // Serve ONLY what this session advertised: the request comes from an
        // untrusted peer, and the manifest set is what stops it turning the
        // host into an arbitrary file reader.
        for (const QJsonValue& value : message.value("files").toArray()) {
            const std::string path = value.toString().toStdString();
            if (!servableAssets_.count(path) || !hammer::collab::isSafeAssetPath(path)) continue;
            const auto bytes = sharedFileSystem_ ? sharedFileSystem_->readFile(path) : std::nullopt;
            if (!bytes) continue;
            send(peer.connection,
                 encodeMessage({{"t", "assetdata"},
                                {"f", QString::fromStdString(path)},
                                {"p", packPayload(std::string(bytes->begin(), bytes->end()))}}));
        }
        return;
    }
    if (type == QLatin1String("fullreq") && peer.welcomed) {
        send(peer.connection,
             encodeMessage({{"t", "resync"}, {"p", packPayload(baseline_.serialize(false))}}));
        emit statusMessage(tr("A collaborator desynced; sent them a fresh copy of the map"));
        return;
    }
}

void CollabSession::handleClientMessage(const QByteArray& body)
{
    const QJsonObject message = QJsonDocument::fromJson(body).object();
    const QString type = message.value("t").toString();
    if (type == QLatin1String("welcome")) {
        const std::optional<hammer::vmf::Document> document =
            hammer::vmf::Document::parse(unpackPayload(message));
        if (!document) {
            const QString reason = tr("The host sent an unreadable map.");
            leave();
            emit sessionEnded(reason);
            return;
        }
        baseline_ = *document;
        localId_ = message.value("idBase").toInt();
        emit statusMessage(tr("Joined collaborative session"));
        emit bootstrapDocument(baseline_, message.value("idBase").toInt(),
                               message.value("idSpan").toInt(kIdSpan));
        return;
    }
    if (type == QLatin1String("delta")) {
        const std::optional<hammer::vmf::SyncDelta> delta =
            hammer::vmf::parseDelta(unpackPayload(message));
        if (!delta) return;
        hammer::vmf::applyDelta(baseline_, *delta);
        emit remoteDelta(*delta);
        return;
    }
    if (type == QLatin1String("hashr")) {
        if (!hashRoundActive_) return;
        hashRoundActive_ = false;
        // A local edit crossed this round: the mismatch it would show is
        // expected, not a desync. The next sweep gets a clean read.
        if (sendCount_ != sendsAtHashq_) return;
        bool ok = false;
        const quint64 hostHash = message.value("h").toString().toULongLong(&ok, 16);
        if (!ok) return;
        if (hostHash == quint64(hammer::vmf::documentSyncHash(baseline_))) return;
        resyncPending_ = true;
        send(hostConnection_, encodeMessage({{"t", "fullreq"}}));
        emit statusMessage(tr("Scene desynced from the host — fetching a fresh copy..."));
        return;
    }
    if (type == QLatin1String("resync")) {
        const std::optional<hammer::vmf::Document> document =
            hammer::vmf::Document::parse(unpackPayload(message));
        resyncPending_ = false;
        if (!document) return;
        baseline_ = *document;
        emit resyncDocument(baseline_);
        emit statusMessage(tr("Scene rebuilt from the host's copy"));
        return;
    }
    if (type == QLatin1String("assets")) {
        offeredAssets_.clear();
        qint64 totalBytes = 0;
        for (const QJsonValue& value : message.value("files").toArray()) {
            const QJsonObject entry = value.toObject();
            const std::string path = entry.value("f").toString().toStdString();
            if (!hammer::collab::isSafeAssetPath(path)) continue;
            // Already having identical content anywhere in the search path
            // (loose or VPK) satisfies the entry.
            if (sharedFileSystem_) {
                const auto mine = sharedFileSystem_->readFile(path);
                if (mine) {
                    const QByteArray hash =
                        QCryptographicHash::hash(QByteArray::fromRawData(
                                                     reinterpret_cast<const char*>(mine->data()),
                                                     qsizetype(mine->size())),
                                                 QCryptographicHash::Sha1)
                            .toHex();
                    if (QString::fromLatin1(hash) == entry.value("h").toString()) continue;
                }
            }
            offeredAssets_.push_back(path);
            totalBytes += entry.value("n").toInteger();
        }
        if (!offeredAssets_.empty() && !assetDownloadDirectory_.isEmpty())
            emit assetsOffered(int(offeredAssets_.size()), totalBytes);
        return;
    }
    if (type == QLatin1String("assetdata")) {
        const std::string path = message.value("f").toString().toStdString();
        if (!expectedAssets_.count(path) || !hammer::collab::isSafeAssetPath(path)) return;
        expectedAssets_.erase(path);
        const std::string bytes = unpackPayload(message);
        const QString target =
            QDir(assetDownloadDirectory_).filePath(QString::fromStdString(path));
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile file(target);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(bytes.data(), qint64(bytes.size()));
            ++downloadedAssets_;
        }
        if (expectedAssets_.empty()) {
            emit assetsDownloaded(downloadedAssets_, assetDownloadDirectory_);
            emit statusMessage(tr("Downloaded %1 custom asset(s) from the host")
                                   .arg(downloadedAssets_));
            downloadedAssets_ = 0;
        }
        return;
    }
    if (type == QLatin1String("chat")) {
        const QString clean = sanitizeChat(message.value("msg").toString());
        if (!clean.isEmpty())
            emit chatMessageReceived(sanitizeName(message.value("from").toString()), clean);
        return;
    }
    if (type == QLatin1String("kicked")) {
        const QString reason = tr("You were removed from the session by the host.");
        leave();
        emit sessionEnded(reason);
        return;
    }
    if (type == QLatin1String("poses")) {
        QList<CollabPeerPose> poses;
        for (const QJsonValue& value : message.value("list").toArray()) {
            const QJsonObject entry = value.toObject();
            const int id = entry.value("id").toInt();
            if (id == localId_) continue;  // never render one's own avatar
            poses.append({id, sanitizeName(entry.value("name").toString()),
                          entry.value("x").toDouble(), entry.value("y").toDouble(),
                          entry.value("z").toDouble(), entry.value("pt").toDouble(),
                          entry.value("yw").toDouble()});
        }
        emit peerPosesChanged(poses);
        return;
    }
    if (type == QLatin1String("peers")) {
        emit peerCountChanged(message.value("n").toInt());
    }
}

void CollabSession::broadcastPoses()
{
    // Rebuilt from live membership each tick so kicks and leaves drop their
    // avatar on the next beat.
    QJsonArray list;
    QList<CollabPeerPose> forHost;
    if (localPoseValid_) {
        list.append(QJsonObject{{"id", localId_}, {"name", localName_},
                                {"x", localPose_.x}, {"y", localPose_.y}, {"z", localPose_.z},
                                {"pt", localPose_.pitch}, {"yw", localPose_.yaw}});
    }
    for (const Peer& peer : peers_) {
        if (!peer.welcomed || !peer.hasPose) continue;
        list.append(QJsonObject{{"id", peer.idBase}, {"name", peer.name},
                                {"x", peer.pose.x}, {"y", peer.pose.y}, {"z", peer.pose.z},
                                {"pt", peer.pose.pitch}, {"yw", peer.pose.yaw}});
        forHost.append(peer.pose);
    }
    if (list.isEmpty()) return;
    const QByteArray body = encodeMessage({{"t", "poses"}, {"list", list}});
    for (const Peer& peer : peers_) {
        if (peer.welcomed) sendUnreliable(peer.connection, body);
    }
    emit peerPosesChanged(forHost);
}

void CollabSession::sendUnreliable(std::uint32_t connection, const QByteArray& body)
{
    // Straight to the wire, bypassing the reliable queue: a pose stuck
    // behind a 40MB map transfer is exactly what this channel avoids, and a
    // lost one is replaced by the next tick. Tag 'J' keeps pump() parsing
    // uniform; poses are far below the message size cap.
    QByteArray message;
    message.reserve(body.size() + 1);
    message.append('J');
    message.append(body);
    SteamNetworkingSockets()->SendMessageToConnection(
        connection, message.constData(), quint32(message.size()),
        k_nSteamNetworkingSend_Unreliable | k_nSteamNetworkingSend_NoNagle, nullptr);
}

void CollabSession::maybeOfferNewAssets(const hammer::vmf::SyncDelta& delta)
{
    if (role_ != Role::Host || !sharedFileSystem_) return;
    // Cost proportional to the edit: probe only the objects the delta
    // touched. A hit outside the already-advertised set means the map now
    // uses custom content the peers have not been offered.
    hammer::vmf::Document probe;
    for (const hammer::vmf::SyncDelta::Upsert& upsert : delta.upserts)
        probe.roots().push_back(upsert.block);
    if (probe.roots().empty()) return;
    // Cheap string pass first: only a genuinely new material/model reference
    // justifies touching the file system. Without this every edit re-probed
    // every material and model it mentioned - including misses, which walk
    // every search path enumerating directories - on the UI thread.
    const std::vector<std::string> references = hammer::collab::collectAssetReferences(probe);
    const bool anyNewReference =
        std::any_of(references.begin(), references.end(), [this](const std::string& reference) {
            return !probedAssetRefs_.count(reference);
        });
    if (!anyNewReference) return;
    probedAssetRefs_.insert(references.begin(), references.end());

    const std::vector<std::string> paths =
        hammer::collab::collectCustomAssetPaths(probe, *sharedFileSystem_);
    const bool anyNew = std::any_of(paths.begin(), paths.end(), [this](const std::string& path) {
        return !servableAssets_.count(path);
    });
    if (!anyNew) return;
    for (Peer& peer : peers_) {
        if (peer.welcomed) sendAssetManifest(peer);
    }
}

void CollabSession::sendAssetManifest(Peer& peer)
{
    if (!sharedFileSystem_) return;
    const std::vector<std::string> paths =
        hammer::collab::collectCustomAssetPaths(baseline_, *sharedFileSystem_);
    // Everything the map already references has now been probed; later edits
    // reusing these materials do no filesystem work at all.
    const std::vector<std::string> references =
        hammer::collab::collectAssetReferences(baseline_);
    probedAssetRefs_.insert(references.begin(), references.end());
    if (paths.empty()) return;
    QJsonArray files;
    for (const std::string& path : paths) {
        const auto bytes = sharedFileSystem_->readFile(path);
        if (!bytes) continue;
        // Append-only for the session: once advertised, a path stays
        // servable, so repeat manifests and late requests keep working.
        servableAssets_.insert(path);
        files.append(QJsonObject{
            {"f", QString::fromStdString(path)},
            {"n", qint64(bytes->size())},
            {"h", QString::fromLatin1(
                      QCryptographicHash::hash(
                          QByteArray::fromRawData(reinterpret_cast<const char*>(bytes->data()),
                                                  qsizetype(bytes->size())),
                          QCryptographicHash::Sha1)
                          .toHex())}});
    }
    if (!files.isEmpty())
        send(peer.connection, encodeMessage({{"t", "assets"}, {"files", files}}));
}

void CollabSession::acceptAssetOffer()
{
    if (role_ != Role::Client || !hostConnection_ || offeredAssets_.empty()) return;
    QJsonArray files;
    expectedAssets_.clear();
    for (const std::string& path : offeredAssets_) {
        files.append(QString::fromStdString(path));
        expectedAssets_.insert(path);
    }
    offeredAssets_.clear();
    downloadedAssets_ = 0;
    send(hostConnection_, encodeMessage({{"t", "assetreq"}, {"files", files}}));
}

void CollabSession::declineAssetOffer()
{
    offeredAssets_.clear();
}

void CollabSession::dropPeer(std::uint32_t connection, bool announce)
{
    const auto it = std::find_if(peers_.begin(), peers_.end(), [connection](const Peer& peer) {
        return peer.connection == connection;
    });
    if (it == peers_.end()) return;
    peers_.erase(it);
    emit peerListChanged();
    if (announce) {
        emit statusMessage(tr("A collaborator left (%1 editors connected)").arg(peerCount()));
        announcePeerCount();
    }
}

void CollabSession::broadcast(const QByteArray& body, std::uint32_t except)
{
    for (const Peer& peer : peers_) {
        if (peer.welcomed && peer.connection != except) send(peer.connection, body);
    }
}

void CollabSession::send(std::uint32_t connection, const QByteArray& body)
{
    lastTraffic_.restart();
    // GNS caps a single message at 512K
    // (k_cbMaxSteamNetworkingSocketsMessageSizeSend); a whole map's welcome
    // payload can exceed that, so large bodies are split into tagged chunks.
    // Reliable-ordered delivery makes reassembly a simple append (see pump()).
    //
    // Chunks go through a per-connection queue, NOT straight to the wire:
    // when the transport's send buffer fills, SendMessageToConnection rejects
    // the message (k_EResultLimitExceeded), and a dropped chunk would leave
    // the peer's reassembly stuck forever. The queue drains from pump().
    constexpr qsizetype kChunk = 256 * 1024;
    std::deque<QByteArray>* queue = &hostOutgoing_;
    if (role_ == Role::Host) {
        const auto it = std::find_if(peers_.begin(), peers_.end(),
                                     [connection](const Peer& peer) {
                                         return peer.connection == connection;
                                     });
        if (it == peers_.end()) return;
        queue = &it->outgoing;
    }
    if (body.size() <= kChunk) {
        QByteArray message;
        message.reserve(body.size() + 1);
        message.append('J');
        message.append(body);
        queue->push_back(std::move(message));
    } else {
        for (qsizetype offset = 0; offset < body.size(); offset += kChunk) {
            const qsizetype length = std::min(kChunk, body.size() - offset);
            QByteArray message;
            message.reserve(length + 1);
            message.append(offset + length < body.size() ? 'F' : 'L');
            message.append(body.constData() + offset, length);
            queue->push_back(std::move(message));
        }
    }
    flushOutgoing(connection, *queue);
}

void CollabSession::flushOutgoing(std::uint32_t connection, std::deque<QByteArray>& queue)
{
    ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
    while (!queue.empty()) {
        const QByteArray& message = queue.front();
        const EResult result = sockets->SendMessageToConnection(
            connection, message.constData(), quint32(message.size()),
            k_nSteamNetworkingSend_Reliable, nullptr);
        if (result == k_EResultLimitExceeded ||
            result == k_EResultInvalidState) {
            // Buffer full, or the connection is mid-handshake and not ready
            // to accept application data yet: keep the queue and retry from
            // pump(). Only hard failures below tear the connection down.
            return;
        }
        queue.pop_front();
        if (result != k_EResultOK) {
            // Message rejected outright: a partial body is useless to the
            // peer, so drop the rest of the queue and end the connection
            // rather than leave a half-synced peer attached and silently out
            // of date. Closing our own connection fires no local status
            // callback, and this can run mid-broadcast (inside a peers_
            // walk), so the bookkeeping is deferred to the event loop.
            queue.clear();
            sockets->CloseConnection(connection, 0, "send failed", false);
            QMetaObject::invokeMethod(this, [this, connection] {
                if (role_ == Role::Client && connection == hostConnection_) {
                    hostConnection_ = 0;
                    const QString reason = tr("Lost the connection to the session host.");
                    leave();
                    emit sessionEnded(reason);
                } else if (role_ == Role::Host) {
                    dropPeer(connection, true);
                }
            }, Qt::QueuedConnection);
            return;
        }
    }
}

void CollabSession::announcePeerCount()
{
    const QByteArray body = encodeMessage({{"t", "peers"}, {"n", peerCount()}});
    broadcast(body, 0);
    emit peerCountChanged(peerCount());
}
