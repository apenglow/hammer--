#include "FreemanWindow.hpp"

#include "Camera3D.hpp"
#include "MapViewWidget.hpp"

#include <QCloseEvent>
#include <QCursor>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QProcess>
#include <QRandomGenerator>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace {

// HL2 movement constants (Source SDK gamemovement.cpp / hl2 gamerules
// defaults: sv_accelerate 10, sv_airaccelerate 10, sv_friction 4,
// sv_stopspeed 100, sv_gravity 600, sv_maxvelocity 3500, 21-unit jump apex).
constexpr double WalkSpeed = 190.0;
constexpr double SprintSpeed = 320.0;
constexpr double DuckSpeedFraction = 0.33;
constexpr double Accelerate = 10.0;
constexpr double AirAccelerate = 10.0;
// The classic surf/bhop server setting: sv_airaccelerate 999.
constexpr double HighAirAccelerate = 999.0;
constexpr double AirSpeedCap = 30.0;
constexpr double Friction = 4.0;
constexpr double StopSpeed = 100.0;
constexpr double Gravity = 600.0;
constexpr double MaxVelocity = 3500.0;
constexpr double JumpVelocity = 158.745078664;  // sqrt(2 * 600 * 21)
constexpr double StandEyeHeight = 64.0;   // VEC_VIEW
constexpr double DuckEyeHeight = 32.0;    // VEC_DUCK_VIEW
constexpr double StandHullHeight = 72.0;  // VEC_HULL_MAX
constexpr double DuckHullHeight = 36.0;   // VEC_DUCK_HULL_MAX
constexpr double HullRadius = 16.0;
constexpr double StepHeight = 18.0;
constexpr double Pi = 3.14159265358979323846;

// HL2 viewmodel bob (2013 SDK basehlcombatweapon_shared.cpp:
// HL2_BOB_CYCLE_MAX 0.45, HL2_BOB_UP 0.5; lateral bob runs the same shape at
// half frequency, and bobtime advances by speed/320 — even in mid-air, per
// the SDK's "let this cycle continue when in the air" note).
constexpr double BobCycle = 0.45;
constexpr double BobUp = 0.5;
constexpr double BobMaxSpeed = 320.0;
// CBaseViewModel::CalcViewModelLag sway constants.
constexpr double MaxViewmodelLag = 1.5;
// HL2 draws viewmodels in their own render pass at viewmodel_fov 54 (a 4:3
// horizontal value — ~41.8 degrees vertical), which is what seats the weapon
// visibly further back than the world FOV would. This crowbar renders in the
// world pass, so the same framing is emulated by pushing the model away from
// the eye by the tan-ratio of the two FOVs at a reference grip distance.
constexpr double ViewmodelFovTanHalf = 0.38215;  // tan(41.82 deg / 2)
constexpr double ViewmodelReferenceDistance = 10.0;
// Length of the crowbar's attack1 sequence window.
constexpr double SwingDuration = 0.4;

// TF2 Soldier mode (source-sdk-2013 tf_player.cpp / tf_gamerules.cpp /
// tf_weaponbase_gun.cpp; user-specified parameters override the SDK where
// they differ).
constexpr double SoldierGravity = 800.0;         // TF2 sv_gravity
constexpr double SoldierSpeed = 240.0;           // soldier ground speed
constexpr double SoldierJumpStand = 50.0;        // jump apex, uncrouched
constexpr double SoldierJumpDuck = 72.0;         // jump apex, crouched
constexpr double RocketSpeed = 1100.0;
constexpr double RocketRadius = 169.0;
constexpr double RocketDamage = 90.0;
constexpr double RocketFireInterval = 0.8;
constexpr double RocketReloadFirst = 0.92;       // TF2 first reload is longer
constexpr double RocketReloadNext = 0.8;
constexpr int RocketClipSize = 4;
constexpr double RocketLifetime = 10.0;
// CTFPlayer::ApplyPushFromDamage / DamageForce: force = damage *
// (48*48*82 / hull volume) * scale, capped at 1000. Ducking substitutes
// hull z = 55 ("Ducking actually increases blast force"), and the scale is
// tf_damageforcescale_self_soldier_badrj (5) grounded vs _rj (10) airborne.
constexpr double RjScaleAir = 10.0;
constexpr double RjScaleGround = 5.0;
constexpr double RjHullZStand = 82.0;
constexpr double RjHullZDuck = 55.0;
constexpr double RjForceCap = 1000.0;
// Self-push tuning: full radius damage into the force formula launched far
// harder than TF2 (which pushes off the reduced self-damage). Scales the
// damage fed to DamageForce for the soldier's own blasts.
constexpr double RjSelfDamageScale = 0.55;
// Soldier viewmodel framing offsets (view space, eyeballed).
constexpr double SoldierViewmodelRight = 9.0;
constexpr double SoldierViewmodelPull = 3.0;

const char* const SoldierViewmodel = "models/weapons/v_models/v_rocketlauncher_soldier.mdl";
const char* const RocketModel = "models/weapons/w_models/w_rocket.mdl";
const char* const RocketFireSound = "sound/weapons/rocket_shoot.wav";

bool isToolMaterial(std::string_view material)
{
    std::string lowered(material);
    std::replace(lowered.begin(), lowered.end(), '\\', '/');
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lowered.rfind("tools/", 0) == 0;
}

bool isToolBrush(const hammer::vmf::BrushGeometry& brush)
{
    for (const auto& face : brush.faces) {
        if (!isToolMaterial(face.material)) return false;
    }
    return !brush.faces.empty();
}

// tools/toolsclip and tools/toolsplayerclip block the player in Source (clip
// blocks everything, playerclip blocks players specifically) while staying
// invisible. Mappers rely on them to smooth exactly the geometry seams and
// odd corners that raw brush collision trips over.
bool isPlayerClipBrush(const hammer::vmf::BrushGeometry& brush)
{
    bool sawClip = false;
    for (const auto& face : brush.faces) {
        std::string lowered(face.material);
        std::replace(lowered.begin(), lowered.end(), '\\', '/');
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (lowered == "tools/toolsclip" || lowered == "tools/toolsplayerclip") {
            sawClip = true;
        } else if (!isToolMaterial(lowered)) {
            return false;
        }
    }
    return sawClip;
}

std::optional<double> rayTriangle(const hammer::vmf::Vec3& origin,
                                  const hammer::vmf::Vec3& direction,
                                  const hammer::vmf::Vec3& a,
                                  const hammer::vmf::Vec3& b,
                                  const hammer::vmf::Vec3& c)
{
    const hammer::vmf::Vec3 edge1{b.x - a.x, b.y - a.y, b.z - a.z};
    const hammer::vmf::Vec3 edge2{c.x - a.x, c.y - a.y, c.z - a.z};
    const hammer::vmf::Vec3 h{direction.y * edge2.z - direction.z * edge2.y,
                              direction.z * edge2.x - direction.x * edge2.z,
                              direction.x * edge2.y - direction.y * edge2.x};
    const double det = edge1.x * h.x + edge1.y * h.y + edge1.z * h.z;
    if (std::abs(det) < 1e-9) return std::nullopt;
    const double inverse = 1.0 / det;
    const hammer::vmf::Vec3 s{origin.x - a.x, origin.y - a.y, origin.z - a.z};
    const double u = (s.x * h.x + s.y * h.y + s.z * h.z) * inverse;
    if (u < -1e-6 || u > 1.0 + 1e-6) return std::nullopt;
    const hammer::vmf::Vec3 q{s.y * edge1.z - s.z * edge1.y,
                              s.z * edge1.x - s.x * edge1.z,
                              s.x * edge1.y - s.y * edge1.x};
    const double v = (direction.x * q.x + direction.y * q.y + direction.z * q.z) * inverse;
    if (v < -1e-6 || u + v > 1.0 + 1e-6) return std::nullopt;
    const double t = (edge2.x * q.x + edge2.y * q.y + edge2.z * q.z) * inverse;
    if (t < 1e-6) return std::nullopt;
    return t;
}

hammer::vmf::Vec3 triangleNormal(const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b,
                                 const hammer::vmf::Vec3& c)
{
    const hammer::vmf::Vec3 edge1{b.x - a.x, b.y - a.y, b.z - a.z};
    const hammer::vmf::Vec3 edge2{c.x - a.x, c.y - a.y, c.z - a.z};
    hammer::vmf::Vec3 normal{edge1.y * edge2.z - edge1.z * edge2.y,
                             edge1.z * edge2.x - edge1.x * edge2.z,
                             edge1.x * edge2.y - edge1.y * edge2.x};
    const double length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (length > 1e-9) {
        normal.x /= length;
        normal.y /= length;
        normal.z /= length;
    }
    return normal;
}

// The material system carries no $surfaceprop, so the surface type comes from
// the material path itself — Source's material directories are named by
// surface, which makes this a solid heuristic.
std::string surfaceFromMaterial(const std::string& material)
{
    std::string lowered = material;
    std::replace(lowered.begin(), lowered.end(), '\\', '/');
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const struct { const char* keyword; const char* surface; } mappings[] = {
        {"metal", "metal"}, {"duct", "duct"}, {"grate", "metalgrate"},
        {"wood", "wood"}, {"plank", "wood"},
        {"dirt", "dirt"}, {"mud", "mud"}, {"ground", "dirt"},
        {"grass", "grass"}, {"gravel", "gravel"}, {"sand", "sand"},
        {"tile", "tile"}, {"glass", "glass"}, {"window", "glass"},
        {"brick", "concrete"}, {"plaster", "drywall"}, {"drywall", "drywall"},
        {"water", "slosh"}, {"snow", "snow"},
    };
    for (const auto& mapping : mappings) {
        if (lowered.find(mapping.keyword) != std::string::npos) return mapping.surface;
    }
    return "concrete";
}

} // namespace

std::string FreemanWindow::firstSound(const std::vector<std::string>& candidates) const
{
    for (const std::string& candidate : candidates) {
        if (soundExists(candidate)) return candidate;
    }
    return {};
}

std::vector<std::string> FreemanWindow::soldierAssetsMissing(
    const hammer::assets::MaterialSystem* materials)
{
    std::vector<std::string> missing;
    const auto fs = materials ? materials->fileSystem() : nullptr;
    const auto require = [&](std::initializer_list<const char*> anyOf) {
        for (const char* path : anyOf) {
            if (fs && fs->exists(path)) return;
        }
        missing.emplace_back(*anyOf.begin());
    };
    require({SoldierViewmodel});
    require({RocketModel});
    require({RocketFireSound});
    require({"sound/weapons/explode1.wav"});
    // TF2 vo lives in mp3s; some mounts nest them under vo/soldier/.
    require({"sound/vo/soldier_painsharp01.mp3", "sound/vo/soldier/soldier_painsharp01.mp3"});
    return missing;
}

