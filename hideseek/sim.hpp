#pragma once

#include <madrona/taskgraph.hpp>
#include <madrona/math.hpp>
#include <madrona/custom_context.hpp>
#include <madrona/components.hpp>
#include <madrona/physics.hpp>
#include <madrona/render/ecs.hpp>
#include <madrona/rand.hpp>

#include "sim_flags.hpp"

namespace GPUHideSeek {

using madrona::Entity;
using madrona::CountT;
using madrona::base::Position;
using madrona::base::Rotation;
using madrona::base::Scale;
using madrona::base::ObjectID;
using madrona::phys::Velocity;
using madrona::phys::ResponseType;
using madrona::phys::ExternalForce;
using madrona::phys::ExternalTorque;
using madrona::math::Vector2;
using madrona::math::Vector3;
using madrona::math::Quat;
using madrona::math::Diag3x3;
using madrona::RNG;
using madrona::RandKey;
using madrona::render::Renderable;
using madrona::phys::RigidBody;

namespace PhysicsSystem = madrona::phys::PhysicsSystem;

namespace consts {

static inline constexpr int32_t maxBoxes = 9;
static inline constexpr int32_t maxRamps = 2;
static inline constexpr int32_t maxAgents = 16;

}

enum class ExportID : uint32_t { // Base requirements
    Reset,
    PrepCounter,
    Action,
    AgentType,
    AgentMask,
    AgentObsData,
    BoxObsData,
    RampObsData,
    AgentVisMasks,
    BoxVisMasks,
    RampVisMasks,
    Lidar,
    Seed,
    Reward,
    Done,
    GlobalDebugPositions,
    Raycast,
    NumExports,
};

enum class TaskGraphID : uint32_t {
    Init,
    Step,
    Render,
    NumTaskGraphs,
};

enum class SimObject : uint32_t {
    Sphere,
    Plane,
    Cube,
    Wall,
    Agent,
    Ramp,
    Box,
    NumObjects,
};

struct Config {
    SimFlags simFlags;
    RandKey initRandKey;
    int32_t minHiders;
    int32_t maxHiders;
    int32_t minSeekers;
    int32_t maxSeekers;
    madrona::phys::ObjectManager *rigidBodyObjMgr;
    const madrona::render::RenderECSBridge *renderBridge;
};

class Engine;

struct WorldReset {
    int32_t resetLevel;
};

struct AgentPrepCounter {
    int32_t numPrepStepsLeft;
};

enum class OwnerTeam : uint32_t {
    None,
    Seeker,
    Hider,
    Unownable,
};

struct GrabData {
    Entity constraintEntity;
};

enum class AgentType : uint32_t {
    Seeker = 0,
    Hider = 1,
};

struct DynamicObject : public madrona::Archetype<
    RigidBody,
    Renderable,
    OwnerTeam
> {};

struct Action {
    int32_t x;
    int32_t y;
    int32_t r;
    int32_t g;
    int32_t l;
};

struct SimEntity {
    Entity e;
};

struct AgentActiveMask {
    float mask;
};

struct GlobalDebugPositions {
    Vector2 boxPositions[consts::maxBoxes];
    Vector2 rampPositions[consts::maxRamps];
    Vector2 agentPositions[consts::maxAgents];
};

struct AgentObservation {
    Vector2 pos;
    Vector2 vel;
};

struct BoxObservation {
    Vector2 pos;
    Vector2 vel;
    Vector2 boxSize;
    float boxRotation;
};

struct RampObservation {
    Vector2 pos;
    Vector2 vel;
    float rampRotation;
};

struct RelativeAgentObservations {
    AgentObservation obs[consts::maxAgents - 1];
};

struct RelativeBoxObservations {
    BoxObservation obs[consts::maxBoxes];
};

struct RelativeRampObservations {
    RampObservation obs[consts::maxRamps];
};

struct AgentVisibilityMasks {
    float visible[consts::maxAgents - 1];
};

struct BoxVisibilityMasks {
    float visible[consts::maxBoxes];
};

struct RampVisibilityMasks {
    float visible[consts::maxRamps];
};

struct Lidar {
    float depth[30];
};

struct Seed {
    RandKey key;
};

struct Reward {
    float v;
};

struct Done {
    int32_t v;
};

static_assert(sizeof(Action) == 5 * sizeof(int32_t));

struct AgentInterface : public madrona::Archetype<
    Position,
    Rotation,
    SimEntity,
    AgentPrepCounter,
    Action,
    AgentType,
    AgentActiveMask,
    RelativeAgentObservations,
    RelativeBoxObservations,
    RelativeRampObservations,
    AgentVisibilityMasks,
    BoxVisibilityMasks,
    RampVisibilityMasks,
    Lidar,
    Seed,
    Reward,
    Done,
    madrona::render::RenderCamera
> {};

struct DynAgent : public madrona::Archetype<
    RigidBody,
    Renderable,
    OwnerTeam,
    GrabData
> {};

struct WorldInit {};

struct LoadCheckpoint {
    int32_t load;
};

struct Checkpoint {
    RandKey initRNDCounter;
};

struct Sim : public madrona::WorldBase {
    static void registerTypes(madrona::ECSRegistry &registry,
                              const Config &cfg);

    static void setupTasks(madrona::TaskGraphManager &taskgraph_mgr,
                           const Config &cfg);

    Sim(Engine &ctx,
        const Config &cfg,
        const WorldInit &init);

    SimFlags simFlags;

    RandKey initRandKey;
    // Current episode within this world
    uint32_t curWorldEpisode;
    // The random seed that generated this world.
    RandKey curEpisodeRNDCounter;
    // Random number generator state
    RNG rng;

    Entity agentInterfaces[consts::maxAgents];

    Entity hiders[3];
    int32_t numHiders;
    Entity seekers[3];
    int32_t numSeekers;
    CountT numActiveAgents;

    Entity *obstacles;
    int32_t numObstacles;
    Entity boxes[consts::maxBoxes];
    Vector2 boxSizes[consts::maxBoxes];
    float boxRotations[consts::maxBoxes];
    Entity ramps[consts::maxRamps];
    float rampRotations[consts::maxRamps];
    CountT numActiveBoxes;
    CountT numActiveRamps;

    CountT curEpisodeStep;

    bool enableRender;

    int32_t minHiders;
    int32_t maxHiders;
    int32_t minSeekers;
    int32_t maxSeekers;
    int32_t maxAgentsPerWorld;

    madrona::AtomicFloat hiderTeamReward {0};
};

class Engine : public ::madrona::CustomContext<Engine, Sim> {
    using CustomContext::CustomContext;

public:
    // These are convenience helpers for creating renderable
    // entities when rendering isn't necessarily enabled
    template <typename ArchetypeT>
    inline madrona::Entity makeRenderableEntity();
    inline void destroyRenderableEntity(Entity e);
};

}

#include "sim.inl"
