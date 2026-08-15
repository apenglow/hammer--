#pragma once

// "Deploy the Freeman": a first-person walkthrough window. Renders the current
// map through a captive perspective MapViewWidget and replaces its editor
// input with HL2-style walking physics (gravity, friction, air control, jump).
// Collision uses ray casts against world brushes and displacement surfaces
// only; tool-textured brushes are neither drawn nor collided with.

#include "MaterialSystem.hpp"
#include "StudioModelSystem.hpp"
#include "VmfScene.hpp"

#include <QDialog>
#include <QElapsedTimer>
#include <QPointF>
#include <QSet>
#include <QString>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QKeyEvent;
class QMouseEvent;
class QTimer;
class MapViewWidget;

class FreemanWindow final : public QDialog
{
    Q_OBJECT
public:
    // Freeman: HL2 crowbar walkthrough. Soldier: TF2 soldier with the rocket
    // launcher (requires the TF2 assets — see soldierAssetsMissing).
    enum class Mode { Freeman, Soldier };

    // "document", when given, supplies brush-entity keyvalues the Scene does
    // not carry (trigger_teleport volumes and their destinations); it is read
    // during construction only and never stored.
    FreemanWindow(std::shared_ptr<const hammer::vmf::Scene> scene,
                  std::shared_ptr<hammer::assets::MaterialSystem> materials,
                  const hammer::vmf::Vec3& spawnPoint,
                  Mode mode = Mode::Freeman,
                  const hammer::vmf::Document* document = nullptr,
                  QWidget* parent = nullptr);
    ~FreemanWindow() override;

    // The TF2 asset paths required for Soldier mode that the mounted content
    // is missing (empty list = mode available).
    static std::vector<std::string> soldierAssetsMissing(
        const hammer::assets::MaterialSystem* materials);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void tick();
    void setCaptured(bool captured);
    void updateCrowbar();
    // Nearest hit along the ray against world brushes + displacements,
    // skipping tool-textured faces. Returns the ray parameter, or nullopt.
    std::optional<double> castRay(const hammer::vmf::Vec3& origin,
                                  const hammer::vmf::Vec3& direction,
                                  double maxDistance,
                                  hammer::vmf::Vec3* hitNormal = nullptr,
                                  std::string* hitMaterial = nullptr) const;
    // First sound path in candidates that exists, or empty.
    std::string firstSound(const std::vector<std::string>& candidates) const;
public:
    // Sweeps the HL2 player hull (32x32xhullHeight, feet origin) through the
    // world: exact Minkowski clipping (face planes, box axials, edge bevels)
    // against convex brushes and displacement triangles.
    struct HullTrace
    {
        double fraction{1.0};
        hammer::vmf::Vec3 normal{};
        bool hit{false};
        bool startSolid{false};
        // Diagnostics: which solid produced the hit.
        int solidId{-1};
        bool fromDisplacement{false};
    };

private:
    HullTrace traceHull(const hammer::vmf::Vec3& start, const hammer::vmf::Vec3& delta,
                        double hullHeight) const;
    void spawn();

    MapViewWidget* view_{nullptr};
    // The window's own copy of the scene: tool brushes are stripped and the
    // crowbar viewmodel marker is appended (and mutated per tick).
    std::shared_ptr<hammer::vmf::Scene> scene_;
    // Invisible collision-only solids: tools/toolsclip + toolsplayerclip.
    std::vector<hammer::vmf::BrushGeometry> clipBrushes_;
    std::shared_ptr<hammer::assets::MaterialSystem> materials_;
    int crowbarIndex_{-1};

    QTimer* timer_{nullptr};
    QElapsedTimer clock_;
    QSet<int> keys_;
    bool captured_{false};
    bool ignoreWarp_{false};