FreemanWindow::FreemanWindow(std::shared_ptr<const hammer::vmf::Scene> scene,
                             std::shared_ptr<hammer::assets::MaterialSystem> materials,
                             const hammer::vmf::Vec3& spawnPoint,
                             Mode mode,
                             const hammer::vmf::Document* document,
                             QWidget* parent)
    : QDialog(parent, Qt::Window), materials_(std::move(materials)), spawnPoint_(spawnPoint),
      mode_(mode)
{
    if (mode_ == Mode::Soldier) {
        gravity_ = SoldierGravity;
        walkSpeed_ = SoldierSpeed;
        sprintSpeed_ = SoldierSpeed;  // the soldier does not sprint
    }
    setWindowTitle(mode_ == Mode::Soldier ? tr("Soldier TF2") : tr("The Freeman"));
    resize(1280, 720);

    // Own scene copy with tool brushes stripped: they are neither displayed
    // nor rendered in here. Clip/playerclip brushes are kept aside in an
    // invisible collision-only list before the strip.
    scene_ = std::make_shared<hammer::vmf::Scene>(scene ? *scene : hammer::vmf::Scene{});
    for (const auto& brush : scene_->brushes) {
        if (isPlayerClipBrush(brush)) clipBrushes_.push_back(brush);
    }
    // trigger_teleport: the Scene carries neither brush-entity classnames nor
    // their keyvalues, so the volumes and destinations come from the VMF
    // document, read here once (the document may close while this window
    // lives). Brushes are grabbed before the tool-brush strip below.
    if (document) {
        auto lowerCopy = [](std::string_view value) {
            std::string result(value);
            std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return result;
        };
        const auto parseVec3 = [](const std::string* value, hammer::vmf::Vec3& out) {
            if (!value) return false;
            return std::sscanf(value->c_str(), "%lf %lf %lf", &out.x, &out.y, &out.z) == 3;
        };
        // (trigger entity id -> target name), then resolve each target against
        // the named entities that have a usable origin.
        std::vector<std::pair<int, std::string>> triggers;
        for (const auto& root : document->roots()) {
            if (lowerCopy(root.name) != "entity") continue;
            const std::string* classname = root.value("classname");
            if (!classname || lowerCopy(*classname) != "trigger_teleport") continue;
            const std::string* target = root.value("target");
            const std::string* id = root.value("id");
            if (!target || target->empty() || !id) continue;
            try {
                triggers.emplace_back(std::stoi(*id), lowerCopy(*target));
            } catch (...) {}
        }
        for (const auto& [triggerId, targetName] : triggers) {
            TeleportVolume volume;
            bool resolved = false;
            for (const auto& root : document->roots()) {
                if (lowerCopy(root.name) != "entity") continue;
                const std::string* name = root.value("targetname");
                if (!name || lowerCopy(*name) != targetName) continue;
                if (!parseVec3(root.value("origin"), volume.destination)) continue;
                hammer::vmf::Vec3 angles;
                if (parseVec3(root.value("angles"), angles)) {
                    volume.destinationAngles = angles;
                    volume.hasAngles = true;
                }
                resolved = true;
                break;
            }
            if (!resolved) continue;
            for (const auto& brush : scene_->brushes) {
                if (brush.ownerEntityId == triggerId) volume.brushes.push_back(brush);
            }
            if (!volume.brushes.empty()) teleports_.push_back(std::move(volume));
        }
    }
    scene_->brushes.erase(std::remove_if(scene_->brushes.begin(), scene_->brushes.end(),
                                         [](const hammer::vmf::BrushGeometry& brush) {
                                             return isToolBrush(brush);
                                         }),
                          scene_->brushes.end());

    if (mode_ == Mode::Soldier && materials_ && materials_->fileSystem()) {
        hammer::assets::StudioModelSystem probe(materials_->fileSystem());
        const auto launcher = probe.model(SoldierViewmodel);
        if (launcher && launcher->valid) {
            // Sequence names vary between MDL versions; match by substring
            // over the actual labels instead of guessing exact names.
            const auto findSequence =
                [&](std::initializer_list<const char*> needles)
                -> std::pair<std::string, double> {
                for (const char* needle : needles) {
                    for (const auto& sequence : launcher->sequences) {
                        std::string lowered = sequence.label;
                        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                                       [](unsigned char ch) {
                                           return static_cast<char>(std::tolower(ch));
                                       });
                        if (lowered.find(needle) != std::string::npos) {
                            return {sequence.label, sequence.duration};
                        }
                    }
                }
                return {};
            };
            const auto fire = findSequence({"fire", "attack", "shoot"});
            if (!fire.first.empty()) swingSequences_.push_back(fire);
            const auto idle = findSequence({"idle"});
            if (!idle.first.empty()) idleSequence_ = idle.first;
            // The three-phase reload set; the explicit names must be probed
            // before the bare "reload" fallback, which as a substring would
            // match reload_start too.
            reloadStartSequence_ = findSequence({"reload_start"});
            reloadLoopSequence_ = findSequence({"reload_loop"});
            reloadFinishSequence_ = findSequence({"reload_finish", "reload_end"});
            if (reloadLoopSequence_.first.empty())
                reloadLoopSequence_ = findSequence({"reload"});
            drawSequence_ = findSequence({"draw", "deploy"});
            drawSoundPath_ = firstSound({"sound/weapons/draw_primary.wav",
                                         "sound/weapons/rocket_draw.wav",
                                         "sound/weapons/draw_secondary.wav"});
            hammer::vmf::EntityMarker marker;
            marker.object = {hammer::vmf::ObjectType::Entity, 1999999901};
            marker.id = 1999999901;
            marker.classname = "freeman_viewmodel";
            marker.model = SoldierViewmodel;
            marker.sizeMinimum = {-64.0, -64.0, -64.0};
            marker.sizeMaximum = {64.0, 64.0, 64.0};
            crowbarIndex_ = static_cast<int>(scene_->entities.size());
            scene_->entities.push_back(std::move(marker));
        }
        // A fixed pool of rocket markers, parked far below the map and
        // recycled — the renderer never sees the entity list change size.
        rockets_.resize(8);
        for (std::size_t i = 0; i < rockets_.size(); ++i) {
            hammer::vmf::EntityMarker marker;
            const int id = 1999999910 + static_cast<int>(i);
            marker.object = {hammer::vmf::ObjectType::Entity, id};
            marker.id = id;
            marker.classname = "freeman_rocket";
            marker.model = RocketModel;
            marker.origin = {0.0, 0.0, -32768.0};
            marker.sizeMinimum = {-16.0, -16.0, -16.0};
            marker.sizeMaximum = {16.0, 16.0, 16.0};
            rockets_[i].markerIndex = static_cast<int>(scene_->entities.size());
            scene_->entities.push_back(std::move(marker));
        }
        for (const char* candidate : {"sound/weapons/explode1.wav", "sound/weapons/explode2.wav",
                                      "sound/weapons/explode3.wav"}) {
            if (soundExists(candidate)) explodeSounds_.emplace_back(candidate);
        }
        // TF2 ships Soldier_PainSharp01-05 and Soldier_PainSevere01-06; probe
        // a generous range of both and keep whatever the mount actually has.
        for (const char* stem : {"sound/vo/soldier_painsharp0", "sound/vo/soldier/soldier_painsharp0",
                                 "sound/vo/soldier_painsevere0", "sound/vo/soldier/soldier_painsevere0"}) {
            for (int i = 1; i <= 9; ++i) {
                const std::string candidate = stem + std::to_string(i) + ".mp3";
                if (soundExists(candidate)) painSounds_.push_back(candidate);
            }
        }
    }

    // The crowbar viewmodel, if the mounted game content actually has it
    // (i.e. HL2 assets are reachable through the configured gameinfo).
    if (mode_ == Mode::Freeman && materials_ && materials_->fileSystem()) {
        hammer::assets::StudioModelSystem probe(materials_->fileSystem());
        const auto crowbar = probe.model("models/weapons/v_crowbar.mdl");
        if (crowbar && crowbar->valid) {
            // The actual swing/hit sequences and their real durations.
            for (const char* name : {"misscenter1", "misscenter2"}) {
                const int index = crowbar->sequenceIndex(name);
                if (index >= 0 && index < crowbar->sequenceCount()) {
                    swingSequences_.emplace_back(
                        name, crowbar->sequences[static_cast<std::size_t>(index)].duration);
                }
            }
            for (const char* name : {"hitcenter1", "hitcenter2", "hitcenter3"}) {
                const int index = crowbar->sequenceIndex(name);
                if (index >= 0 && index < crowbar->sequenceCount()) {
                    hitSequences_.emplace_back(
                        name, crowbar->sequences[static_cast<std::size_t>(index)].duration);
                }
            }
            hammer::vmf::EntityMarker marker;
            marker.object = {hammer::vmf::ObjectType::Entity, 1999999901};
            marker.id = 1999999901;
            marker.classname = "freeman_viewmodel";
            marker.model = "models/weapons/v_crowbar.mdl";
            marker.sizeMinimum = {-64.0, -64.0, -64.0};
            marker.sizeMaximum = {64.0, 64.0, 64.0};
            crowbarIndex_ = static_cast<int>(scene_->entities.size());
            scene_->entities.push_back(std::move(marker));
        }
    }

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    view_ = new MapViewWidget(MapViewWidget::Kind::Perspective, this);
    view_->setGridVisible(false);
    view_->setMaterialSystem(materials_);
    // Full advanced material preview: shaded polygons with phong, specular,
    // bump maps, and the rest of the effect stack.
    view_->setTexturedRenderMode(MapViewWidget::TexturedRenderMode::ShadedMaterialPolygons);
    view_->setScene(scene_, false);
    layout->addWidget(view_);
    view_->installEventFilter(this);
    view_->setFocus();

    // Attacking requires both the crowbar model and the HL2 swing sound; the
    // other sounds each degrade independently when missing.
    // HL2's crowbar swing is the ice axe sound (Weapon_Crowbar.Single ->
    // weapons/iceaxe/iceaxe_swing1.wav); older/other mounts may carry the
    // HL1-style crowbar_swing files instead.
    for (const char* candidate : {"sound/weapons/iceaxe/iceaxe_swing1.wav",
                                  "sound/weapons/crowbar/crowbar_swing1.wav"}) {
        if (soundExists(candidate)) {
            swingSoundPath_ = candidate;
            break;
        }
    }
    swingSoundAvailable_ = !swingSoundPath_.empty();
    // Weapon_Crowbar.Melee_Hit: crowbar_impact1/2, when the mount has them.
    for (const char* candidate : {"sound/weapons/crowbar/crowbar_impact1.wav",
                                  "sound/weapons/crowbar/crowbar_impact2.wav"}) {
        if (soundExists(candidate)) impactSounds_.emplace_back(candidate);
    }
    footstepsAvailable_ = soundExists("sound/player/footsteps/concrete1.wav");

    spawn();

    // The G-Man greets a fresh deployment, when HL2's voice files are
    // mounted; the Soldier announces himself instead.
    if (mode_ == Mode::Soldier) {
        const std::string cry = firstSound({"sound/vo/soldier_battlecry01.mp3",
                                            "sound/vo/soldier/soldier_battlecry01.mp3"});
        if (!cry.empty()) playSound(cry);
    } else if (soundExists("sound/vo/gman_misc/gman_riseshine.wav")) {
        playSound("sound/vo/gman_misc/gman_riseshine.wav");
    }

    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::PreciseTimer);
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, &FreemanWindow::tick);
    clock_.start();
    timer_->start();
    setCaptured(true);
}

