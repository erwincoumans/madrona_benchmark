#include <madrona/mw_gpu_entry.hpp>

#include "sim.hpp"
#include "level_gen.hpp"

using namespace madrona;
using namespace madrona::math;
using namespace madrona::phys;

namespace RenderingSystem = madrona::render::RenderingSystem;

namespace GPUHideSeek {

constexpr inline float deltaT = 1.f / 30.f;
constexpr inline CountT numPhysicsSubsteps = 4;
constexpr inline CountT numPrepSteps = 96;
constexpr inline CountT episodeLen = 240;

constexpr inline auto physicsSolverSelector = PhysicsSystem::Solver::XPBD;

void Sim::registerTypes(ECSRegistry &registry,
                        const Config &cfg)
{
    base::registerTypes(registry);
    PhysicsSystem::registerTypes(registry, physicsSolverSelector);

    RenderingSystem::registerTypes(registry, cfg.renderBridge);

    registry.registerComponent<AgentPrepCounter>();
    registry.registerComponent<Action>();
    registry.registerComponent<OwnerTeam>();
    registry.registerComponent<AgentType>();
    registry.registerComponent<GrabData>();

    registry.registerComponent<SimEntity>();

    registry.registerComponent<AgentActiveMask>();
    registry.registerComponent<RelativeAgentObservations>();
    registry.registerComponent<RelativeBoxObservations>();
    registry.registerComponent<RelativeRampObservations>();
    registry.registerComponent<AgentVisibilityMasks>();
    registry.registerComponent<BoxVisibilityMasks>();
    registry.registerComponent<RampVisibilityMasks>();
    registry.registerComponent<Lidar>();
    registry.registerComponent<Seed>();
    registry.registerComponent<Reward>();
    registry.registerComponent<Done>();

    registry.registerSingleton<WorldReset>();
    registry.registerSingleton<GlobalDebugPositions>();
    registry.registerSingleton<LoadCheckpoint>();
    registry.registerSingleton<Checkpoint>();

    registry.registerArchetype<DynamicObject>();
    registry.registerArchetype<AgentInterface>();
    registry.registerArchetype<DynAgent>();

    registry.exportSingleton<WorldReset>(
        ExportID::Reset);
    registry.exportColumn<AgentInterface, AgentPrepCounter>(
        ExportID::PrepCounter);
    registry.exportColumn<AgentInterface, Action>(
        ExportID::Action);
    registry.exportColumn<AgentInterface, AgentType>(
        ExportID::AgentType);
    registry.exportColumn<AgentInterface, AgentActiveMask>(
        ExportID::AgentMask);
    registry.exportColumn<AgentInterface, RelativeAgentObservations>(
        ExportID::AgentObsData);
    registry.exportColumn<AgentInterface, RelativeBoxObservations>(
        ExportID::BoxObsData);
    registry.exportColumn<AgentInterface, RelativeRampObservations>(
        ExportID::RampObsData);
    registry.exportColumn<AgentInterface, AgentVisibilityMasks>(
        ExportID::AgentVisMasks);
    registry.exportColumn<AgentInterface, BoxVisibilityMasks>(
        ExportID::BoxVisMasks);
    registry.exportColumn<AgentInterface, RampVisibilityMasks>(
        ExportID::RampVisMasks);
    registry.exportColumn<AgentInterface, Lidar>(ExportID::Lidar);
    registry.exportColumn<AgentInterface, Seed>(ExportID::Seed);
    registry.exportColumn<AgentInterface, Reward>(ExportID::Reward);
    registry.exportColumn<AgentInterface, Done>(ExportID::Done);
    registry.exportSingleton<GlobalDebugPositions>(
        ExportID::GlobalDebugPositions);
    registry.exportColumn<render::RaycastOutputArchetype,
        render::RGBOutputBuffer>(
            (uint32_t)ExportID::Raycast);
}

static void initEpisodeRNG(Engine &ctx)
{
    RandKey new_rnd_counter;
    if ((ctx.data().simFlags & SimFlags::UseFixedWorld) ==
            SimFlags::UseFixedWorld) {
        new_rnd_counter = { 0, 0 };
    } else {
        if (ctx.singleton<LoadCheckpoint>().load == 1) {
            // If loading a checkpoint, use the random
            // seed that generated that world.
            new_rnd_counter = ctx.singleton<Checkpoint>().initRNDCounter;
        } else {
            new_rnd_counter = {
                .a = ctx.data().curWorldEpisode++,
                .b = (uint32_t)ctx.worldID().idx,
            };
        }
    }

    ctx.data().curEpisodeRNDCounter = new_rnd_counter;
    ctx.data().rng = RNG(rand::split_i(ctx.data().initRandKey,
        new_rnd_counter.a, new_rnd_counter.b));
}

static inline void resetEnvironment(Engine &ctx)
{
    ctx.data().curEpisodeStep = 0;

    phys::PhysicsSystem::reset(ctx);

    Entity *all_entities = ctx.data().obstacles;
    for (CountT i = 0; i < ctx.data().numObstacles; i++) {
        Entity e = all_entities[i];
        ctx.destroyRenderableEntity(e);
    }
    ctx.data().numObstacles = 0;
    ctx.data().numActiveBoxes = 0;
    ctx.data().numActiveRamps = 0;

    auto destroyAgent = [&](Entity e) {
        auto grab_data = ctx.getSafe<GrabData>(e);

        if (grab_data.valid()) {
            auto constraint_entity = grab_data.value().constraintEntity;
            if (constraint_entity != Entity::none()) {
                ctx.destroyEntity(constraint_entity);
            }
        }

        ctx.destroyRenderableEntity(e);
    };

    for (CountT i = 0; i < ctx.data().numHiders; i++) {
        destroyAgent(ctx.data().hiders[i]);
    }
    ctx.data().numHiders = 0;

    for (CountT i = 0; i < ctx.data().numSeekers; i++) {
        destroyAgent(ctx.data().seekers[i]);
    }
    ctx.data().numSeekers = 0;

    ctx.data().numActiveAgents = 0;

    initEpisodeRNG(ctx);
}

inline void resetSystem(Engine &ctx, WorldReset &reset)
{
    int32_t level = reset.resetLevel;

    if ((ctx.data().simFlags & SimFlags::IgnoreEpisodeLength) !=
                SimFlags::IgnoreEpisodeLength &&
            ctx.data().curEpisodeStep == episodeLen - 1) {
        level = 1;
    }

    if (level != 0) {
        resetEnvironment(ctx);

        reset.resetLevel = 0;

        int32_t num_hiders = ctx.data().rng.sampleI32(
            ctx.data().minHiders, ctx.data().maxHiders + 1);
        int32_t num_seekers = ctx.data().rng.sampleI32(
            ctx.data().minSeekers, ctx.data().maxSeekers + 1);

        generateEnvironment(ctx, level, num_hiders, num_seekers);
    } else {
        ctx.data().curEpisodeStep += 1;
    }

    ctx.data().hiderTeamReward.store_relaxed(1.f);
}

inline void movementSystem(Engine &ctx, Action &action, SimEntity sim_e,
                                 AgentType agent_type)
{
    if (sim_e.e == Entity::none()) return;
    if (agent_type == AgentType::Seeker &&
            ctx.data().curEpisodeStep < numPrepSteps - 1) {
        return;
    }

    constexpr CountT discrete_action_buckets = 11;
    constexpr CountT half_buckets = discrete_action_buckets / 2;
    constexpr float move_discrete_action_max = 60;
    constexpr float move_delta_per_bucket = move_discrete_action_max / half_buckets;

    constexpr float turn_discrete_action_max = 15;
    constexpr float turn_delta_per_bucket = turn_discrete_action_max / half_buckets;

    Quat cur_rot = ctx.get<Rotation>(sim_e.e);

    float f_x = move_delta_per_bucket * (action.x - 5);
    float f_y = move_delta_per_bucket * (action.y - 5);
    float t_z = turn_delta_per_bucket * (action.r - 5);

    ctx.get<ExternalForce>(sim_e.e) = cur_rot.rotateVec({ f_x, f_y, 0 });
    ctx.get<ExternalTorque>(sim_e.e) = Vector3 { 0, 0, t_z };
}

inline void actionSystem(Engine &ctx,
                         Action &action,
                         SimEntity sim_e,
                         AgentType agent_type)
{
    if (sim_e.e == Entity::none()) return;
    if (agent_type == AgentType::Seeker &&
            ctx.data().curEpisodeStep < numPrepSteps - 1) {
        return;
    }

    if (action.l == 1) {
        Vector3 cur_pos = ctx.get<Position>(sim_e.e);
        Quat cur_rot = ctx.get<Rotation>(sim_e.e);

        auto &bvh = ctx.singleton<broadphase::BVH>();
        float hit_t;
        Vector3 hit_normal;
        Entity lock_entity = bvh.traceRay(cur_pos + 0.5f * math::up,
            cur_rot.rotateVec(math::fwd), &hit_t, &hit_normal, 2.5f);

        if (lock_entity != Entity::none()) {
            auto &owner = ctx.get<OwnerTeam>(lock_entity);
            auto &response_type = ctx.get<ResponseType>(lock_entity);

            if (response_type == ResponseType::Static) {
                if ((agent_type == AgentType::Seeker &&
                        owner == OwnerTeam::Seeker) ||
                        (agent_type == AgentType::Hider &&
                         owner == OwnerTeam::Hider)) {
                    response_type = ResponseType::Dynamic;
                    owner = OwnerTeam::None;
                }
            } else {
                if (owner == OwnerTeam::None) {
                    response_type = ResponseType::Static;
                    owner = agent_type == AgentType::Hider ?
                        OwnerTeam::Hider : OwnerTeam::Seeker;
                }
            }
        }
    }

    if (action.g == 1) {
        Vector3 cur_pos = ctx.get<Position>(sim_e.e);
        Quat cur_rot = ctx.get<Rotation>(sim_e.e);

        auto &grab_data = ctx.get<GrabData>(sim_e.e);

        if (grab_data.constraintEntity != Entity::none()) {
            ctx.destroyEntity(grab_data.constraintEntity);
            grab_data.constraintEntity = Entity::none();
        } else {
            auto &bvh = ctx.singleton<broadphase::BVH>();
            float hit_t;
            Vector3 hit_normal;

            Vector3 ray_o = cur_pos + 0.5f * math::up;
            Vector3 ray_d = cur_rot.rotateVec(math::fwd);

            Entity grab_entity =
                bvh.traceRay(ray_o, ray_d, &hit_t, &hit_normal, 2.5f);

            if (grab_entity != Entity::none()) {
                auto &owner = ctx.get<OwnerTeam>(grab_entity);
                auto &response_type = ctx.get<ResponseType>(grab_entity);

                if (owner == OwnerTeam::None &&
                    response_type == ResponseType::Dynamic) {

                    Vector3 other_pos = ctx.get<Position>(grab_entity);
                    Quat other_rot = ctx.get<Rotation>(grab_entity);

                    Vector3 r1 = 1.25f * math::fwd + 0.5f * math::up;

                    Vector3 hit_pos = ray_o + ray_d * hit_t;
                    Vector3 r2 =
                        other_rot.inv().rotateVec(hit_pos - other_pos);

                    Quat attach1 = { 1, 0, 0, 0 };
                    Quat attach2 = (other_rot.inv() * cur_rot).normalize();

                    float separation = hit_t - 1.25f;

                    grab_data.constraintEntity = PhysicsSystem::makeFixedJoint(
                        ctx, sim_e.e, grab_entity, attach1, attach2,
                        r1, r2, separation);

                }
            }
        }
    }

    // "Consume" the actions. This isn't strictly necessary but
    // allows step to be called without every agent having acted
    action.x = 5;
    action.y = 5;
    action.r = 5;
    action.g = 0;
    action.l = 0;
}

inline void collectObservationsSystem(Engine &ctx,
                                      Entity agent_e,
                                      SimEntity sim_e,
                                      RelativeAgentObservations &agent_obs,
                                      RelativeBoxObservations &box_obs,
                                      RelativeRampObservations &ramp_obs,
                                      AgentPrepCounter &prep_counter)
{
    if (sim_e.e == Entity::none()) {
        return;
    }

    CountT cur_step = ctx.data().curEpisodeStep;
    if (cur_step <= numPrepSteps) {
        prep_counter.numPrepStepsLeft = numPrepSteps - cur_step;
    } 

    Vector3 agent_pos = ctx.get<Position>(sim_e.e);
    Quat agent_rot = ctx.get<Rotation>(sim_e.e);

    CountT num_boxes = ctx.data().numActiveBoxes;
    for (CountT box_idx = 0; box_idx < consts::maxBoxes; box_idx++) {
        auto &obs = box_obs.obs[box_idx];

        if (box_idx >= num_boxes) {
            obs= {};
            continue;
        }

        Entity box_e = ctx.data().boxes[box_idx];

        Vector3 box_pos = ctx.get<Position>(box_e);
        Vector3 box_vel = ctx.get<Velocity>(box_e).linear;
        Quat box_rot = ctx.get<Rotation>(box_e);

        Vector3 box_relative_pos =
            agent_rot.inv().rotateVec(box_pos - agent_pos);
        Vector3 box_relative_vel =
            agent_rot.inv().rotateVec(box_vel);

        obs.pos = { box_relative_pos.x, box_relative_pos.y };
        obs.vel = { box_relative_vel.x, box_relative_vel.y };
        obs.boxSize = ctx.data().boxSizes[box_idx];

        Quat relative_rot = agent_rot * box_rot.inv();
        obs.boxRotation = atan2f(
            2.f * (relative_rot.w * relative_rot.z +
                   relative_rot.x * relative_rot.y),
            1.f - 2.f * (relative_rot.y * relative_rot.y +
                         relative_rot.z * relative_rot.z));
    }

    CountT num_ramps = ctx.data().numActiveRamps;
    for (CountT ramp_idx = 0; ramp_idx < consts::maxRamps; ramp_idx++) {
        auto &obs = ramp_obs.obs[ramp_idx];

        if (ramp_idx >= num_ramps) {
            obs = {};
            continue;
        }

        Entity ramp_e = ctx.data().ramps[ramp_idx];

        Vector3 ramp_pos = ctx.get<Position>(ramp_e);
        Vector3 ramp_vel = ctx.get<Velocity>(ramp_e).linear;
        Quat ramp_rot = ctx.get<Rotation>(ramp_e);

        Vector3 ramp_relative_pos =
            agent_rot.inv().rotateVec(ramp_pos - agent_pos);
        Vector3 ramp_relative_vel =
            agent_rot.inv().rotateVec(ramp_vel);

        obs.pos = { ramp_relative_pos.x, ramp_relative_pos.y };
        obs.vel = { ramp_relative_vel.x, ramp_relative_vel.y };

        Quat relative_rot = agent_rot * ramp_rot.inv();
        obs.rampRotation = atan2f(
            2.f * (relative_rot.w * relative_rot.z +
                   relative_rot.x * relative_rot.y),
            1.f - 2.f * (relative_rot.y * relative_rot.y +
                         relative_rot.z * relative_rot.z));
    }

    CountT num_agents = ctx.data().numActiveAgents;
    CountT num_other_agents = 0;
    for (CountT agent_idx = 0; agent_idx < consts::maxAgents; agent_idx++) {
        if (agent_idx >= num_agents) {
            agent_obs.obs[num_other_agents++] = {};
            continue;
        }

        Entity other_agent_e = ctx.data().agentInterfaces[agent_idx];
        if (agent_e == other_agent_e) {
            continue;
        }

        Entity other_agent_sim_e = ctx.get<SimEntity>(other_agent_e).e;

        auto &obs = agent_obs.obs[num_other_agents++];

        Vector3 other_agent_pos =
            ctx.get<Position>(other_agent_sim_e);
        Vector3 other_agent_vel =
            ctx.get<Velocity>(other_agent_sim_e).linear;

        Vector3 other_agent_relative_pos =
            agent_rot.inv().rotateVec(other_agent_pos - agent_pos);
        Vector3 other_agent_relative_vel =
            agent_rot.inv().rotateVec(other_agent_vel);

        obs.pos = { other_agent_relative_pos.x, other_agent_relative_pos.y };
        obs.vel = { other_agent_relative_vel.x, other_agent_relative_vel.y };
    }
}

inline void computeVisibilitySystem(Engine &ctx,
                                    Entity agent_e,
                                    SimEntity sim_e,
                                    AgentType agent_type,
                                    AgentVisibilityMasks &agent_vis,
                                    BoxVisibilityMasks &box_vis,
                                    RampVisibilityMasks &ramp_vis)
{
    if (sim_e.e == Entity::none()) {
        return;
    }

    Vector3 agent_pos = ctx.get<Position>(sim_e.e);
    Quat agent_rot = ctx.get<Rotation>(sim_e.e);
    Vector3 agent_fwd = agent_rot.rotateVec(math::fwd);
    const float cos_angle_threshold = cosf(toRadians(135.f / 2.f));

    auto &bvh = ctx.singleton<broadphase::BVH>();

    auto checkVisibility = [&](Entity other_e) {
        Vector3 other_pos = ctx.get<Position>(other_e);

        Vector3 to_other = other_pos - agent_pos;

        Vector3 to_other_norm = to_other.normalize();

        float cos_angle = dot(to_other_norm, agent_fwd);

        if (cos_angle < cos_angle_threshold) {
            return 0.f;
        }

        float hit_t;
        Vector3 hit_normal;
        Entity hit_entity =
            bvh.traceRay(agent_pos, to_other, &hit_t, &hit_normal, 1.f);

        return hit_entity == other_e ? 1.f : 0.f;
    };

#ifdef MADRONA_GPU_MODE
    constexpr int32_t num_total_vis =
        consts::maxBoxes + consts::maxRamps + consts::maxAgents;
    const int32_t lane_id = threadIdx.x % 32;
    for (int32_t global_offset = 0; global_offset < num_total_vis;
         global_offset += 32) {
        int32_t cur_idx = global_offset + lane_id;

        Entity check_e = Entity::none();
        float *vis_out = nullptr;

        bool checking_agent = cur_idx < consts::maxAgents;
        uint32_t agent_mask = __ballot_sync(mwGPU::allActive, checking_agent);
        if (checking_agent) {
            bool valid_check = true;
            if (cur_idx < ctx.data().numActiveAgents) {
                Entity other_agent_e = ctx.data().agentInterfaces[cur_idx];
                valid_check = other_agent_e != agent_e;

                if (valid_check) {
                    check_e = ctx.get<SimEntity>(other_agent_e).e;
                }
            }

            uint32_t valid_mask = __ballot_sync(agent_mask, valid_check);
            valid_mask <<= (32 - lane_id);
            uint32_t num_lower_valid = __popc(valid_mask);

            if (valid_check) {
                vis_out = &agent_vis.visible[num_lower_valid];
            }
        } else if (int32_t box_idx = cur_idx - consts::maxAgents;
                   box_idx < consts::maxBoxes) {
            if (cur_idx < ctx.data().numActiveBoxes) {
                check_e = ctx.data().boxes[cur_idx];
            }
            vis_out = &box_vis.visible[cur_idx];
        } else if (int32_t ramp_idx =
                       cur_idx - consts::maxAgents - consts::maxBoxes;
                   ramp_idx < consts::maxRamps) {
            if (ramp_idx < ctx.data().numActiveRamps) {
                check_e = ctx.data().ramps[ramp_idx];
            }
            vis_out = &ramp_vis.visible[ramp_idx];
        } 

        if (check_e == Entity::none()) {
           if (vis_out != nullptr) {
               *vis_out = 0.f;
           }
        } else {
            bool is_visible = checkVisibility(check_e);
            *vis_out = is_visible ? 1.f : 0.f;
        }
    }
#else
    CountT num_boxes = ctx.data().numActiveBoxes;
    for (CountT box_idx = 0; box_idx < consts::maxBoxes; box_idx++) {
        if (box_idx < num_boxes) {
            Entity box_e = ctx.data().boxes[box_idx];
            box_vis.visible[box_idx] = checkVisibility(box_e);
        } else {
            box_vis.visible[box_idx] = 0.f;
        }
    }

    CountT num_ramps = ctx.data().numActiveRamps;
    for (CountT ramp_idx = 0; ramp_idx < consts::maxRamps; ramp_idx++) {
        if (ramp_idx < num_ramps) {
            Entity ramp_e = ctx.data().ramps[ramp_idx];
            ramp_vis.visible[ramp_idx] = checkVisibility(ramp_e);
        } else {
            ramp_vis.visible[ramp_idx] = 0.f;
        }
    }

    CountT num_agents = ctx.data().numActiveAgents;
    CountT num_other_agents = 0;
    for (CountT agent_idx = 0; agent_idx < consts::maxAgents; agent_idx++) {
        if (agent_idx >= num_agents) {
            agent_vis.visible[num_other_agents++] = 0.f;
            continue;
        }

        Entity other_agent_e = ctx.data().agentInterfaces[agent_idx];
        if (agent_e == other_agent_e) {
            continue;
        }

        Entity other_agent_sim_e = ctx.get<SimEntity>(other_agent_e).e;

        bool is_visible = checkVisibility(other_agent_sim_e);

        if (agent_type == AgentType::Seeker && is_visible) {
            AgentType other_type = ctx.get<AgentType>(other_agent_e);
            if (other_type == AgentType::Hider) {
                ctx.data().hiderTeamReward.store_relaxed(-1.f);
            }
        }

        agent_vis.visible[num_other_agents++] = is_visible;
    }
#endif
}

inline void lidarSystem(Engine &ctx,
                        SimEntity sim_e,
                        Lidar &lidar)
{
    if (sim_e.e == Entity::none()) {
        return;
    }

    Vector3 pos = ctx.get<Position>(sim_e.e);
    Quat rot = ctx.get<Rotation>(sim_e.e);
    auto &bvh = ctx.singleton<broadphase::BVH>();

    Vector3 agent_fwd = rot.rotateVec(math::fwd);
    Vector3 right = rot.rotateVec(math::right);

    auto traceRay = [&](int32_t idx) {
        float theta = 2.f * math::pi * (float(idx) / float(30)) +
            math::pi / 2.f;
        float x = cosf(theta);
        float y = sinf(theta);

        Vector3 ray_dir = (x * right + y * agent_fwd).normalize();

        float hit_t;
        Vector3 hit_normal;
        Entity hit_entity =
            bvh.traceRay(pos, ray_dir, &hit_t, &hit_normal, 200.f);

        if (hit_entity == Entity::none()) {
            lidar.depth[idx] = 0.f;
        } else {
            lidar.depth[idx] = hit_t;
        }
    };


#ifdef MADRONA_GPU_MODE
    int32_t idx = threadIdx.x % 32;

    if (idx < 30) {
        traceRay(idx);
    }
#else
    for (int32_t i = 0; i < 30; i++) {
        traceRay(i);
    }
#endif
}

// FIXME: refactor this so the observation systems can reuse these raycasts
// (unless a reset has occurred)
inline void rewardsVisSystem(Engine &ctx,
                             SimEntity sim_e,
                             AgentType agent_type)
{
#if 0
    const float cos_angle_threshold = cosf(toRadians(135.f / 2.f));

    if (sim_e.e == Entity::none() || agent_type != AgentType::Seeker) {
        return;
    }

    auto &bvh = ctx.singleton<broadphase::BVH>();

    Vector3 seeker_pos = ctx.get<Position>(sim_e.e);
    Quat seeker_rot = ctx.get<Rotation>(sim_e.e);
    Vector3 seeker_fwd = seeker_rot.rotateVec(math::fwd);

    for (CountT i = 0; i < ctx.data().numHiders; i++) {
        Entity hider_sim_e = ctx.data().hiders[i];

        Vector3 hider_pos = ctx.get<Position>(hider_sim_e);

        Vector3 to_hider = hider_pos - seeker_pos;

        Vector3 to_hider_norm = to_hider.normalize();

        float cos_angle = dot(to_hider_norm, seeker_fwd);

        if (cos_angle < cos_angle_threshold) {
            continue;
        }

        float hit_t;
        Vector3 hit_normal;
        Entity hit_entity =
            bvh.traceRay(seeker_pos, to_hider, &hit_t, &hit_normal, 1.f);

        if (hit_entity == hider_sim_e) {
            ctx.data().hiderTeamReward.store_relaxed(-1);
            break;
        }
    }
#endif
}

inline void outputRewardsDonesSystem(Engine &ctx,
                                     SimEntity sim_e,
                                     AgentType agent_type,
                                     Reward &reward,
                                     Done &done)
{
    if (sim_e.e == Entity::none()) {
        return;
    }

    CountT cur_step = ctx.data().curEpisodeStep;

    if (cur_step == 0) {
        done.v = 0;
    }

    if (cur_step < numPrepSteps - 1) {
        reward.v = 0.f;
        return;
    } else if (cur_step == episodeLen - 1) {
        done.v = 1;
    }

    float reward_val = ctx.data().hiderTeamReward.load_relaxed();
    if (agent_type == AgentType::Seeker) {
        reward_val *= -1.f;
    }

    Vector3 pos = ctx.get<Position>(sim_e.e);

    if (fabsf(pos.x) >= 18.f || fabsf(pos.y) >= 18.f) {
        reward_val -= 10.f;
    }

    reward.v = reward_val;
}

inline void globalPositionsDebugSystem(Engine &ctx,
                                       GlobalDebugPositions &global_positions)
{
    auto getXY = [](Vector3 v) {
        return Vector2 {
            v.x,
            v.y,
        };
    };

    for (CountT i = 0; i < consts::maxBoxes; i++) {
        if (i >= ctx.data().numActiveBoxes) {
            global_positions.boxPositions[i] = Vector2 {0, 0};
            continue;
        }

        global_positions.boxPositions[i] =
            getXY(ctx.get<Position>(ctx.data().boxes[i]));
    }

    for (CountT i = 0; i < consts::maxRamps; i++) {
        if (i >= ctx.data().numActiveRamps) {
            global_positions.rampPositions[i] = Vector2 {0, 0};
            continue;
        }

        global_positions.rampPositions[i] =
            getXY(ctx.get<Position>(ctx.data().ramps[i]));
    }

    {
        CountT out_offset = 0;
        for (CountT i = 0; i < ctx.data().numHiders; i++) {
            global_positions.agentPositions[out_offset++] = 
                getXY(ctx.get<Position>(ctx.data().hiders[i]));
        }

        for (CountT i = 0; i < ctx.data().numSeekers; i++) {
            global_positions.agentPositions[out_offset++] = 
                getXY(ctx.get<Position>(ctx.data().seekers[i]));
        }

        for (; out_offset < consts::maxAgents; out_offset++) {
            global_positions.agentPositions[out_offset++] = Vector2 {0, 0};
        }
    }
}

inline void updateCameraSystem(Engine &ctx,
                               Position &pos,
                               Rotation &rot,
                               SimEntity sim_e)
{
    if (sim_e.e == Entity::none()) {
        return;
    }

    pos = ctx.get<Position>(sim_e.e);
    rot = ctx.get<Rotation>(sim_e.e);
}

#ifdef MADRONA_GPU_MODE
template <typename ArchetypeT>
TaskGraph::NodeID queueSortByWorld(TaskGraphBuilder &builder,
                                   Span<const TaskGraph::NodeID> deps)
{
    auto sort_sys =
        builder.addToGraph<SortArchetypeNode<ArchetypeT, WorldID>>(
            deps);
    auto post_sort_reset_tmp =
        builder.addToGraph<ResetTmpAllocNode>({sort_sys});

    return post_sort_reset_tmp;
}
#endif

static TaskGraphNodeID processActionsAndPhysicsTasks(TaskGraphBuilder &builder)
{
    auto move_sys = builder.addToGraph<ParallelForNode<Engine, movementSystem,
        Action, SimEntity, AgentType>>({});

    auto broadphase_setup_sys = phys::PhysicsSystem::setupBroadphaseTasks(builder,
        {move_sys});

    auto action_sys = builder.addToGraph<ParallelForNode<Engine, actionSystem,
        Action, SimEntity, AgentType>>({broadphase_setup_sys});

    auto substep_sys = PhysicsSystem::setupPhysicsStepTasks(builder,
        {action_sys}, numPhysicsSubsteps, physicsSolverSelector);

    auto sim_done = substep_sys;

    sim_done = phys::PhysicsSystem::setupCleanupTasks(
        builder, {sim_done});

    return sim_done;
}

static TaskGraphNodeID rewardsAndDonesTasks(TaskGraphBuilder &builder,
                                            Span<const TaskGraphNodeID> deps)
{
    auto rewards_vis = builder.addToGraph<ParallelForNode<Engine,
        rewardsVisSystem,
            SimEntity,
            AgentType
        >>(deps);

    auto output_rewards_dones = builder.addToGraph<ParallelForNode<Engine,
        outputRewardsDonesSystem,
            SimEntity,
            AgentType,
            Reward,
            Done
        >>({rewards_vis});

    return output_rewards_dones;
}

static TaskGraphNodeID resetTasks(TaskGraphBuilder &builder,
                                  Span<const TaskGraphNodeID> deps)
{
    auto reset_sys = builder.addToGraph<ParallelForNode<Engine,
        resetSystem, WorldReset>>(deps);

    auto clear_tmp = builder.addToGraph<ResetTmpAllocNode>({reset_sys});

#ifdef MADRONA_GPU_MODE
    auto sort_dyn_agent = queueSortByWorld<DynAgent>(builder, {clear_tmp});
    auto sort_objects = queueSortByWorld<DynamicObject>(builder, {sort_dyn_agent});
    auto reset_finish = sort_objects;
#else
    auto reset_finish = clear_tmp;
#endif

#ifdef MADRONA_GPU_MODE
    auto recycle_sys = builder.addToGraph<RecycleEntitiesNode>({reset_finish});
    (void)recycle_sys;
#endif

    auto post_reset_broadphase = phys::PhysicsSystem::setupBroadphaseTasks(
        builder, {reset_finish});

    return post_reset_broadphase;
}

static void observationsTasks(const Config &cfg,
                              TaskGraphBuilder &builder,
                              Span<const TaskGraphNodeID> deps)
{
    auto collect_observations = builder.addToGraph<ParallelForNode<Engine,
        collectObservationsSystem,
            Entity,
            SimEntity,
            RelativeAgentObservations,
            RelativeBoxObservations,
            RelativeRampObservations,
            AgentPrepCounter
        >>(deps);

#ifdef MADRONA_GPU_MODE
    auto compute_visibility = builder.addToGraph<CustomParallelForNode<Engine,
        computeVisibilitySystem, 32, 1,
#else
    auto compute_visibility = builder.addToGraph<ParallelForNode<Engine,
        computeVisibilitySystem,
#endif
            Entity,
            SimEntity,
            AgentType,
            AgentVisibilityMasks,
            BoxVisibilityMasks,
            RampVisibilityMasks
        >>(deps);

#ifdef MADRONA_GPU_MODE
    auto lidar = builder.addToGraph<CustomParallelForNode<Engine,
        lidarSystem, 32, 1,
#else
    auto lidar = builder.addToGraph<ParallelForNode<Engine,
        lidarSystem,
#endif
            SimEntity,
            Lidar
        >>(deps);

    auto global_positions_debug = builder.addToGraph<ParallelForNode<Engine,
        globalPositionsDebugSystem,
            GlobalDebugPositions
        >>(deps);

    /* if (cfg.renderBridge) */ {
        auto update_camera = builder.addToGraph<ParallelForNode<Engine,
            updateCameraSystem,
                Position,
                Rotation,
                SimEntity
            >>(deps);

        // RenderingSystem::setupTasks(builder, {update_camera});
    }

    (void)lidar;
    (void)compute_visibility;
    (void)collect_observations;
    (void)global_positions_debug;
}

static void setupInitTasks(TaskGraphBuilder &builder, const Config &cfg)
{
#ifdef MADRONA_GPU_MODE
    // Agent interfaces only need to be sorted during init
    auto sort_agent_iface =
        queueSortByWorld<AgentInterface>(builder, {});
#endif

    auto resets = resetTasks(builder, {
#ifdef MADRONA_GPU_MODE
        sort_agent_iface
#endif
    });
    observationsTasks(cfg, builder, {resets});
}

static void setupStepTasks(TaskGraphBuilder &builder, const Config &cfg)
{
    auto sim_done = processActionsAndPhysicsTasks(builder);
    auto rewards_and_dones = rewardsAndDonesTasks(builder, {sim_done});
    auto resets = resetTasks(builder, {rewards_and_dones});
    observationsTasks(cfg, builder, {resets});
}

static void setupRenderTasks(TaskGraphBuilder &builder, 
                             const Config &cfg)
{
    RenderingSystem::setupTasks(builder, {});
}

void Sim::setupTasks(TaskGraphManager &taskgraph_mgr, const Config &cfg)
{
    setupInitTasks(taskgraph_mgr.init(TaskGraphID::Init), cfg);
    setupStepTasks(taskgraph_mgr.init(TaskGraphID::Step), cfg);
    setupRenderTasks(taskgraph_mgr.init(TaskGraphID::Render), cfg);
}

Sim::Sim(Engine &ctx,
         const Config &cfg,
         const WorldInit &)
    : WorldBase(ctx)
{
    simFlags = cfg.simFlags;

    initRandKey = cfg.initRandKey;
    curWorldEpisode = 0;

    const CountT max_total_entities = consts::maxBoxes + consts::maxRamps +
        consts::maxAgents + 30;

    PhysicsSystem::init(ctx, cfg.rigidBodyObjMgr, deltaT,
         numPhysicsSubsteps, -9.8 * math::up, max_total_entities,
         physicsSolverSelector);

    // enableRender = cfg.renderBridge != nullptr;
    enableRender = true;

    if (enableRender) {
        RenderingSystem::init(ctx, cfg.renderBridge);
    }

    obstacles =
        (Entity *)rawAlloc(sizeof(Entity) * size_t(max_total_entities));
    numObstacles = 0;

    numHiders = 0;
    numSeekers = 0;
    numActiveAgents = 0;
    
    curEpisodeStep = 0;

    minHiders = cfg.minHiders;
    maxHiders = cfg.maxHiders;
    minSeekers = cfg.minSeekers;
    maxSeekers = cfg.maxSeekers;
    maxAgentsPerWorld = cfg.maxHiders + cfg.maxSeekers;

    assert(maxAgentsPerWorld <= consts::maxAgents && maxAgentsPerWorld > 0);

    ctx.singleton<WorldReset>() = {
        .resetLevel = 1,
    };
     
    ctx.singleton<LoadCheckpoint>() = {
        .load = 0,
    };

    for (CountT i = 0; i < (CountT)maxAgentsPerWorld; i++) {
        Entity agent_iface = agentInterfaces[i] =
            ctx.makeEntity<AgentInterface>();

        if (enableRender) {
            render::RenderingSystem::attachEntityToView(ctx,
                    agent_iface,
                    100.f, 0.001f,
                    0.5f * math::up);
        }
    }

    ctx.data().hiderTeamReward.store_relaxed(1.f);
}

MADRONA_BUILD_MWGPU_ENTRY(Engine, Sim, Config, WorldInit);

}