    hammer::vmf::Vec3 spawnPoint_{};
    hammer::vmf::Vec3 feet_{};
    hammer::vmf::Vec3 velocity_{};
    bool onGround_{false};
    bool ducked_{false};
    bool jumpHeld_{false};
    bool airAccelHigh_{false};
    hammer::vmf::Vec3 lastFacing_{};
    QElapsedTimer swayClock_;
    double eyeHeight_{64.0};

    // Sound: bytes come out of the mounted game filesystem, get cached to a
    // temp file once, and play through paplay/aplay detached.
    bool soundExists(const std::string& path) const;
    void playSound(const std::string& path);
    std::map<std::string, QString> soundCache_;
    bool swingSoundAvailable_{false};
    std::string swingSoundPath_;
    bool footstepsAvailable_{false};
    double stepTimer_{0.0};
    int stepIndex_{0};

    // Mode + per-mode movement parameters (Soldier: TF2 sv_gravity 800,
    // 240 u/s ground speed, no sprint).
    Mode mode_{Mode::Freeman};
    double gravity_{600.0};
    double walkSpeed_{190.0};
    double sprintSpeed_{320.0};
    double jumpVelocity() const;

    // Soldier: rocket launcher state (clip of 4, 1100 u/s rockets,
    // 169-unit blast radius; knockback per TF2 ApplyPushFromDamage).
    struct Rocket
    {
        bool active{false};
        hammer::vmf::Vec3 position{};
        hammer::vmf::Vec3 velocity{};
        double life{0.0};
        int markerIndex{-1};
    };
    std::vector<Rocket> rockets_;
    int rocketClip_{4};
    double fireCooldown_{0.0};
    double reloadTimer_{0.0};
    double reloadCycleDuration_{0.8};
    // Three-phase TF2 reload: reload_start (bring the launcher down, loads
    // nothing) -> reload_loop per rocket -> reload_finish once the clip is
    // full. Reloading only begins once the fire interval has elapsed, and
    // firing cancels any phase back to Idle. When the MDL lacks the phase
    // sequences, Loop alone runs on the 0.92 s first / 0.8 s next constants.
    enum class ReloadPhase { Idle, Start, Loop, Finish };
    ReloadPhase reloadPhase_{ReloadPhase::Idle};
    // (name, duration) per phase, empty name when the MDL lacks it.
    std::pair<std::string, double> reloadStartSequence_;
    std::pair<std::string, double> reloadLoopSequence_;
    std::pair<std::string, double> reloadFinishSequence_;
    // The launcher's draw sequence, played on (re)spawn.
    std::pair<std::string, double> drawSequence_;
    std::string drawSoundPath_;

    // trigger_teleport volumes (kept aside before the tool-brush strip) and
    // their pre-resolved destination entity's origin/angles.
    struct TeleportVolume
    {
        std::vector<hammer::vmf::BrushGeometry> brushes;
        hammer::vmf::Vec3 destination{};
        hammer::vmf::Vec3 destinationAngles{};
        bool hasAngles{false};
    };
    std::vector<TeleportVolume> teleports_;
    void checkTeleporters(double hullHeight);
    std::vector<std::string> explodeSounds_;
    std::vector<std::string> painSounds_;
    void fireRocket();
    void updateRockets(double dt);
    void explodeRocket(Rocket& rocket, const hammer::vmf::Vec3& source);
    void parkRocketMarker(Rocket& rocket);

    // Crowbar viewmodel bob/swing state (HL2 CalcViewmodelBob).
    double bobTime_{0.0};
    double lastBobTime_{0.0};
    double verticalBob_{0.0};
    double lateralBob_{0.0};
    double swingRemaining_{0.0};
    // The crowbar's real swing sequences (name, duration), probed from the
    // MDL at load: v_crowbar has no "attack1" — its swings are misscenter1/2.
    std::vector<std::pair<std::string, double>> swingSequences_;
    std::vector<std::pair<std::string, double>> hitSequences_;
    std::string swingSequence_;
    std::string idleSequence_{"idle01"};
    double swingDuration_{0.667};
    std::vector<std::string> impactSounds_;
};