FreemanWindow::~FreemanWindow() = default;

void FreemanWindow::spawn()
{
    // The Freeman deploys at the selected point entity's location.
    feet_ = spawnPoint_;
    velocity_ = {};
    onGround_ = false;
    ducked_ = false;
    eyeHeight_ = StandEyeHeight;
    // Drop the hull to the floor below the spawn point.
    const HullTrace drop = traceHull(feet_, {0.0, 0.0, -16384.0}, StandHullHeight);
    if (drop.hit) feet_.z -= 16384.0 * drop.fraction;
    view_->cameraState_.position = {feet_.x, feet_.y, feet_.z + eyeHeight_};

    if (mode_ == Mode::Soldier) {
        // Fresh launcher: full clip, no reload in progress, and the draw
        // animation + sfx (also on F1 respawn), via the one-shot channel.
        rocketClip_ = RocketClipSize;
        fireCooldown_ = 0.0;
        reloadPhase_ = ReloadPhase::Idle;
        if (!drawSequence_.first.empty()) {
            swingSequence_ = drawSequence_.first;
            swingDuration_ = std::max(0.1, drawSequence_.second);
            swingRemaining_ = swingDuration_;
        }
        if (!drawSoundPath_.empty()) playSound(drawSoundPath_);
    }
}

bool FreemanWindow::soundExists(const std::string& path) const
{
    return materials_ && materials_->fileSystem() && materials_->fileSystem()->exists(path);
}

void FreemanWindow::playSound(const std::string& path)
{
    if (!soundExists(path)) return;
    auto cached = soundCache_.find(path);
    if (cached == soundCache_.end()) {
        const auto bytes = materials_->fileSystem()->readFile(path);
        if (!bytes) return;
        QString name = QString::fromStdString(path);
        name.replace(QLatin1Char('/'), QLatin1Char('_'));
        const QString file = QDir::temp().filePath(QStringLiteral("hammer-freeman-") + name);
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly)) return;
        out.write(reinterpret_cast<const char*>(bytes->data()),
                  static_cast<qint64>(bytes->size()));
        out.close();
        cached = soundCache_.emplace(path, file).first;
    }
    // Detached playback keeps overlapping sounds and never blocks the tick.
    // TF2 voice lines are mp3s, which paplay/aplay cannot decode.
    if (cached->second.endsWith(QLatin1String(".mp3"), Qt::CaseInsensitive)) {
        if (!QProcess::startDetached(QStringLiteral("mpg123"),
                                     {QStringLiteral("-q"), cached->second})) {
            QProcess::startDetached(
                QStringLiteral("ffplay"),
                {QStringLiteral("-nodisp"), QStringLiteral("-autoexit"),
                 QStringLiteral("-loglevel"), QStringLiteral("quiet"), cached->second});
        }
        return;
    }
    if (!QProcess::startDetached(QStringLiteral("paplay"), {cached->second})) {
        QProcess::startDetached(QStringLiteral("aplay"), {QStringLiteral("-q"), cached->second});
    }
}

std::optional<double> FreemanWindow::castRay(const hammer::vmf::Vec3& origin,
                                             const hammer::vmf::Vec3& direction,
                                             double maxDistance,
                                             hammer::vmf::Vec3* hitNormal,
                                             std::string* hitMaterial) const
{
    std::optional<double> nearest;
    for (const auto& brush : scene_->brushes) {
        for (const auto& face : brush.faces) {
            if (isToolMaterial(face.material)) continue;
            // Source collides with the displacement surface, not the flat
            // face it replaced.
            if (hammer::vmf::isFaceMaskedByDisplacementSolid(brush, face, true)) continue;
            const auto testTriangle = [&](const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b,
                                          const hammer::vmf::Vec3& c) {
                const auto distance = rayTriangle(origin, direction, a, b, c);
                if (!distance || *distance > maxDistance) return;
                if (nearest && *distance >= *nearest) return;
                nearest = *distance;
                if (hitMaterial) *hitMaterial = face.material;
                if (hitNormal) {
                    *hitNormal = triangleNormal(a, b, c);
                    // Face the ray.
                    if (hitNormal->x * direction.x + hitNormal->y * direction.y +
                            hitNormal->z * direction.z > 0.0) {
                        hitNormal->x = -hitNormal->x;
                        hitNormal->y = -hitNormal->y;
                        hitNormal->z = -hitNormal->z;
                    }
                }
            };
            if (face.displacement && face.displacementIndices.size() >= 3) {
                for (std::size_t index = 0; index + 2 < face.displacementIndices.size();
                     index += 3) {
                    const std::size_t a = face.displacementIndices[index];
                    const std::size_t b = face.displacementIndices[index + 1];
                    const std::size_t c = face.displacementIndices[index + 2];
                    if (a < face.displacementVertices.size() &&
                        b < face.displacementVertices.size() &&
                        c < face.displacementVertices.size()) {
                        testTriangle(face.displacementVertices[a].position,
                                     face.displacementVertices[b].position,
                                     face.displacementVertices[c].position);
                    }
                }
            } else if (face.vertices.size() >= 3) {
                const std::size_t first = face.vertices.front();
                if (first >= brush.vertices.size()) continue;
                for (std::size_t index = 1; index + 1 < face.vertices.size(); ++index) {
                    const std::size_t b = face.vertices[index];
                    const std::size_t c = face.vertices[index + 1];
                    if (b < brush.vertices.size() && c < brush.vertices.size())
                        testTriangle(brush.vertices[first], brush.vertices[b],
                                     brush.vertices[c]);
                }
            }
        }
    }
    return nearest;
}

namespace {

// Support distance of the feet-origin player box (x/y +-16, z 0..H) along a
// plane normal — how far a plane must move out to sweep the box as a point.
double hullSupport(const hammer::vmf::Vec3& normal, double hullHeight)
{
    return HullRadius * (std::abs(normal.x) + std::abs(normal.y)) +
           std::max(0.0, -normal.z) * hullHeight;
}

struct SweptBounds
{
    hammer::vmf::Vec3 min;
    hammer::vmf::Vec3 max;
    bool overlaps(const hammer::vmf::Vec3& lo, const hammer::vmf::Vec3& hi) const
    {
        return min.x <= hi.x && max.x >= lo.x && min.y <= hi.y && max.y >= lo.y &&
               min.z <= hi.z && max.z >= lo.z;
    }
};

// AABB of the whole swept hull (start->end, expanded by the box) plus a pad.
SweptBounds sweptBounds(const hammer::vmf::Vec3& start, const hammer::vmf::Vec3& end,
                        double hullHeight)
{
    SweptBounds bounds;
    bounds.min = {std::min(start.x, end.x) - HullRadius - 1.0,
                  std::min(start.y, end.y) - HullRadius - 1.0,
                  std::min(start.z, end.z) - 1.0};
    bounds.max = {std::max(start.x, end.x) + HullRadius + 1.0,
                  std::max(start.y, end.y) + HullRadius + 1.0,
                  std::max(start.z, end.z) + hullHeight + 1.0};
    return bounds;
}

// Sweeps the player box along start->end against a convex point set: the
// Minkowski sum's faces come exactly from the solid's face normals, the box's
// six axial normals, and every solid-edge x box-axis cross (QBSP's bevel
// planes). Each candidate is used in both signs with its own supporting
// distance (max normal.point), so orientation never matters.
class HullClipper
{
public:
    HullClipper(const hammer::vmf::Vec3& start, const hammer::vmf::Vec3& end,
                double hullHeight)
        : start_(start), end_(end), hullHeight_(hullHeight)
    {
    }

    // Returns false once the sweep provably misses the solid.
    bool clip(const hammer::vmf::Vec3& normal, const hammer::vmf::Vec3* points,
              std::size_t pointCount)
    {
        const double length = std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                        normal.z * normal.z);
        if (length < 1e-9) return true;
        const hammer::vmf::Vec3 unit{normal.x / length, normal.y / length,
                                     normal.z / length};
        if (!clipOneSign(unit, points, pointCount)) return false;
        return clipOneSign({-unit.x, -unit.y, -unit.z}, points, pointCount);
    }

    bool finish(FreemanWindow::HullTrace& result) const
    {
        if (missed_) return false;
        if (!outsideSomewhere_) {
            // Started inside the expanded solid: report it, but do not trap
            // the player — the mover treats start-solid solids as passable.
            result.startSolid = true;
            return false;
        }
        if (enterFraction_ > -1.0 && enterFraction_ < exitFraction_ &&
            enterFraction_ < result.fraction) {
            result.fraction = std::max(0.0, enterFraction_);
            result.normal = enterNormal_;
            result.hit = true;
            return true;
        }
        return false;
    }

private:
    bool clipOneSign(const hammer::vmf::Vec3& normal, const hammer::vmf::Vec3* points,
                     std::size_t pointCount)
    {
        double planeDistance = -1e300;
        for (std::size_t i = 0; i < pointCount; ++i) {
            planeDistance = std::max(planeDistance, normal.x * points[i].x +
                                                        normal.y * points[i].y +
                                                        normal.z * points[i].z);
        }
        planeDistance += hullSupport(normal, hullHeight_);
        const double startDot =
            normal.x * start_.x + normal.y * start_.y + normal.z * start_.z - planeDistance;
        const double endDot =
            normal.x * end_.x + normal.y * end_.y + normal.z * end_.z - planeDistance;
        // Quake/Source CM_ClipBoxToBrush conditions. The miss test must be
        // "in front AND not approaching" — a move that drifts toward the
        // plane but ends still (barely) outside must register as an enter at
        // fraction 0 so the velocity gets clipped. The old "both dots
        // positive" test silently ate the 0.03125 resting gap tick by tick
        // until the hull was inside the solid and fell through (start-solid
        // brushes are passable).
        if (startDot > 0.0 && endDot >= startDot) {
            missed_ = true;
            return false;
        }
        if (startDot <= 0.0 && endDot <= 0.0) return true;
        if (startDot > 0.0) outsideSomewhere_ = true;
        if (startDot > endDot) {
            const double fraction = (startDot - 0.03125) / (startDot - endDot);
            if (fraction > enterFraction_) {
                enterFraction_ = fraction;
                enterNormal_ = normal;
            }
        } else {
            exitFraction_ =
                std::min(exitFraction_, (startDot + 0.03125) / (startDot - endDot));
        }
        return true;
    }

    hammer::vmf::Vec3 start_;
    hammer::vmf::Vec3 end_;
    double hullHeight_;
    double enterFraction_{-1.0};
    double exitFraction_{1.0};
    hammer::vmf::Vec3 enterNormal_{};
    bool outsideSomewhere_{false};
    bool missed_{false};
};

constexpr hammer::vmf::Vec3 BoxAxes[3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};

// HAMMER_FREEMAN_DEBUG=1 streams movement anomalies (velocity kills, crease
// slides, step-moves) to stderr for diagnosing collision misbehavior.
bool freemanDebug()
{
    static const bool enabled = qEnvironmentVariableIsSet("HAMMER_FREEMAN_DEBUG");
    return enabled;
}

}  // namespace

FreemanWindow::HullTrace FreemanWindow::traceHull(const hammer::vmf::Vec3& start,
                                                 const hammer::vmf::Vec3& delta,
                                                 double hullHeight) const
{
    HullTrace result;
    const hammer::vmf::Vec3 end{start.x + delta.x, start.y + delta.y, start.z + delta.z};
    const SweptBounds sweep = sweptBounds(start, end, hullHeight);

    // World brushes plus the invisible clip/playerclip hulls.
    const std::vector<hammer::vmf::BrushGeometry>* const brushLists[] = {&scene_->brushes,
                                                                         &clipBrushes_};
    for (const auto* brushList : brushLists)
    for (const auto& brush : *brushList) {
        if (brush.hasDisplacement) {
            // Displacement solids collide as their sculpted surface: each
            // triangle is a (thin) convex solid swept with the same clipper.
            for (const auto& face : brush.faces) {
                if (!face.displacement || face.displacementIndices.size() < 3) continue;
                for (std::size_t index = 0; index + 2 < face.displacementIndices.size();
                     index += 3) {
                    const std::size_t a = face.displacementIndices[index];
                    const std::size_t b = face.displacementIndices[index + 1];
                    const std::size_t c = face.displacementIndices[index + 2];
                    if (a >= face.displacementVertices.size() ||
                        b >= face.displacementVertices.size() ||
                        c >= face.displacementVertices.size()) {
                        continue;
                    }
                    const hammer::vmf::Vec3 triangle[3] = {
                        face.displacementVertices[a].position,
                        face.displacementVertices[b].position,
                        face.displacementVertices[c].position};
                    const hammer::vmf::Vec3 lo{
                        std::min({triangle[0].x, triangle[1].x, triangle[2].x}),
                        std::min({triangle[0].y, triangle[1].y, triangle[2].y}),
                        std::min({triangle[0].z, triangle[1].z, triangle[2].z})};
                    const hammer::vmf::Vec3 hi{
                        std::max({triangle[0].x, triangle[1].x, triangle[2].x}),
                        std::max({triangle[0].y, triangle[1].y, triangle[2].y}),
                        std::max({triangle[0].z, triangle[1].z, triangle[2].z})};
                    if (!sweep.overlaps(lo, hi)) continue;
                    hammer::vmf::Vec3 faceNormal =
                        triangleNormal(triangle[0], triangle[1], triangle[2]);
                    // Orient the face normal toward the trace start — the
                    // reattribution gate below compares approach against the
                    // side actually being ridden, so ramp sides and
                    // undersides test against the correct hemisphere (a
                    // blind +z flip broke the gate anywhere not ridden from
                    // above).
                    if ((start.x - triangle[0].x) * faceNormal.x +
                            (start.y - triangle[0].y) * faceNormal.y +
                            (start.z - triangle[0].z) * faceNormal.z < 0.0) {
                        faceNormal = {-faceNormal.x, -faceNormal.y, -faceNormal.z};
                    }
                    HullClipper clipper(start, end, hullHeight);
                    bool alive = clipper.clip(faceNormal, triangle, 3);
                    for (int axis = 0; alive && axis < 3; ++axis) {
                        alive = clipper.clip(BoxAxes[axis], triangle, 3);
                    }
                    for (int edge = 0; alive && edge < 3; ++edge) {
                        const hammer::vmf::Vec3& from = triangle[edge];
                        const hammer::vmf::Vec3& to = triangle[(edge + 1) % 3];
                        const hammer::vmf::Vec3 direction{to.x - from.x, to.y - from.y,
                                                          to.z - from.z};
                        for (int axis = 0; alive && axis < 3; ++axis) {
                            const hammer::vmf::Vec3& box = BoxAxes[axis];
                            const hammer::vmf::Vec3 bevel{
                                direction.y * box.z - direction.z * box.y,
                                direction.z * box.x - direction.x * box.z,
                                direction.x * box.y - direction.y * box.x};
                            alive = clipper.clip(bevel, triangle, 3);
                        }
                    }
                    // Thin-solid seam fix: adjacent triangles' lateral planes
                    // (prism bevels/axials) get crossed at almost the same
                    // fraction as the face plane, so enter selection can
                    // arbitrarily report a phantom "wall" at every triangle
                    // seam — stopping walkers on slopes and killing surf
                    // momentum on steep ramps (Source's infamous ramp bug).
                    // The fraction is right either way, but the slide logic
                    // must see the surface plane: any non-face contact normal
                    // is re-attributed to the face normal as long as the move
                    // actually approaches the face from its front side.
                    if (alive && clipper.finish(result)) {
                        result.fromDisplacement = true;
                        result.solidId = brush.id;
                        const double align = result.normal.x * faceNormal.x +
                                             result.normal.y * faceNormal.y +
                                             result.normal.z * faceNormal.z;
                        const double approach = delta.x * faceNormal.x +
                                                delta.y * faceNormal.y +
                                                delta.z * faceNormal.z;
                        if (align < 0.99) {
                            if (approach < 0.0) {
                                result.normal = faceNormal;
                            } else if (freemanDebug()) {
                                fprintf(stderr,
                                        "[freeman] reattr DECLINED n=(%.3f %.3f %.3f) "
                                        "face=(%.3f %.3f %.3f) approach=%.3f\n",
                                        result.normal.x, result.normal.y, result.normal.z,
                                        faceNormal.x, faceNormal.y, faceNormal.z, approach);
                            }
                        }
                    }
                }
            }
            continue;
        }

        if (brush.vertices.empty()) continue;
        hammer::vmf::Vec3 lo = brush.vertices.front();
        hammer::vmf::Vec3 hi = lo;
        for (const auto& vertex : brush.vertices) {
            lo = {std::min(lo.x, vertex.x), std::min(lo.y, vertex.y), std::min(lo.z, vertex.z)};
            hi = {std::max(hi.x, vertex.x), std::max(hi.y, vertex.y), std::max(hi.z, vertex.z)};
        }
        if (!sweep.overlaps(lo, hi)) continue;

        HullClipper clipper(start, end, hullHeight);
        bool alive = true;
        // The brush's own face planes.
        for (const auto& face : brush.faces) {
            if (!alive) break;
            alive = clipper.clip(face.normal, brush.vertices.data(), brush.vertices.size());
        }
        // The box's axial planes.
        for (int axis = 0; alive && axis < 3; ++axis) {
            alive = clipper.clip(BoxAxes[axis], brush.vertices.data(), brush.vertices.size());
        }
        // Edge bevels: every face-loop edge crossed with every box axis.
        for (const auto& face : brush.faces) {
            if (!alive) break;
            for (std::size_t i = 0; alive && i < face.vertices.size(); ++i) {
                const std::size_t fromIndex = face.vertices[i];
                const std::size_t toIndex = face.vertices[(i + 1) % face.vertices.size()];
                if (fromIndex >= brush.vertices.size() || toIndex >= brush.vertices.size())
                    continue;
                const hammer::vmf::Vec3& from = brush.vertices[fromIndex];
                const hammer::vmf::Vec3& to = brush.vertices[toIndex];
                const hammer::vmf::Vec3 direction{to.x - from.x, to.y - from.y, to.z - from.z};
                for (int axis = 0; alive && axis < 3; ++axis) {
                    const hammer::vmf::Vec3& box = BoxAxes[axis];
                    const hammer::vmf::Vec3 bevel{direction.y * box.z - direction.z * box.y,
                                                  direction.z * box.x - direction.x * box.z,
                                                  direction.x * box.y - direction.y * box.x};
                    alive = clipper.clip(bevel, brush.vertices.data(), brush.vertices.size());
                }
            }
        }
        if (alive && clipper.finish(result)) {
            result.fromDisplacement = false;
            result.solidId = brush.id;
            // Flush-seam fix for abutting brushes (Source's BSP merges these
            // at compile time; raw VMF brushes keep their buried end-cap
            // faces). A wall-ish contact is probed 2 units further along the
            // motion: if the probe is NOT inside this brush but rides within
            // epsilon of one of its other planes, the "wall" is a buried
            // seam face and the contact is re-attributed to the surface
            // plane actually being ridden. A real wall keeps blocking — the
            // probe lands strictly inside it.
            if (result.normal.z < 0.7 && result.normal.z > -0.1) {
                const double deltaLength = std::sqrt(delta.x * delta.x + delta.y * delta.y +
                                                     delta.z * delta.z);
                if (deltaLength > 1e-9) {
                    const hammer::vmf::Vec3 probe{
                        start.x + delta.x * result.fraction + delta.x / deltaLength * 2.0,
                        start.y + delta.y * result.fraction + delta.y / deltaLength * 2.0,
                        start.z + delta.z * result.fraction + delta.z / deltaLength * 2.0};
                    double surfaceMax = -1e300;
                    hammer::vmf::Vec3 surfaceNormal{};
                    const auto probePlane = [&](hammer::vmf::Vec3 normal) {
                        const double length =
                            std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                      normal.z * normal.z);
                        if (length < 1e-9) return;
                        normal = {normal.x / length, normal.y / length, normal.z / length};
                        double planeDistance = -1e300;
                        for (const auto& vertex : brush.vertices) {
                            planeDistance = std::max(planeDistance,
                                                     normal.x * vertex.x + normal.y * vertex.y +
                                                         normal.z * vertex.z);
                        }
                        planeDistance += hullSupport(normal, hullHeight);
                        const double distance = normal.x * probe.x + normal.y * probe.y +
                                                normal.z * probe.z - planeDistance;
                        if (distance > surfaceMax) {
                            surfaceMax = distance;
                            surfaceNormal = normal;
                        }
                    };
                    for (const auto& face : brush.faces) probePlane(face.normal);
                    for (const auto& axis : BoxAxes) {
                        probePlane(axis);
                        probePlane({-axis.x, -axis.y, -axis.z});
                    }
                    const double align = surfaceNormal.x * result.normal.x +
                                         surfaceNormal.y * result.normal.y +
                                         surfaceNormal.z * result.normal.z;
                    if (surfaceMax > -0.001 && surfaceMax < 0.1 && align < 0.99) {
                        if (freemanDebug()) {
                            fprintf(stderr,
                                    "[freeman] brush-seam reattr n=(%.3f %.3f %.3f) -> "
                                    "(%.3f %.3f %.3f) brush%d\n",
                                    result.normal.x, result.normal.y, result.normal.z,
                                    surfaceNormal.x, surfaceNormal.y, surfaceNormal.z,
                                    brush.id);
                        }
                        result.normal = surfaceNormal;
                    }
                }
            }
        }
    }
    return result;
}

void FreemanWindow::tick()
{
    // Nanosecond clock: restart() only returns whole milliseconds, and the
    // 16/17 ms alternation that quantization causes reads as constant jitter
    // in the bob and movement.
    const double dt = std::clamp(clock_.nsecsElapsed() / 1e9, 0.0, 0.05);
    clock_.restart();
    if (dt <= 0.0) return;

    // Flattened camera basis, so W walks where the view faces regardless of
    // pitch (walking, not flying).
    hammer::vmf::Vec3 forward = hammer::camera::forwardVector(view_->cameraState_);
    forward.z = 0.0;
    const double forwardLength = std::sqrt(forward.x * forward.x + forward.y * forward.y);
    if (forwardLength > 1e-6) {
        forward.x /= forwardLength;
        forward.y /= forwardLength;
    }
    hammer::vmf::Vec3 right = hammer::camera::rightVector(view_->cameraState_);
    right.z = 0.0;
    const double rightLength = std::sqrt(right.x * right.x + right.y * right.y);
    if (rightLength > 1e-6) {
        right.x /= rightLength;
        right.y /= rightLength;
    }

    double wishX = 0.0;
    double wishY = 0.0;
    if (keys_.contains(Qt::Key_W)) { wishX += forward.x; wishY += forward.y; }
    if (keys_.contains(Qt::Key_S)) { wishX -= forward.x; wishY -= forward.y; }
    if (keys_.contains(Qt::Key_D)) { wishX += right.x; wishY += right.y; }
    if (keys_.contains(Qt::Key_A)) { wishX -= right.x; wishY -= right.y; }
    const double wishLength = std::sqrt(wishX * wishX + wishY * wishY);
    if (wishLength > 1e-6) {
        wishX /= wishLength;
        wishY /= wishLength;
    }
    // Duck (CGameMovement::Duck, simplified: instant hull, smoothed eye).
    // Crouching in mid-air tucks the legs UP: Source shrinks the hull from
    // the bottom while airborne, which is exactly what makes crouch-jumping
    // clear higher ledges. With a feet-based origin that means the feet rise
    // by the hull difference on an air-duck and drop back on an air-unduck.
    const bool wantDuck = keys_.contains(Qt::Key_Control);
    if (wantDuck != ducked_) {
        const double tuck = StandHullHeight - DuckHullHeight;
        if (wantDuck) {
            // Ducking is always allowed: the duck hull occupies a subset of
            // the stand hull's volume (same top, legs tucked up).
            if (!onGround_) feet_.z += tuck;
            ducked_ = true;
        } else {
            // Unducking must be traced (Source's CanUnduck): airborne, the
            // feet drop back down only as far as the world allows — the old
            // blind teleport embedded the hull in the floor when un-crouching
            // (or crouching then landing) within a leg's length of it, and a
            // start-solid hull falls through. The stand hull must also fit
            // (an upward duck-hull sweep covers exactly the stand hull), or
            // we stay ducked and retry next tick.
            hammer::vmf::Vec3 candidate = feet_;
            bool blocked = false;
            if (!onGround_) {
                const HullTrace drop = traceHull(feet_, {0.0, 0.0, -tuck}, DuckHullHeight);
                blocked = drop.startSolid;
                candidate.z -= tuck * drop.fraction;
            }
            const HullTrace fit = traceHull(candidate, {0.0, 0.0, tuck}, DuckHullHeight);
            if (!blocked && !fit.startSolid && !fit.hit) {
                feet_ = candidate;
                ducked_ = false;
            }
        }
    }
    const double hullHeight = ducked_ ? DuckHullHeight : StandHullHeight;
    // Instant crouch: no eye-height smoothing. The lerp read as slowed
    // gravity mid-air (feet tuck up instantly, camera drifted down after).
    eyeHeight_ = ducked_ ? DuckEyeHeight : StandEyeHeight;

    double wishSpeed = keys_.contains(Qt::Key_Shift) ? sprintSpeed_ : walkSpeed_;
    if (ducked_) wishSpeed *= DuckSpeedFraction;

    // CGameMovement::FullWalkMove order: StartGravity (half a tick), the jump
    // check, THEN friction — so the jump tick never eats ground friction.
    if (!onGround_) velocity_.z -= gravity_ * dt * 0.5;
    // HL2 has no auto-hop: CheckJumpButton bails while IN_JUMP was already
    // down last tick, so a held Space only jumps once per press.
    const bool jumpDown = keys_.contains(Qt::Key_Space);
    if (jumpDown && !jumpHeld_ && onGround_) {
        velocity_.z = jumpVelocity();
        onGround_ = false;
    }
    jumpHeld_ = jumpDown;
    // For StayOnGround below: grounded entering the move, and not launched
    // by the jump impulse this tick.
    const bool groundedAtMoveStart = onGround_;

    // Ground friction (CGameMovement::Friction with sv_stopspeed).
    if (onGround_) {
        const double speed = std::sqrt(velocity_.x * velocity_.x + velocity_.y * velocity_.y);
        if (speed > 0.1) {
            const double control = std::max(speed, StopSpeed);
            const double drop = control * Friction * dt;
            const double scale = std::max(0.0, (speed - drop) / speed);
            velocity_.x *= scale;
            velocity_.y *= scale;
        } else {
            velocity_.x = 0.0;
            velocity_.y = 0.0;
        }
    }

    // Accelerate (CGameMovement::Accelerate / AirAccelerate).
    if (wishLength > 1e-6) {
        const double cap = onGround_ ? wishSpeed : std::min(wishSpeed, AirSpeedCap);
        const double current = velocity_.x * wishX + velocity_.y * wishY;
        const double addSpeed = cap - current;
        if (addSpeed > 0.0) {
            const double airAccel = airAccelHigh_ ? HighAirAccelerate : AirAccelerate;
            const double accelSpeed =
                std::min((onGround_ ? Accelerate : airAccel) * wishSpeed * dt, addSpeed);
            velocity_.x += wishX * accelSpeed;
            velocity_.y += wishY * accelSpeed;
        }
    }

    // CGameMovement::CheckVelocity.
    velocity_.x = std::clamp(velocity_.x, -MaxVelocity, MaxVelocity);
    velocity_.y = std::clamp(velocity_.y, -MaxVelocity, MaxVelocity);
    velocity_.z = std::clamp(velocity_.z, -MaxVelocity, MaxVelocity);

    // Collide-and-slide with the full HL2 hull (CGameMovement::TryPlayerMove),
    // with a Source-style step attempt when a steep plane blocks a grounded
    // move.
    {
        hammer::vmf::Vec3 position = feet_;
        double timeLeft = dt;
        // TryPlayerMove keeps every plane hit this move (MAX_CLIP_PLANES = 5)
        // and re-clips against all of them, so a crease between two walls
        // slides along their intersection instead of re-entering the first
        // plane and jittering.
        const hammer::vmf::Vec3 primalVelocity = velocity_;
        // Source's per-plane clips always start from the velocity as of the
        // last covered distance — never from an already-clipped velocity.
        // Re-clipping the clipped vector double-projects on back-to-back
        // zero-fraction contacts (slope + bevel) and bleeds speed into the
        // crease/kill paths.
        hammer::vmf::Vec3 originalVelocity = velocity_;
        hammer::vmf::Vec3 planes[5];
        int planeCount = 0;
        for (int bump = 0; bump < 4 && timeLeft > 1e-6; ++bump) {
            const hammer::vmf::Vec3 delta{velocity_.x * timeLeft, velocity_.y * timeLeft,
                                          velocity_.z * timeLeft};
            if (std::abs(delta.x) < 1e-9 && std::abs(delta.y) < 1e-9 &&
                std::abs(delta.z) < 1e-9) {
                break;
            }
            const HullTrace trace = traceHull(position, delta, hullHeight);
            position.x += delta.x * trace.fraction;
            position.y += delta.y * trace.fraction;
            position.z += delta.z * trace.fraction;
            // Covering any distance restarts the plane list — stale planes
            // from contacts we've slid away from must not join fresh ones.
            if (trace.fraction > 0.0) {
                planeCount = 0;
                originalVelocity = velocity_;
            }
            if (!trace.hit) {
                timeLeft = 0.0;
                break;
            }
            timeLeft *= 1.0 - trace.fraction;
            // Steep plane while grounded: try the StepMove — up a step, over,
            // back down — before surrendering the velocity to the slide. Like
            // Source's StepMove, the step only wins if it actually travels
            // farther than sliding along the blocking plane would; otherwise
            // a grazing zero-fraction wall contact (bevels at brush seams)
            // would trigger an 18-unit up-over-down teleport mid-slope.
            if (onGround_ && trace.normal.z < 0.7 && trace.normal.z > -0.1) {
                const double slideInto = velocity_.x * trace.normal.x +
                                         velocity_.y * trace.normal.y +
                                         velocity_.z * trace.normal.z;
                const double slideX = velocity_.x - trace.normal.x * std::min(0.0, slideInto);
                const double slideY = velocity_.y - trace.normal.y * std::min(0.0, slideInto);
                const double slideDistance =
                    std::sqrt(slideX * slideX + slideY * slideY) * timeLeft;
                const HullTrace up = traceHull(position, {0.0, 0.0, StepHeight}, hullHeight);
                const double upDistance = StepHeight * up.fraction;
                if (upDistance > 0.5) {
                    hammer::vmf::Vec3 stepped{position.x, position.y, position.z + upDistance};
                    const hammer::vmf::Vec3 horizontal{velocity_.x * timeLeft,
                                                       velocity_.y * timeLeft, 0.0};
                    const HullTrace over = traceHull(stepped, horizontal, hullHeight);
                    const double overDistance =
                        std::sqrt(horizontal.x * horizontal.x + horizontal.y * horizontal.y) *
                        over.fraction;
                    if (over.fraction > 0.1 && overDistance > slideDistance + 0.01) {
                        stepped.x += horizontal.x * over.fraction;
                        stepped.y += horizontal.y * over.fraction;
                        const HullTrace down =
                            traceHull(stepped, {0.0, 0.0, -upDistance}, hullHeight);
                        stepped.z -= upDistance * down.fraction;
                        if (down.hit && down.normal.z >= 0.7) {
                            if (freemanDebug())
                                fprintf(stderr, "[freeman] STEPMOVE up=%.2f over=%.2f\n",
                                        upDistance, overDistance);
                            position = stepped;
                            timeLeft *= 1.0 - over.fraction;
                            planeCount = 0;
                            originalVelocity = velocity_;
                            continue;
                        }
                    }
                }
            }
            // Repeat contact with a plane we already clipped against: the
            // world here is many separate solids (brush per face, triangle
            // per displacement cell), so coplanar neighbors each report the
            // same plane at fraction 0. Storing the duplicate would turn the
            // crease slide into a degenerate zero-cross kill. Quake 3's
            // PM_SlideMove handles this by nudging the velocity out along
            // the normal and retrying instead.
            {
                bool duplicate = false;
                for (int existing = 0; existing < planeCount; ++existing) {
                    if (trace.normal.x * planes[existing].x +
                            trace.normal.y * planes[existing].y +
                            trace.normal.z * planes[existing].z > 0.99) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) {
                    if (freemanDebug()) {
                        fprintf(stderr, "[freeman] dup-nudge n=(%.3f %.3f %.3f)\n",
                                trace.normal.x, trace.normal.y, trace.normal.z);
                    }
                    velocity_.x += trace.normal.x;
                    velocity_.y += trace.normal.y;
                    velocity_.z += trace.normal.z;
                    continue;
                }
            }
            // Too many planes this move: wedged, stop dead.
            if (planeCount >= 5) {
                if (freemanDebug()) fprintf(stderr, "[freeman] KILL: 5 planes\n");
                velocity_ = {0.0, 0.0, 0.0};
                break;
            }
            planes[planeCount++] = trace.normal;
            const double debugSpeedBefore =
                std::sqrt(velocity_.x * velocity_.x + velocity_.y * velocity_.y);

            // Try clipping the velocity to each stored plane in turn,
            // accepting the first result that moves away from all of them.
            const auto dot = [](const hammer::vmf::Vec3& a, const hammer::vmf::Vec3& b) {
                return a.x * b.x + a.y * b.y + a.z * b.z;
            };
            int clipped = 0;
            for (; clipped < planeCount; ++clipped) {
                hammer::vmf::Vec3 candidate = originalVelocity;
                const double into = dot(candidate, planes[clipped]);
                candidate.x -= planes[clipped].x * into;
                candidate.y -= planes[clipped].y * into;
                candidate.z -= planes[clipped].z * into;
                int other = 0;
                for (; other < planeCount; ++other) {
                    if (other != clipped && dot(candidate, planes[other]) < 0.0) break;
                }
                if (other == planeCount) {
                    velocity_ = candidate;
                    break;
                }
            }
            if (clipped == planeCount) {
                // No single plane works. Two planes form a crease: slide
                // along their intersection line. Three or more: stop.
                if (planeCount != 2) {
                    if (freemanDebug())
                        fprintf(stderr, "[freeman] KILL: %d planes unclippable\n", planeCount);
                    velocity_ = {0.0, 0.0, 0.0};
                    break;
                }
                hammer::vmf::Vec3 crease{
                    planes[0].y * planes[1].z - planes[0].z * planes[1].y,
                    planes[0].z * planes[1].x - planes[0].x * planes[1].z,
                    planes[0].x * planes[1].y - planes[0].y * planes[1].x};
                const double creaseLength = std::sqrt(
                    crease.x * crease.x + crease.y * crease.y + crease.z * crease.z);
                if (creaseLength < 1e-9) {
                    if (freemanDebug()) fprintf(stderr, "[freeman] KILL: degenerate crease\n");
                    velocity_ = {0.0, 0.0, 0.0};
                    break;
                }
                crease.x /= creaseLength;
                crease.y /= creaseLength;
                crease.z /= creaseLength;
                const double along = dot(crease, originalVelocity);
                velocity_ = {crease.x * along, crease.y * along, crease.z * along};
            }
            if (freemanDebug()) {
                fprintf(stderr,
                        "[freeman] bump %d frac=%.4f n=(%.3f %.3f %.3f) planes=%d ground=%d "
                        "%s%d hspd %.1f -> %.1f\n",
                        bump, trace.fraction, trace.normal.x, trace.normal.y, trace.normal.z,
                        planeCount, onGround_ ? 1 : 0,
                        trace.fromDisplacement ? "disp" : "brush", trace.solidId,
                        debugSpeedBefore,
                        std::sqrt(velocity_.x * velocity_.x + velocity_.y * velocity_.y));
            }
            // If the clipped velocity now opposes the move's original
            // direction, kill it — stops oscillation in sloped corners.
            if (dot(velocity_, primalVelocity) <= 0.0) {
                if (freemanDebug()) fprintf(stderr, "[freeman] KILL: opposes primal\n");
                velocity_ = {0.0, 0.0, 0.0};
                break;
            }
        }
        feet_ = position;
    }

    // CGameMovement::StayOnGround: a grounded walker is glued to walkable
    // slopes and steps by snapping down up to a full step height after the
    // move — without this, walking downhill goes micro-airborne every tick
    // (no friction, bobbing feet). Only applies when the move started on the
    // ground and nothing (jump) launched us upward.
    if (groundedAtMoveStart && velocity_.z <= 140.0) {
        const HullTrace glue = traceHull(feet_, {0.0, 0.0, -StepHeight}, hullHeight);
        if (glue.hit && glue.fraction > 0.0 && glue.fraction < 1.0 && !glue.startSolid &&
            glue.normal.z >= 0.7) {
            feet_.z -= StepHeight * glue.fraction;
        }
    }

    // Ground categorization (CGameMovement::CategorizePosition): a short
    // downward hull trace; standing only on planes flatter than 0.7. The
    // airborne threshold is Source's NON_JUMP_VELOCITY (140) — it must sit
    // BELOW the 158.7 jump impulse, or the first jump ticks get re-grounded
    // and the jump feels delayed.
    if (velocity_.z <= 140.0) {
        const HullTrace ground = traceHull(feet_, {0.0, 0.0, -4.0}, hullHeight);
        if (ground.hit && ground.normal.z >= 0.7) {
            feet_.z -= 4.0 * ground.fraction;
            if (velocity_.z < 0.0) velocity_.z = 0.0;
            onGround_ = true;
        } else {
            onGround_ = false;
        }
    } else {
        onGround_ = false;
    }

    // CGameMovement::FinishGravity: the second half of the tick's gravity.
    if (!onGround_) velocity_.z -= gravity_ * dt * 0.5;

    checkTeleporters(hullHeight);

    view_->cameraState_.position = {feet_.x, feet_.y, feet_.z + eyeHeight_};

    // Footsteps (CBasePlayer::UpdateStepSound cadence: ~400 ms walking, 300
    // sprinting, 600 ducked), when HL2's footstep sounds are mounted.
    const double groundSpeed = std::sqrt(velocity_.x * velocity_.x + velocity_.y * velocity_.y);
    if (footstepsAvailable_ && onGround_ && groundSpeed > 50.0) {
        stepTimer_ -= dt;
        if (stepTimer_ <= 0.0) {
            stepTimer_ = ducked_ ? 0.6 : (groundSpeed > walkSpeed_ + 10.0 ? 0.3 : 0.4);
            stepIndex_ = (stepIndex_ + 1) % 4;
            // The surface underfoot picks the footstep set, concrete when the
            // mount lacks that set.
            std::string floorMaterial;
            castRay({feet_.x, feet_.y, feet_.z + 2.0}, {0.0, 0.0, -1.0}, 32.0, nullptr,
                    &floorMaterial);
            const std::string surface = surfaceFromMaterial(floorMaterial);
            const std::string variant = std::to_string(stepIndex_ + 1) + ".wav";
            const std::string step = firstSound(
                {"sound/player/footsteps/" + surface + variant,
                 "sound/player/footsteps/concrete" + variant});
            if (!step.empty()) playSound(step);
        }
    } else {
        stepTimer_ = 0.0;
    }

    // Viewmodel bob (CBaseHLCombatWeapon::CalcViewmodelBob): bobtime scales
    // with 2D speed and keeps cycling in the air; vertical bob runs at the
    // 0.45 s cycle, lateral at half that frequency (cycle * 2).
    {
        const double speed = std::clamp(groundSpeed, 0.0, BobMaxSpeed);
        bobTime_ += dt * (speed / BobMaxSpeed);
        const auto bobWave = [](double time, double period) {
            double cycle = time - static_cast<int>(time / period) * period;
            cycle /= period;
            return cycle < BobUp ? Pi * cycle / BobUp
                                 : Pi + Pi * (cycle - BobUp) / (1.0 - BobUp);
        };
        verticalBob_ = std::clamp(
            speed * 0.005 * (0.3 + 0.7 * std::sin(bobWave(bobTime_, BobCycle))), -7.0, 4.0);
        lateralBob_ = std::clamp(
            speed * 0.005 * (0.3 + 0.7 * std::sin(bobWave(bobTime_, BobCycle * 2.0))), -7.0,
            4.0);
    }

    if (swingRemaining_ > 0.0) swingRemaining_ = std::max(0.0, swingRemaining_ - dt);

    if (mode_ == Mode::Soldier) {
        if (fireCooldown_ > 0.0) fireCooldown_ = std::max(0.0, fireCooldown_ - dt);
        // TF2 auto-reloads while not firing, and only once the 0.8 s fire
        // interval has elapsed (firing cancels back to Idle). Three phases,
        // each driven over its sequence's real MDL duration: reload_start
        // loads nothing, each reload_loop loads one rocket, reload_finish
        // plays after the last. Without the phase sequences, Loop alone runs
        // on the 0.92 s first / 0.8 s next TF2 constants.
        const bool phased =
            !reloadStartSequence_.first.empty() && !reloadLoopSequence_.first.empty();
        const auto enterPhase = [&](ReloadPhase phase, double duration) {
            reloadPhase_ = phase;
            reloadCycleDuration_ = std::max(0.1, duration);
            reloadTimer_ = reloadCycleDuration_;
        };
        switch (reloadPhase_) {
        case ReloadPhase::Idle:
            if (rocketClip_ < RocketClipSize && fireCooldown_ <= 0.0) {
                if (phased) enterPhase(ReloadPhase::Start, reloadStartSequence_.second);
                else enterPhase(ReloadPhase::Loop, RocketReloadFirst);
            }
            break;
        case ReloadPhase::Start:
            reloadTimer_ -= dt;
            if (reloadTimer_ <= 0.0)
                enterPhase(ReloadPhase::Loop,
                           phased ? reloadLoopSequence_.second : RocketReloadNext);
            break;
        case ReloadPhase::Loop:
            reloadTimer_ -= dt;
            if (reloadTimer_ <= 0.0) {
                ++rocketClip_;
                const std::string reload = firstSound({"sound/weapons/rocket_reload.wav"});
                if (!reload.empty()) playSound(reload);
                if (rocketClip_ < RocketClipSize) {
                    enterPhase(ReloadPhase::Loop,
                               phased ? reloadLoopSequence_.second : RocketReloadNext);
                } else if (!reloadFinishSequence_.first.empty()) {
                    enterPhase(ReloadPhase::Finish, reloadFinishSequence_.second);
                } else {
                    reloadPhase_ = ReloadPhase::Idle;
                }
            }
            break;
        case ReloadPhase::Finish:
            reloadTimer_ -= dt;
            if (reloadTimer_ <= 0.0) reloadPhase_ = ReloadPhase::Idle;
            break;
        }
        updateRockets(dt);
    }

    updateCrowbar();
    view_->invalidateBaseFrame();
    view_->requestRepaint(true);
}

void FreemanWindow::updateCrowbar()
{
    if (crowbarIndex_ < 0 ||
        crowbarIndex_ >= static_cast<int>(scene_->entities.size())) {
        return;
    }
    hammer::vmf::EntityMarker& marker = scene_->entities[static_cast<std::size_t>(crowbarIndex_)];
    const auto& camera = view_->cameraState_;
    // Source viewmodels sit at the eye with the view angles, offset by the
    // bob (CBaseViewModel::AddViewModelBob: forward and up by 0.1 * vertical
    // bob, right by 0.8 * lateral bob).
    const hammer::vmf::Vec3 forward = hammer::camera::forwardVector(camera);
    const hammer::vmf::Vec3 right = hammer::camera::rightVector(camera);
    const hammer::vmf::Vec3 up{forward.y * right.z - forward.z * right.y,
                               forward.z * right.x - forward.x * right.z,
                               forward.x * right.y - forward.y * right.x};
    marker.origin = camera.position;
    // Seat the model back to match HL2's 54-FOV viewmodel pass (see the
    // constants above): at the world FOV the grip would otherwise loom too
    // close and large.
    const double pushBack =
        ViewmodelReferenceDistance *
        (std::tan(camera.verticalFovRadians * 0.5) / ViewmodelFovTanHalf - 1.0);
    marker.origin.x += forward.x * pushBack;
    marker.origin.y += forward.y * pushBack;
    marker.origin.z += forward.z * pushBack;
    if (mode_ == Mode::Soldier) {
        // Seat the launcher over the right shoulder and pull it slightly
        // toward the eye — eyeballed to match TF2's viewmodel framing.
        marker.origin.x += right.x * SoldierViewmodelRight - forward.x * SoldierViewmodelPull;
        marker.origin.y += right.y * SoldierViewmodelRight - forward.y * SoldierViewmodelPull;
        marker.origin.z += right.z * SoldierViewmodelRight - forward.z * SoldierViewmodelPull;
    }
    marker.origin.x += forward.x * verticalBob_ * 0.1 + right.x * lateralBob_ * 0.8;
    marker.origin.y += forward.y * verticalBob_ * 0.1 + right.y * lateralBob_ * 0.8;
    marker.origin.z += forward.z * verticalBob_ * 0.1 + verticalBob_ * 0.1;
    // Soldier mode: no viewmodel sway (lag) — the launcher stays glued to
    // the view; the bob above still applies.
    if (mode_ != Mode::Soldier) {
    // CalcViewModelLag: the facing chases the camera's forward at speed 5
    // (scaled up past 1.5 units of lag) and the origin is pushed 5 units
    // opposite the remaining difference. Source runs this every rendered
    // frame — so it runs here, on the sway's own clock, because updateCrowbar
    // fires per mouse move as well as per tick and a sway frozen between
    // ticks stutters against the freshly rotated camera.
    {
        const double lagDt =
            std::clamp((swayClock_.isValid() ? swayClock_.nsecsElapsed() : 0) / 1e9, 0.0, 0.05);
        swayClock_.restart();
        hammer::vmf::Vec3 difference{forward.x - lastFacing_.x, forward.y - lastFacing_.y,
                                     forward.z - lastFacing_.z};
        double lagSpeed = 5.0;
        const double diffLength =
            std::sqrt(difference.x * difference.x + difference.y * difference.y +
                      difference.z * difference.z);
        if (diffLength > MaxViewmodelLag) lagSpeed *= diffLength / MaxViewmodelLag;
        lastFacing_.x += difference.x * lagSpeed * lagDt;
        lastFacing_.y += difference.y * lagSpeed * lagDt;
        lastFacing_.z += difference.z * lagSpeed * lagDt;
        const double facingLength =
            std::sqrt(lastFacing_.x * lastFacing_.x + lastFacing_.y * lastFacing_.y +
                      lastFacing_.z * lastFacing_.z);
        if (facingLength > 1e-9) {
            lastFacing_.x /= facingLength;
            lastFacing_.y /= facingLength;
            lastFacing_.z /= facingLength;
        }
        marker.origin.x -= difference.x * 5.0;
        marker.origin.y -= difference.y * 5.0;
        marker.origin.z -= difference.z * 5.0;
    }
    }
    // The pitch-based push that keeps the model from exposing its unmodeled
    // backside when looking up or down.
    const double pitchDegrees = -camera.pitchRadians * 180.0 / Pi;
    if (mode_ != Mode::Soldier) {
        marker.origin.x -= pitchDegrees * (forward.x * 0.035 + right.x * 0.03 + up.x * 0.02);
        marker.origin.y -= pitchDegrees * (forward.y * 0.035 + right.y * 0.03 + up.y * 0.02);
        marker.origin.z -= pitchDegrees * (forward.z * 0.035 + right.z * 0.03 + up.z * 0.02);
    }
    // AddViewmodelBob's angle bob: roll +0.5 and pitch -0.4 of the vertical
    // bob, yaw -0.3 of the lateral bob.
    marker.angles = {pitchDegrees - verticalBob_ * 0.4,
                     camera.yawRadians * 180.0 / Pi - lateralBob_ * 0.3,
                     verticalBob_ * 0.5};
    // The actual v_crowbar sequences: attack1 driven through the swing window,
    // idle01 on the editor animation clock otherwise.
    marker.animateModel = true;
    if (swingRemaining_ > 0.0 && !swingSequence_.empty()) {
        marker.animationSequence = swingSequence_;
        marker.animationSequenceIndex = -1;
        marker.animationCycle = std::clamp(1.0 - swingRemaining_ / swingDuration_, 0.0, 1.0);
    } else if (mode_ == Mode::Soldier && reloadPhase_ != ReloadPhase::Idle &&
               !reloadLoopSequence_.first.empty()) {
        // The active phase's sequence, tracking the phase timer's progress —
        // the full animation plays for each phase.
        marker.animationSequence =
            reloadPhase_ == ReloadPhase::Start    ? reloadStartSequence_.first
            : reloadPhase_ == ReloadPhase::Finish ? reloadFinishSequence_.first
                                                  : reloadLoopSequence_.first;
        marker.animationSequenceIndex = -1;
        marker.animationCycle =
            std::clamp(1.0 - reloadTimer_ / reloadCycleDuration_, 0.0, 1.0);
    } else {
        marker.animationSequence = idleSequence_;
        marker.animationSequenceIndex = -1;
        marker.animationCycle = -1.0;  // free-running editor clock
    }
}

double FreemanWindow::jumpVelocity() const
{
    if (mode_ == Mode::Soldier) {
        // Impulse derived from the specified apex heights at TF2 gravity:
        // 72 units crouched, 50 uncrouched.
        return std::sqrt(2.0 * SoldierGravity * (ducked_ ? SoldierJumpDuck : SoldierJumpStand));
    }
    return JumpVelocity;
}

void FreemanWindow::fireRocket()
{
    if (crowbarIndex_ < 0 || rocketClip_ <= 0 || fireCooldown_ > 0.0) return;
    --rocketClip_;
    fireCooldown_ = RocketFireInterval;
    // Firing cancels any reload phase; the next reload starts from
    // reload_start once the fire interval has elapsed.
    reloadPhase_ = ReloadPhase::Idle;
    playSound(RocketFireSound);

    // CTFWeaponBaseGun::FireRocket: spawn offset (23.5, 12, -3) in view
    // space, fired along the eye direction at the rocket's speed.
    const auto& camera = view_->cameraState_;
    const hammer::vmf::Vec3 eye = camera.position;
    const hammer::vmf::Vec3 forward = hammer::camera::forwardVector(camera);
    const hammer::vmf::Vec3 right = hammer::camera::rightVector(camera);
    const hammer::vmf::Vec3 up{forward.y * right.z - forward.z * right.y,
                               forward.z * right.x - forward.x * right.z,
                               forward.x * right.y - forward.y * right.x};

    // Recycle: a free slot, else the oldest rocket in flight.
    Rocket* slot = nullptr;
    for (auto& rocket : rockets_) {
        if (!rocket.active) { slot = &rocket; break; }
    }
    if (!slot) {
        for (auto& rocket : rockets_) {
            if (!slot || rocket.life > slot->life) slot = &rocket;
        }
    }
    slot->active = true;
    slot->life = 0.0;
    slot->position = {eye.x + forward.x * 23.5 + right.x * 12.0 + up.x * -3.0,
                      eye.y + forward.y * 23.5 + right.y * 12.0 + up.y * -3.0,
                      eye.z + forward.z * 23.5 + right.z * 12.0 + up.z * -3.0};
    slot->velocity = {forward.x * RocketSpeed, forward.y * RocketSpeed,
                      forward.z * RocketSpeed};

    // Point-blank shots (fired at your feet or a wall in your face): the
    // spawn offset can already be at or past the surface, where the flight
    // ray would start behind the face and never hit. Sweep eye->spawn first
    // and detonate on contact — the canonical rocket jump.
    {
        hammer::vmf::Vec3 toSpawn{slot->position.x - eye.x, slot->position.y - eye.y,
                                  slot->position.z - eye.z};
        const double reach =
            std::sqrt(toSpawn.x * toSpawn.x + toSpawn.y * toSpawn.y + toSpawn.z * toSpawn.z);
        if (reach > 1e-9) {
            const hammer::vmf::Vec3 direction{toSpawn.x / reach, toSpawn.y / reach,
                                              toSpawn.z / reach};
            const auto hit = castRay(eye, direction, reach);
            if (hit) {
                const double at = std::max(0.0, *hit - 1.0);
                explodeRocket(*slot, {eye.x + direction.x * at, eye.y + direction.y * at,
                                      eye.z + direction.z * at});
            }
        }
    }

    // Fire animation on the viewmodel when the MDL has one.
    if (!swingSequences_.empty()) {
        swingSequence_ = swingSequences_.front().first;
        swingDuration_ = std::max(0.1, swingSequences_.front().second);
        swingRemaining_ = swingDuration_;
    }
}

void FreemanWindow::updateRockets(double dt)
{
    for (auto& rocket : rockets_) {
        if (!rocket.active) continue;
        rocket.life += dt;
        const hammer::vmf::Vec3 delta{rocket.velocity.x * dt, rocket.velocity.y * dt,
                                      rocket.velocity.z * dt};
        const double length =
            std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
        if (length > 1e-9) {
            const hammer::vmf::Vec3 direction{delta.x / length, delta.y / length,
                                              delta.z / length};
            // Rockets fly through clip/playerclip (castRay only sees world
            // geometry) — matching TF2, where those block players only.
            const auto hit = castRay(rocket.position, direction, length);
            if (hit) {
                // Detonate pulled 1 unit back off the surface.
                const double at = std::max(0.0, *hit - 1.0);
                const hammer::vmf::Vec3 source{rocket.position.x + direction.x * at,
                                               rocket.position.y + direction.y * at,
                                               rocket.position.z + direction.z * at};
                explodeRocket(rocket, source);
                continue;
            }
            rocket.position = {rocket.position.x + delta.x, rocket.position.y + delta.y,
                               rocket.position.z + delta.z};
        }
        if (rocket.life > RocketLifetime) {
            parkRocketMarker(rocket);
            continue;
        }
        if (rocket.markerIndex >= 0 &&
            rocket.markerIndex < static_cast<int>(scene_->entities.size())) {
            hammer::vmf::EntityMarker& marker =
                scene_->entities[static_cast<std::size_t>(rocket.markerIndex)];
            marker.origin = rocket.position;
            const double speed = std::sqrt(rocket.velocity.x * rocket.velocity.x +
                                           rocket.velocity.y * rocket.velocity.y +
                                           rocket.velocity.z * rocket.velocity.z);
            if (speed > 1e-6) {
                marker.angles = {-std::asin(rocket.velocity.z / speed) * 180.0 / Pi,
                                 std::atan2(rocket.velocity.y, rocket.velocity.x) * 180.0 / Pi,
                                 0.0};
            }
        }
    }
}

void FreemanWindow::explodeRocket(Rocket& rocket, const hammer::vmf::Vec3& source)
{
    if (!explodeSounds_.empty()) {
        playSound(explodeSounds_[static_cast<std::size_t>(
            QRandomGenerator::global()->bounded(static_cast<int>(explodeSounds_.size())))]);
    }

    // Self-knockback per CTFPlayer::ApplyPushFromDamage + CTFRadiusDamageInfo:
    // distance is the closer of hull center / feet to the blast, damage falls
    // off linearly to 50% at the radius edge (DMG_HALF_FALLOFF), and the push
    // runs from the blast point lowered 10 units toward the hull center.
    const double hullHeight = ducked_ ? DuckHullHeight : StandHullHeight;
    const hammer::vmf::Vec3 center{feet_.x, feet_.y, feet_.z + hullHeight * 0.5};
    const auto distanceTo = [&](const hammer::vmf::Vec3& point) {
        const double dx = source.x - point.x;
        const double dy = source.y - point.y;
        const double dz = source.z - point.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    // Falloff distance from the hull center only: measuring to the feet as
    // well made every floor shot register point-blank (distance ~0, zero
    // falloff), which overshot TF2's rocket-jump speeds badly.
    const double distance = distanceTo(center);
    if (std::min(distance, distanceTo(feet_)) <= RocketRadius) {
        const double damage =
            RocketDamage - (RocketDamage * 0.5) * (distance / RocketRadius);
        // DamageForce: damage * (48*48*82 / hull volume) * scale, cap 1000.
        // TF2 substitutes hull z = 55 while ducked — the crouch-jump bonus.
        const double hullZ = ducked_ ? RjHullZDuck : RjHullZStand;
        const double scale = onGround_ ? RjScaleGround : RjScaleAir;
        const double force = std::min(
            RjForceCap, damage * RjSelfDamageScale * (RjHullZStand / hullZ) * scale);
        hammer::vmf::Vec3 push{center.x - source.x, center.y - source.y,
                               center.z - (source.z - 10.0)};
        const double pushLength =
            std::sqrt(push.x * push.x + push.y * push.y + push.z * push.z);
        if (pushLength > 1e-6) {
            velocity_.x += push.x / pushLength * force;
            velocity_.y += push.y / pushLength * force;
            velocity_.z += push.z / pushLength * force;
            if (freemanDebug()) {
                fprintf(stderr,
                        "[freeman] rj dist=%.1f dmg=%.1f force=%.1f vel=(%.0f %.0f %.0f)\n",
                        distance, damage, force, velocity_.x, velocity_.y, velocity_.z);
            }
        }
        if (!painSounds_.empty()) {
            playSound(painSounds_[static_cast<std::size_t>(
                QRandomGenerator::global()->bounded(static_cast<int>(painSounds_.size())))]);
        }
    }
    parkRocketMarker(rocket);
}

void FreemanWindow::checkTeleporters(double hullHeight)
{
    if (teleports_.empty()) return;
    // Player hull AABB (32x32xhull, feet origin) against each trigger's
    // convex brushes, testing only the brush face planes (slightly generous
    // near corners — fine for a trigger volume, as in-engine triggers are).
    const hammer::vmf::Vec3 minimum{feet_.x - 16.0, feet_.y - 16.0, feet_.z};
    const hammer::vmf::Vec3 maximum{feet_.x + 16.0, feet_.y + 16.0, feet_.z + hullHeight};
    for (const TeleportVolume& teleport : teleports_) {
        bool touched = false;
        for (const auto& brush : teleport.brushes) {
            bool inside = true;
            for (const auto& face : brush.faces) {
                if (face.vertices.empty()) continue;
                const hammer::vmf::Vec3& normal = face.normal;
                const hammer::vmf::Vec3& point = brush.vertices[face.vertices.front()];
                // The AABB corner deepest along -normal: if even it sits
                // outside this face plane, the whole box does.
                const hammer::vmf::Vec3 support{normal.x > 0.0 ? minimum.x : maximum.x,
                                                normal.y > 0.0 ? minimum.y : maximum.y,
                                                normal.z > 0.0 ? minimum.z : maximum.z};
                const double distance = normal.x * (support.x - point.x) +
                                        normal.y * (support.y - point.y) +
                                        normal.z * (support.z - point.z);
                if (distance > 0.0) {
                    inside = false;
                    break;
                }
            }
            if (inside && !brush.faces.empty()) {
                touched = true;
                break;
            }
        }
        if (!touched) continue;
        // trigger_teleport: the toucher lands at the destination entity with
        // its velocity zeroed, facing the destination's angles.
        feet_ = teleport.destination;
        // Destinations authored flush with (or slightly inside) the floor
        // would start the hull solid and fall through — nudge up until free.
        for (int lift = 0; lift < 8; ++lift) {
            const HullTrace probe = traceHull(feet_, {0.0, 0.0, -1.0}, hullHeight);
            if (!probe.startSolid) break;
            feet_.z += 2.0;
        }
        velocity_ = {};
        onGround_ = false;
        if (teleport.hasAngles) {
            view_->cameraState_.yawRadians = teleport.destinationAngles.y * Pi / 180.0;
            // Same sign convention as the viewmodel: Source pitch is
            // negative-up, camera pitch positive-up.
            view_->cameraState_.pitchRadians = -teleport.destinationAngles.x * Pi / 180.0;
        }
        break;
    }
}

void FreemanWindow::parkRocketMarker(Rocket& rocket)
{
    rocket.active = false;
    if (rocket.markerIndex >= 0 &&
        rocket.markerIndex < static_cast<int>(scene_->entities.size())) {
        scene_->entities[static_cast<std::size_t>(rocket.markerIndex)].origin =
            {0.0, 0.0, -32768.0};
    }
}

bool FreemanWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != view_) return QDialog::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::KeyPress: {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            if (captured_) setCaptured(false);
            else close();
            return true;
        }
        if (key->key() == Qt::Key_F1) {
            spawn();
            // HealthKit.Touch — the medkit pickup chime marks the fresh start.
            playSound("sound/items/smallmedkit1.wav");
            return true;
        }
        if (key->key() == Qt::Key_F2) {
            airAccelHigh_ = !airAccelHigh_;
            // The two GameUI menu clicks: press for on, release for off.
            playSound(airAccelHigh_ ? "sound/ui/buttonclick.wav"
                                    : "sound/ui/buttonclickrelease.wav");
            return true;
        }
        keys_.insert(key->key());
        return true;
    }
    case QEvent::KeyRelease: {
        auto* key = static_cast<QKeyEvent*>(event);
        if (!key->isAutoRepeat()) keys_.remove(key->key());
        return true;
    }
    case QEvent::MouseButtonPress: {
        if (!captured_) {
            setCaptured(true);
            return true;
        }
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mode_ == Mode::Soldier) {
            if (mouse->button() == Qt::LeftButton) fireRocket();
            return true;
        }
        // Primary fire needs BOTH the crowbar model and the HL2 swing sound.
        if (mouse->button() == Qt::LeftButton && crowbarIndex_ >= 0 &&
            swingSoundAvailable_ && swingRemaining_ <= 0.0) {
            // CWeaponCrowbar::Swing: trace 75 units from the eye; a hit plays
            // the hit animation and impact sound, a whiff plays the miss
            // animation and the swing sound.
            const hammer::vmf::Vec3 eye = view_->cameraState_.position;
            const hammer::vmf::Vec3 aim = hammer::camera::forwardVector(view_->cameraState_);
            std::string struckMaterial;
            const bool struck = castRay(eye, aim, 75.0, nullptr, &struckMaterial).has_value();
            const auto& pool = struck && !hitSequences_.empty() ? hitSequences_ : swingSequences_;
            if (!pool.empty()) {
                const auto& pick = pool[static_cast<std::size_t>(
                    QRandomGenerator::global()->bounded(static_cast<int>(pool.size())))];
                swingSequence_ = pick.first;
                swingDuration_ = std::max(0.1, pick.second);
            } else {
                swingSequence_.clear();
                swingDuration_ = SwingDuration;
            }
            swingRemaining_ = swingDuration_;
            // The swing whoosh always plays; a hit layers the surface's own
            // impact sound (physics/<surface>/..._impact_hard*.wav) on top,
            // falling back to the crowbar impact and then to nothing.
            playSound(swingSoundPath_);
            if (struck) {
                const std::string surface = surfaceFromMaterial(struckMaterial);
                const int variant = 1 + QRandomGenerator::global()->bounded(3);
                std::vector<std::string> candidates{
                    "sound/physics/" + surface + "/" + surface + "_impact_hard" +
                        std::to_string(variant) + ".wav",
                    "sound/physics/" + surface + "/" + surface + "_impact_hard1.wav"};
                candidates.insert(candidates.end(), impactSounds_.begin(), impactSounds_.end());
                const std::string impact = firstSound(candidates);
                if (!impact.empty()) playSound(impact);
            }
        }
        return true;
    }
    case QEvent::MouseMove: {
        if (!captured_) return true;
        if (ignoreWarp_) {
            ignoreWarp_ = false;
            return true;
        }
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint center = view_->mapToGlobal(view_->rect().center());
        const QPointF delta = mouse->globalPosition() - QPointF(center);
        if (!delta.isNull()) {
            view_->applyCameraLookDelta(delta.x(), delta.y());
            // Re-glue the viewmodel immediately: the look delta invalidates
            // the frame, and rendering it with the crowbar still at the old
            // angles is exactly the "laggy viewmodel" effect.
            updateCrowbar();
            ignoreWarp_ = true;
            QCursor::setPos(center);
        }
        return true;
    }
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::Wheel:
        return true;
    case QEvent::FocusOut:
        keys_.clear();
        return false;
    default:
        return false;
    }
}

void FreemanWindow::setCaptured(bool captured)
{
    captured_ = captured;
    if (captured) {
        view_->setCursor(Qt::BlankCursor);
        ignoreWarp_ = true;
        QCursor::setPos(view_->mapToGlobal(view_->rect().center()));
    } else {
        view_->unsetCursor();
        keys_.clear();
    }
}

void FreemanWindow::closeEvent(QCloseEvent* event)
{
    if (timer_) timer_->stop();
    setCaptured(false);
    QDialog::closeEvent(event);
}
