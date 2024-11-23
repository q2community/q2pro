/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
// nav.c -- Kex navigation node support

#include "server.h"
#include "server/nav.h"
#include "common/error.h"
#if USE_REF
#include "refresh/refresh.h"
// ugly but necessary to hook into nav system without
// exposing this into a mess of spaghetti
#include "../refresh/gl.h"

static cvar_t *nav_debug;
static cvar_t *nav_debug_range;
#endif

static struct {
    bool	loaded;
    char	filename[MAX_QPATH];

    int32_t     num_nodes;
    int32_t     num_links;
    int32_t     num_traversals;
    int32_t     num_edicts;
    float       heuristic;

    // for quick node lookups
    int32_t     node_link_bitmap_size;
    byte        *node_link_bitmap;
    
    nav_node_t      *nodes;
    nav_link_t      *links;
    nav_traversal_t *traversals;
    nav_edict_t     *edicts;

    int32_t     num_conditional_nodes;
    nav_node_t  **conditional_nodes;

    // built-in context
    nav_ctx_t   *ctx;

    // entity stuff; TODO efficiently
    const edict_t     *registered_edicts[MAX_EDICTS];
    size_t            num_registered_edicts;

    bool              setup_entities;
    int32_t           nav_frame;
} nav_data;

// invalid value used for most of the system
const int32_t INVALID_ID = -1;

// magic file header
const int32_t NAV_MAGIC = MakeLittleLong('N', 'A', 'V', '3');

// last nav version we support
// changes from 5: yellow and green teams were removed;
// all versions prior to 6 should strip bitflags 2 and 3
// from link flags.
// binary comaptible with v5.
const int32_t NAV_VERSION_6 = 6;

// changes from 4: soft limit change for max nodes.
// binary compatible with v4.
const int32_t NAV_VERSION_5 = 5;

// changes from 3: ladder move plane was added to traversal.
const int32_t NAV_VERSION_4 = 4;

/**
meta:
  id: nav_v6_v5_v4
  file-extension: nav
  endian: le
seq:
  - id: header
    type: nav_header
  - id: data_header
    type: nav_data_header
  - id: nodes
    type: nav_node
    repeat: expr
    repeat-expr: data_header.num_nodes
  - id: node_positions
    type: vec3
    repeat: expr
    repeat-expr: data_header.num_nodes
  - id: links
    type: nav_link
    repeat: expr
    repeat-expr: data_header.num_links
  - id: traversals
    type: nav_traversal
    repeat: expr
    repeat-expr: data_header.num_traversals
  - id: num_edicts
    type: s4
  - id: edicts
    type: nav_edict
    repeat: expr
    repeat-expr: num_edicts
types:
  nav_header:
    seq:
      - id: magic
        contents: "NAV3"
      - id: version
        type: u4
  nav_data_header:
    seq:
      - id: num_nodes
        type: s4
      - id: num_links
        type: s4
      - id: num_traversals
        type: s4
      - id: heuristic
        type: f4
  nav_node:
    seq:
      - id: flags
        type: u2
      - id: num_links
        type: s2
      - id: first_link
        type: s2
      - id: radius
        type: s2
  nav_link:
    seq:
      - id: target
        type: s2
      - id: type
        type: u1
      - id: flags
        type: u1
      - id: traversal
        type: s2
  nav_traversal:
    seq:
      - id: funnel
        type: vec3
      - id: start
        type: vec3
      - id: end
        type: vec3
      - id: ladder_plane
        type: vec3
  nav_edict:
    seq:
      - id: link
        type: s2
      - id: model
        type: s4
      - id: mins
        type: vec3
      - id: maxs
        type: vec3
  vec3:
    seq:
      - id: x
        type: f4
      - id: y
        type: f4
      - id: z
        type: f4
*/

// changes from 2: link team flags become general link flags;
// all prior versions should use NavLinkFlag_AllTeams for linkFlags
// binary compatible with v2
const int32_t NAV_VERSION_3 = 3;

// changes from 1: edict now contains model index.
const int32_t NAV_VERSION_2 = 2;

/**
meta:
  id: nav_v3_v2
  file-extension: nav
  endian: le
seq:
  - id: header
    type: nav_header
  - id: data_header
    type: nav_data_header
  - id: nodes
    type: nav_node
    repeat: expr
    repeat-expr: data_header.num_nodes
  - id: node_positions
    type: vec3
    repeat: expr
    repeat-expr: data_header.num_nodes
  - id: links
    type: nav_link
    repeat: expr
    repeat-expr: data_header.num_links
  - id: traversals
    type: nav_traversal
    repeat: expr
    repeat-expr: data_header.num_traversals
  - id: num_edicts
    type: s4
  - id: edicts
    type: nav_edict
    repeat: expr
    repeat-expr: num_edicts
types:
  nav_header:
    seq:
      - id: magic
        contents: "NAV3"
      - id: version
        type: u4
  nav_data_header:
    seq:
      - id: num_nodes
        type: s4
      - id: num_links
        type: s4
      - id: num_traversals
        type: s4
      - id: heuristic
        type: f4
  nav_node:
    seq:
      - id: flags
        type: u2
      - id: num_links
        type: s2
      - id: first_link
        type: s2
      - id: radius
        type: s2
  nav_link:
    seq:
      - id: target
        type: s2
      - id: type
        type: u1
      - id: flags
        type: u1
      - id: traversal
        type: s2
  nav_traversal:
    seq:
      - id: funnel
        type: vec3
      - id: start
        type: vec3
      - id: end
        type: vec3
  nav_edict:
    seq:
      - id: link
        type: s2
      - id: model
        type: s4
      - id: mins
        type: vec3
      - id: maxs
        type: vec3
  vec3:
    seq:
      - id: x
        type: f4
      - id: y
        type: f4
      - id: z
        type: f4
*/

const int32_t NAV_VERSION_1 = 1;

/**
meta:
  id: nav_v1
  file-extension: nav
  endian: le
seq:
  - id: header
    type: nav_header
  - id: data_header
    type: nav_data_header
  - id: nodes
    type: nav_node
    repeat: expr
    repeat-expr: data_header.num_nodes
  - id: node_positions
    type: vec3
    repeat: expr
    repeat-expr: data_header.num_nodes
  - id: links
    type: nav_link
    repeat: expr
    repeat-expr: data_header.num_links
  - id: traversals
    type: nav_traversal
    repeat: expr
    repeat-expr: data_header.num_traversals
  - id: num_edicts
    type: s4
  - id: edicts
    type: nav_edict
    repeat: expr
    repeat-expr: num_edicts
types:
  nav_header:
    seq:
      - id: magic
        contents: "NAV3"
      - id: version
        type: u4
  nav_data_header:
    seq:
      - id: num_nodes
        type: s4
      - id: num_links
        type: s4
      - id: num_traversals
        type: s4
      - id: heuristic
        type: f4
  nav_node:
    seq:
      - id: flags
        type: u2
      - id: num_links
        type: s2
      - id: first_link
        type: s2
      - id: radius
        type: s2
  nav_link:
    seq:
      - id: target
        type: s2
      - id: type
        type: u1
      - id: flags
        type: u1
      - id: traversal
        type: s2
  nav_traversal:
    seq:
      - id: funnel
        type: vec3
      - id: start
        type: vec3
      - id: end
        type: vec3
  nav_edict:
    seq:
      - id: link
        type: s2
      - id: mins
        type: vec3
      - id: maxs
        type: vec3
  vec3:
    seq:
      - id: x
        type: f4
      - id: y
        type: f4
      - id: z
        type: f4
*/

#define NAV_VERSION_LATEST NAV_VERSION_6

#define NAV_VERIFY(condition, error) \
    if (!(condition)) { Com_SetLastError(error); goto fail; }

#define NAV_VERIFY_READ(v) \
    NAV_VERIFY(FS_Read(&v, sizeof(v), f) == sizeof(v), "bad data")

typedef struct nav_open_s {
    const nav_node_t  *node;
    float             f_score;
    list_t            entry;
} nav_open_t;

typedef struct nav_ctx_s {
    list_t          open_set_open, open_set_free;
    
    // TODO: min-heap or priority queue ordered by f_score?
    // currently using linked list which is a bit slow for insertion
    nav_open_t     *open_set;
    // TODO: figure out a way to get rid of "came_from"
    // and track start -> end off the bat
    int16_t         *came_from, *went_to;
    float           *g_score;
} nav_ctx_t;

#define NAV_ALLOC(n) \
    Z_TagMalloc(n, TAG_NAV)
#define NAV_ALLOCZ(n) \
    Z_TagMallocz(n, TAG_NAV)

nav_ctx_t *Nav_AllocCtx(void)
{
    size_t size = sizeof(nav_ctx_t) +
        (sizeof(nav_open_t) * nav_data.num_nodes) +
        (sizeof(int16_t) * nav_data.num_nodes) +
        (sizeof(int16_t) * nav_data.num_nodes) +
        (sizeof(float) * nav_data.num_nodes);
    nav_ctx_t *ctx = Z_TagMalloc(size, TAG_NAV);
    ctx->open_set = (nav_open_t *) (ctx + 1);
    ctx->came_from = (int16_t *) (ctx->open_set + nav_data.num_nodes);
    ctx->went_to = (int16_t *) (ctx->came_from + nav_data.num_nodes);
    ctx->g_score = (float *) (ctx->went_to + nav_data.num_nodes);

    return ctx;
}

void Nav_FreeCtx(nav_ctx_t *ctx)
{
    Z_Free(ctx);
}

// built-in path functions
static float Nav_Heuristic(const nav_path_t *path, const nav_node_t *node)
{
    return VectorDistance(path->goal->origin, node->origin);
}

static float Nav_Weight(const nav_path_t *path, const nav_node_t *node, const nav_link_t *link)
{
    if (link->type == NavLinkType_Teleport)
        return 1.0f;

    return VectorDistance(node->origin, link->target->origin) * nav_data.heuristic;
}

static bool Nav_NodeAccessible(const nav_path_t *path, const nav_node_t *node)
{
    if (node->flags & NodeFlag_Disabled) {
        return false;
    }

    if (path->request->nodeSearch.ignoreNodeFlags) {
        if (node->flags & NodeFlag_NoPOI) {
            return false;
        }
    } else {
        if (node->flags & (NodeFlag_NoMonsters | NodeFlag_Crouch | NodeFlag_Ladder | NodeFlag_Pusher | NodeFlag_Teleporter)) {
            return false;
        } else if ((node->flags & NodeFlag_UnderWater) && (path->request->pathFlags & (PathFlags_Walk | PathFlags_Water)) == PathFlags_Walk) {
            return false;
        } else if (!(node->flags & NodeFlag_UnderWater) && (path->request->pathFlags & (PathFlags_Walk | PathFlags_Water)) == PathFlags_Water) {
            return false;
        } else if (!(path->request->pathFlags & PathFlags_Elevator) && (node->flags & NodeFlag_Elevator)) {
            return false;
        }
    }

    return true;
}

static bool Nav_LinkAccessible(const nav_path_t *path, const nav_node_t *node, const nav_link_t *link)
{
    if (!path->request->nodeSearch.ignoreNodeFlags) {
        bool entity_traversal = false;

        // can only path to walk in water
        if (path->request->pathFlags == PathFlags_Water) {
            if (link->type != NavLinkType_Walk)
                return false;
        } else if (link->type == NavLinkType_Elevator) {
            // only use elevators if we asked for it
            if (!(path->request->pathFlags & PathFlags_Elevator))
                return false;

            entity_traversal = true;
        } else if (link->type == NavLinkType_WalkOffLedge) {
            // did we ask for it
            if (!(path->request->pathFlags & PathFlags_WalkOffLedge))
                return false;

            // check drop height
            if (path->request->traversals.dropHeight > 0.0f &&
                link->target->origin[2] < node->origin[2] - path->request->traversals.dropHeight)
                return false;
        } else if (link->type == NavLinkType_LongJump) {
            // did we ask for it
            if (!(path->request->pathFlags & PathFlags_LongJump))
                return false;
        } else if (link->type == NavLinkType_BarrierJump) {
            // did we ask for it
            if (!(path->request->pathFlags & NavLinkType_BarrierJump))
                return false;

            // check drop height
            if (path->request->traversals.jumpHeight > 0.0f &&
                link->target->origin[2] > node->origin[2] + path->request->traversals.jumpHeight)
                return false;
        }

        if (link->edict && !entity_traversal)
            return false;
    }

    return Nav_NodeAccessible(path, link->target);
}

#define Nav_ParamSupplied(x, def) \
    x > 0.0f ? x : def

static nav_node_t *Nav_ClosestNodeTo(const vec3_t p, const PathRequest *request)
{
    float w = INFINITY;
    nav_node_t *c = NULL;
    float minHeight = Nav_ParamSupplied(request->nodeSearch.minHeight, 64.0f);
    float maxHeight = Nav_ParamSupplied(request->nodeSearch.maxHeight, 64.0f);
    float radius = Nav_ParamSupplied(request->nodeSearch.maxHeight, 512.0f); 
    bool waterOnly = request->pathFlags == PathFlags_Water;

    float bz = p[2] - minHeight;
    float tz = p[2] + maxHeight;

    for (int i = 0; i < nav_data.num_nodes; i++) {
        nav_node_t *node = &nav_data.nodes[i];

        if (!request->nodeSearch.ignoreNodeFlags) {
            // these nodes should never be considered for
            // closest walkable nodes, they're transitional
            // or not for monsters
            if (node->flags & (NodeFlag_Disabled | NodeFlag_Pusher | NodeFlag_Teleporter | NodeFlag_Ladder | NodeFlag_Crouch | NodeFlag_NoMonsters))
                continue;

            // swimmies?
            if (waterOnly && !(node->flags & NodeFlag_UnderWater))
                continue;
        }

        // check Z distance
        if (node->origin[2] < bz || node->origin[2] > tz)
            continue;

        // check XY distance
        vec2_t d;
        Vector2Subtract(p, node->origin, d);

        float l = Vector2Length(d);

        if (l > radius)
            continue;

        if (l > w)
            continue;

        // check visibility
        vec3_t end = { 0.f, 0.f, 32.f };
        VectorAdd(end, node->origin, end);
        trace_t tr = SV_Trace(p, vec3_origin, vec3_origin, end, NULL, MASK_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP);

        if (tr.fraction < 1.0f)
            continue;

        w = l;
        c = node;
    }

    return c;
}

const float PATH_POINT_TOO_CLOSE = 64.f;

static const nav_link_t *Nav_GetLink(const nav_node_t *a, const nav_node_t *b)
{
    for (const nav_link_t *link = a->links; link != a->links + a->num_links; link++)
        if (link->target == b)
            return link;

    Q_assert(false);
    return NULL;
}

static bool Nav_TouchingNode(const vec3_t pos, float move_dist, const nav_node_t *node)
{
    return VectorDistance(pos, node->origin) <= move_dist;
}

static bool Nav_NodeReached(const vec3_t pos, const nav_node_t *node)
{
    vec3_t d;
    VectorSubtract(node->origin, pos, d);

    if (Vector2Length(d) > node->radius)
        return false;
    else if (fabsf(d[2]) > 64.f)
        return false;

    return true;
}

static inline void Nav_PushOpenSet(nav_ctx_t *ctx, const nav_node_t *node, float f)
{
    // grab free entry
    nav_open_t *o = LIST_FIRST(nav_open_t, &ctx->open_set_free, entry);
    List_Remove(&o->entry);

    o->node = node;
    o->f_score = f;

    nav_open_t *open_where = LIST_FIRST(nav_open_t, &ctx->open_set_open, entry);

    while (!LIST_TERM(open_where, &ctx->open_set_open, entry)) {

        if (f < open_where->f_score) {
            List_Insert(open_where->entry.prev, &o->entry);
            return;
        }

        open_where = LIST_NEXT(nav_open_t, open_where, entry);
    }

    List_Append(&ctx->open_set_open, &o->entry);
}

static inline void Nav_PushPathPoint(PathInfo *info, const PathRequest *request, const vec3_t p)
{
    if (info->numPathPoints < request->pathPoints.count)
        VectorCopy(p, request->pathPoints.posArray[info->numPathPoints]);
    info->numPathPoints++;
}

static inline void Nav_ReachedGoal(nav_path_t *path, PathInfo *info, const PathRequest *request, nav_ctx_t *ctx, int current)
{
    int64_t num_points = 0;

    // reverse the order of came_from into went_to
    // to make stuff below a bit easier to work with
    int16_t n = current;
    while (ctx->came_from[n] != -1) {
        num_points++;
        n = ctx->came_from[n];
    }

    n = current;
    int64_t p = 0;
    while (ctx->came_from[n] != -1) {
        n = ctx->went_to[num_points - p - 1] = ctx->came_from[n];
        p++;
    }

    // num_points now contains points between start
    // and current; it will be at least 1, since start can't
    // be the same as end, but may be less once we start clipping.
    Q_assert(num_points >= 1);
    Q_assert(ctx->went_to[0] != -1);

    int64_t first_point = 0;
    const nav_link_t *link = NULL;
            
    if (num_points > 1) {
        link = Nav_GetLink(&nav_data.nodes[ctx->went_to[0]], &nav_data.nodes[ctx->went_to[1]]);

        if (!path->request->nodeSearch.ignoreNodeFlags) {
            // if the node isn't a traversal, we may want
            // to skip the first node if we're either past it
            // or touching it
            if (link->type == NavLinkType_Walk || link->type == NavLinkType_Crouch) {
                if (Nav_NodeReached(request->start, &nav_data.nodes[ctx->went_to[0]])) {
                    first_point++;
                } else {
                    // check if we're in line for the node
                    vec3_t d = { 0.f, 0.f, 0.f };
                    Vector2Subtract(nav_data.nodes[ctx->went_to[1]].origin, nav_data.nodes[ctx->went_to[0]].origin, d);
                    Vector2Normalize(d);

                    vec3_t origin;
                    VectorMA(nav_data.nodes[ctx->went_to[0]].origin, nav_data.nodes[ctx->went_to[0]].radius, d, origin);

                    vec3_t path = { 0.f, 0.f, 0.f };
                    Vector2Subtract(nav_data.nodes[ctx->went_to[1]].origin, origin, path);

                    if (DotProduct(d, path) > 0.f)
                        first_point++;
                }
            }
        }
    }

    // store resulting path for compass, etc
    if (request->pathPoints.count) {
        // if we're too far from the first node, add in our current position.
        float dist = VectorDistance(request->start, nav_data.nodes[ctx->went_to[first_point]].origin);

        if (dist > PATH_POINT_TOO_CLOSE)
            Nav_PushPathPoint(info, request, request->start);

        // crawl forwards and add nodes
        for (p = first_point; p < num_points; p++)
            Nav_PushPathPoint(info, request, nav_data.nodes[ctx->went_to[p]].origin);

        // add the end point if we have room
        dist = VectorDistance(request->goal, nav_data.nodes[ctx->went_to[num_points - 1]].origin);

        if (dist > PATH_POINT_TOO_CLOSE)
            Nav_PushPathPoint(info, request, request->goal);
    }

    // we don't care about traversals
    if (path->request->nodeSearch.ignoreNodeFlags) {
        info->returnCode = PathReturnCode_RawPathFound;
        return;
    }

    // store move point info; check if we have
    // a traversal pending first
    if (link && link->traversal != NULL) {
        VectorCopy(link->traversal->start, info->firstMovePoint);
        VectorCopy(link->traversal->end, info->secondMovePoint);
        info->returnCode = PathReturnCode_TraversalPending;
        return;
    }

    VectorCopy(nav_data.nodes[ctx->went_to[first_point]].origin, info->firstMovePoint);
    if (first_point + 1 < num_points)
        VectorCopy(nav_data.nodes[ctx->went_to[first_point + 1]].origin, info->secondMovePoint);
    else
        VectorCopy(path->request->goal, info->secondMovePoint);
    info->returnCode = PathReturnCode_InProgress;
}

static PathInfo Nav_Path_(nav_path_t *path)
{
    PathInfo info = { 0 };

    if (!nav_data.loaded) {
        info.returnCode = PathReturnCode_NoNavAvailable;
        return info;
    }

    if ((path->request->pathFlags & (PathFlags_Walk | PathFlags_Water)) == 0) {
        info.returnCode = PathReturnCode_MissingWalkOrSwimFlag;
        return info;
    }

    const PathRequest *request = path->request;

    path->start = Nav_ClosestNodeTo(request->start, path->request);

    if (!path->start) {
        info.returnCode = PathReturnCode_NoStartNode;
        return info;
    }

    path->goal = Nav_ClosestNodeTo(request->goal, path->request);

    if (!path->goal) {
        info.returnCode = PathReturnCode_NoGoalNode;
        return info;
    }

    if (path->start == path->goal ||
        Nav_TouchingNode(request->start, request->moveDist, path->goal)) {
        info.returnCode = PathReturnCode_ReachedGoal;
        return info;
    }

    if (!path->request->nodeSearch.ignoreNodeFlags) {
        if (SV_PointContents(path->request->start) & MASK_SOLID) {
            info.returnCode = PathReturnCode_InvalidStart;
            return info;
        }
        if (SV_PointContents(path->request->goal) & MASK_SOLID) {
            info.returnCode = PathReturnCode_InvalidGoal;
            return info;
        }
    }

    int16_t start_id = path->start->id;
    int16_t goal_id = path->goal->id;
    
    nav_weight_func_t weight_func = path->weight ? path->weight : Nav_Weight;
    nav_heuristic_func_t heuristic_func = path->heuristic ? path->heuristic : Nav_Heuristic;
    nav_link_accessible_func_t link_accessible_func = path->link_accessible ? path->link_accessible : Nav_LinkAccessible;

    nav_ctx_t *ctx = path->context ? path->context : nav_data.ctx;

    for (int i = 0; i < nav_data.num_nodes; i++)
        ctx->g_score[i] = INFINITY;
    
    List_Init(&ctx->open_set_open);
    List_Init(&ctx->open_set_free);

    for (int i = 0; i < nav_data.num_nodes; i++)
        List_Append(&ctx->open_set_free, &ctx->open_set[i].entry);
    
    ctx->came_from[start_id] = -1;
    ctx->g_score[start_id] = 0;
    Nav_PushOpenSet(ctx, path->start, heuristic_func(path, path->start));

    info.returnCode = PathReturnCode_NoPathFound;

    while (true) {
        nav_open_t *cursor = LIST_FIRST(nav_open_t, &ctx->open_set_open, entry);

        // end of open set; can't reach the goal, or something
        // weird happened
        if (LIST_TERM(cursor, &ctx->open_set_open, entry))
            break;

        // shift off the head, insert into free
        List_Remove(&cursor->entry);
        List_Insert(&ctx->open_set_free, &cursor->entry);
        
        int16_t current = cursor->node->id;

        if (current == goal_id) {
            Nav_ReachedGoal(path, &info, request, ctx, current);
            break;
        }

        const nav_node_t *current_node = &nav_data.nodes[current];

        for (const nav_link_t *link = current_node->links; link != current_node->links + current_node->num_links; link++) {
            if (!link_accessible_func(path, current_node, link))
                continue;

            int16_t target_id = link->target->id;

            float temp_g_score = ctx->g_score[current] + weight_func(path, current_node, link);

            if (temp_g_score >= ctx->g_score[target_id])
                continue;

            ctx->came_from[target_id] = current;
            ctx->g_score[target_id] = temp_g_score;

            Nav_PushOpenSet(ctx, link->target, temp_g_score + heuristic_func(path, link->target));
        }
    }

    return info;
}

#if USE_REF
static void Nav_DebugPath(const PathInfo *path, const PathRequest *request)
{
    R_ClearDebugLines();

    int time = (request->debugging.drawTime * 1000) + 6000;
    
    color_t path_color = COLOR_SETA_U8(COLOR_RED, 64);
    color_t arrow_color = COLOR_SETA_U8(COLOR_YELLOW, 64);

    R_AddDebugSphere(request->start, 8.0f, path_color, time, false);
    R_AddDebugSphere(request->goal, 8.0f, path_color, time, false);

    if (request->pathPoints.count) {
        R_AddDebugArrow(request->start, request->pathPoints.posArray[0], 8.0f, arrow_color, arrow_color, time, false);

        for (int64_t i = 0; i < request->pathPoints.count - 1; i++)
            R_AddDebugArrow(request->pathPoints.posArray[i], request->pathPoints.posArray[i + 1], 8.0f, arrow_color, arrow_color, time, false);

        R_AddDebugArrow(request->pathPoints.posArray[request->pathPoints.count - 1], request->goal, 8.0f, arrow_color, arrow_color, time, false);
    } else {
        R_AddDebugArrow(request->start, request->goal, 8.0f, arrow_color, arrow_color, time, false);
    }

    R_AddDebugSphere(path->firstMovePoint, 16.0f, path_color, time, false);
    R_AddDebugArrow(path->firstMovePoint, path->secondMovePoint, 16.0f, path_color, path_color, time, false);
}
#endif

PathInfo Nav_Path(nav_path_t *path)
{
    PathInfo result = Nav_Path_(path);
    
#if USE_REF
    if (path->request->debugging.drawTime)
        Nav_DebugPath(&result, path->request);
#endif

    return result;
}

static bool Nav_NodeIsConditional(const nav_node_t *node)
{
    return node->flags & (NodeFlag_CheckDoorLinks | NodeFlag_CheckForHazard | NodeFlag_CheckHasFloor | NodeFlag_CheckInLiquid | NodeFlag_CheckInSolid);
}

void Nav_Load(const char *map_name)
{
    Q_assert(!nav_data.loaded);

    nav_data.loaded = true;

    Q_snprintf(nav_data.filename, sizeof(nav_data.filename), "bots/navigation/%s.nav", map_name);

    qhandle_t f;
    int64_t l = FS_OpenFile(nav_data.filename, &f, FS_MODE_READ);

    if (l < 0)
        return;

    int v;

    NAV_VERIFY_READ(v);
    NAV_VERIFY(v == NAV_MAGIC, "bad magic");

    NAV_VERIFY_READ(v);
    NAV_VERIFY((v <= NAV_VERSION_LATEST), va("bad version %i\n", v));

    NAV_VERIFY_READ(nav_data.num_nodes);
    NAV_VERIFY_READ(nav_data.num_links);
    NAV_VERIFY_READ(nav_data.num_traversals);
    NAV_VERIFY_READ(nav_data.heuristic);

    NAV_VERIFY(nav_data.nodes = NAV_ALLOCZ(sizeof(nav_node_t) * nav_data.num_nodes), "out of memory");
    if (nav_data.num_links)
        NAV_VERIFY(nav_data.links = NAV_ALLOCZ(sizeof(nav_link_t) * nav_data.num_links), "out of memory");
    if (nav_data.num_traversals)
        NAV_VERIFY(nav_data.traversals = NAV_ALLOCZ(sizeof(nav_traversal_t) * nav_data.num_traversals), "out of memory");

    nav_data.num_conditional_nodes = 0;

    for (int i = 0; i < nav_data.num_nodes; i++) {
        nav_node_t *node = nav_data.nodes + i;
        
        node->id = i;
        NAV_VERIFY_READ(node->flags);
        NAV_VERIFY_READ(node->num_links);
        int16_t first_link;
        NAV_VERIFY_READ(first_link);
        NAV_VERIFY(first_link >= 0 && first_link + node->num_links <= nav_data.num_links, "bad node link extents");
        node->links = &nav_data.links[first_link];
        NAV_VERIFY_READ(node->radius);

        if (Nav_NodeIsConditional(node))
            nav_data.num_conditional_nodes++;
    }

    if (nav_data.num_conditional_nodes)
        NAV_VERIFY(nav_data.conditional_nodes = NAV_ALLOCZ(sizeof(nav_node_t *) * nav_data.num_conditional_nodes), "out of memory");

    for (int i = 0, c = 0; i < nav_data.num_nodes; i++) {
        nav_node_t *node = nav_data.nodes + i;

        NAV_VERIFY_READ(node->origin);

        if (Nav_NodeIsConditional(node))
            nav_data.conditional_nodes[c++] = node;
    }

    for (int i = 0; i < nav_data.num_links; i++) {
        nav_link_t *link = nav_data.links + i;
        
        int16_t target;
        NAV_VERIFY_READ(target);
        NAV_VERIFY(target >= 0 && target < nav_data.num_nodes, "bad link target");
        link->target = &nav_data.nodes[target];
        NAV_VERIFY_READ(link->type);
        NAV_VERIFY_READ(link->flags);

        if (v < NAV_VERSION_3)
            link->flags = NavLinkFlag_AllTeams;
        // strip old green/yellow team flags
        else if (v < NAV_VERSION_6)
            link->flags &= ~(BIT(2) | BIT(3));

        int16_t traversal;
        NAV_VERIFY_READ(traversal);
        link->traversal = NULL;
        link->edict = NULL;

        if (traversal != -1) {
            NAV_VERIFY(traversal < nav_data.num_traversals, "bad link traversal");
            link->traversal = &nav_data.traversals[traversal];
        }
    }

    for (int i = 0; i < nav_data.num_traversals; i++) {
        nav_traversal_t *traversal = nav_data.traversals + i;
        
        NAV_VERIFY_READ(traversal->funnel);
        NAV_VERIFY_READ(traversal->start);
        NAV_VERIFY_READ(traversal->end);

        if (v >= NAV_VERSION_4)
            NAV_VERIFY_READ(traversal->ladder_plane);
    }
    
    NAV_VERIFY_READ(nav_data.num_edicts);

    if (nav_data.num_edicts) {
        NAV_VERIFY(nav_data.edicts = NAV_ALLOCZ(sizeof(nav_edict_t) * nav_data.num_edicts), "out of memory");

        for (int i = 0; i < nav_data.num_edicts; i++) {
            nav_edict_t *edict = nav_data.edicts + i;
        
            int16_t link;
            NAV_VERIFY_READ(link);
            NAV_VERIFY(link >= 0 && link < nav_data.num_links, "bad edict link");
            edict->link = &nav_data.links[link];
            edict->link->edict = edict;
            edict->game_edict = NULL;
            if (v >= NAV_VERSION_2)
                NAV_VERIFY_READ(edict->model);
            NAV_VERIFY_READ(edict->mins);
            NAV_VERIFY_READ(edict->maxs);
        }
    }

    nav_data.node_link_bitmap_size = (nav_data.num_nodes + CHAR_BIT - 1) / CHAR_BIT;
    NAV_VERIFY(nav_data.node_link_bitmap = NAV_ALLOCZ(nav_data.node_link_bitmap_size * nav_data.num_nodes), "out of memory");

    for (int i = 0; i < nav_data.num_nodes; i++) {
        nav_node_t *node = nav_data.nodes + i;
        byte *bits = nav_data.node_link_bitmap + (nav_data.node_link_bitmap_size * i);

        for (nav_link_t *link = node->links; link != node->links + node->num_links; link++) {
            Q_SetBit(bits, link->target->id);
        }
    }

    Com_DPrintf("Bot navigation file (%s) loaded:\n %i nodes\n %i links\n %i traversals\n %i edicts\n",
        nav_data.filename, nav_data.num_nodes, nav_data.num_links, nav_data.num_traversals, nav_data.num_edicts);

    nav_data.ctx = Nav_AllocCtx();

    return;

fail:
    Com_EPrintf("Couldn't load bot navigation file (%s): %s\n", nav_data.filename, Com_GetLastError());
    Nav_Unload();
}

void Nav_Unload(void)
{
    if (!nav_data.loaded)
        return;

    Z_FreeTags(TAG_NAV);

    memset(&nav_data, 0, sizeof(nav_data));
}

static void Nav_GetNodeBounds(const nav_node_t *node, vec3_t mins, vec3_t maxs)
{
    VectorSet(mins, -16, -16, -24);
    VectorSet(maxs, 16, 16, 32);

    if (node->flags & NodeFlag_Crouch) {
        maxs[2] = 4.0f;
    }
}

static void Nav_GetNodeTraceOrigin(const nav_node_t *node, vec3_t origin)
{
    VectorCopy(node->origin, origin);
    origin[2] += 24.0f;
}

const float NavFloorDistance = 96.0f;

#if USE_REF
static void Nav_Debug(void)
{
    if (!nav_debug->integer) {
        return;
    }

    for (int i = 0; i < nav_data.num_nodes; i++) {
        const nav_node_t *node = &nav_data.nodes[i];
        float len = VectorDistance(node->origin, glr.fd.vieworg);

        if (len > nav_debug_range->value) {
            continue;
        }

        uint8_t alpha = Q_clipf((1.0f - ((len - 32.f) / (nav_debug_range->value - 32.f))), 0.0f, 1.0f) * 255.f;

        R_AddDebugCircle(node->origin, node->radius, COLOR_SETA_U8(COLOR_CYAN, alpha), SV_FRAMETIME, true);

        vec3_t mins, maxs, origin;
        Nav_GetNodeBounds(node, mins, maxs);
        Nav_GetNodeTraceOrigin(node, origin);
        
        VectorAdd(mins, origin, mins);
        VectorAdd(maxs, origin, maxs);

        R_AddDebugBounds(mins, maxs, COLOR_SETA_U8((node->flags & NodeFlag_Disabled) ? COLOR_RED : COLOR_YELLOW, alpha), SV_FRAMETIME, true);

        if (node->flags & NodeFlag_CheckHasFloor) {
            vec3_t floormins, floormaxs;
            VectorCopy(mins, floormins);
            VectorCopy(maxs, floormaxs);

            float mins_z = floormins[2];
            floormins[2] = origin[2] - NavFloorDistance;
            floormaxs[2] = mins_z;

            R_AddDebugBounds(floormins, floormaxs, COLOR_SETA_U8(COLOR_RED, alpha * 0.5f), SV_FRAMETIME, true);
        }

        vec3_t s;
        VectorCopy(node->origin, s);
        s[2] += 24;

        R_AddDebugLine(node->origin, s, COLOR_SETA_U8(COLOR_CYAN, alpha), SV_FRAMETIME, true);

        vec3_t t;
        VectorCopy(node->origin, t);
        t[2] += 64;

        R_AddDebugText(t, NULL, va("%td", node - nav_data.nodes), 0.25f, COLOR_SETA_U8(COLOR_CYAN, alpha), SV_FRAMETIME, true);

        t[2] -= 18;

        static char node_text_buffer[128];
        *node_text_buffer = 0;
        
        if (node->flags & NodeFlag_Disabled)
            Q_strlcat(node_text_buffer, "DISABLED\n\n", sizeof(node_text_buffer));
        if (node->flags & NodeFlag_Teleporter)
            Q_strlcat(node_text_buffer, "TELEPORTER\n", sizeof(node_text_buffer));
        if (node->flags & NodeFlag_Pusher)
            Q_strlcat(node_text_buffer, "PUSHER\n", sizeof(node_text_buffer));
        if (node->flags & NodeFlag_Elevator)
            Q_strlcat(node_text_buffer, "ELEVATOR\n", sizeof(node_text_buffer));
        if (node->flags & NodeFlag_Ladder)
            Q_strlcat(node_text_buffer, "LADDER\n", sizeof(node_text_buffer));
        if (node->flags & NodeFlag_UnderWater)
            Q_strlcat(node_text_buffer, "UNDERWATER\n", sizeof(node_text_buffer));
        if (node->flags & NodeFlag_CheckForHazard)
            Q_strlcat(node_text_buffer, "CHECK HAZARD\n", sizeof(node_text_buffer));
        if (node->flags & NodeFlag_CheckHasFloor)
            Q_strlcat(node_text_buffer, "CHECK FLOOR\n", sizeof(node_text_buffer));
        if (node->flags & NodeFlag_CheckInSolid)
            Q_strlcat(node_text_buffer, "CHECK SOLID\n", sizeof(node_text_buffer));
        if (node->flags & NodeFlag_NoMonsters)
            Q_strlcat(node_text_buffer, "NO MOBS\n", sizeof(node_text_buffer));
        if (node->flags & NodeFlag_Crouch)
            Q_strlcat(node_text_buffer, "CROUCH\n", sizeof(node_text_buffer));
        if (node->flags & NodeFlag_NoPOI)
            Q_strlcat(node_text_buffer, "NO POI\n", sizeof(node_text_buffer));
        if (node->flags & NodeFlag_CheckInLiquid)
            Q_strlcat(node_text_buffer, "CHECK LIQUID\n", sizeof(node_text_buffer));
        if (node->flags & NodeFlag_CheckDoorLinks)
            Q_strlcat(node_text_buffer, "CHECK DOORS\n", sizeof(node_text_buffer));

        if (*node_text_buffer)
            R_AddDebugText(t, NULL, node_text_buffer, 0.1f, COLOR_SETA_U8(COLOR_GREEN, alpha), SV_FRAMETIME, true);
        
        for (const nav_link_t *link = node->links; link != node->links + node->num_links; link++) {
            vec3_t e;
            VectorCopy(link->target->origin, e);
            e[2] += 24;

            const byte *target_bits = nav_data.node_link_bitmap + (nav_data.node_link_bitmap_size * link->target->id);
            bool link_disabled = ((node->flags | link->target->flags) & NodeFlag_Disabled);
            uint8_t link_alpha = link_disabled ? (alpha * 0.5f) : alpha;
            
            color_t line_color = COLOR_SETA_U8(link_disabled ? COLOR_RED : COLOR_WHITE, link_alpha);
            color_t traversal_color = COLOR_SETA_U8(link_disabled ? COLOR_RED : COLOR_BLUE, link_alpha);
            color_t arrow_color = COLOR_SETA_U8(COLOR_RED, link_alpha);
            color_t one_way_line_color = COLOR_SETA_U8(link_disabled ? COLOR_RED : COLOR_CYAN, link_alpha);

            if (Q_IsBitSet(target_bits, i)) {
                // two-way link
                if (i < link->target->id) {
                    continue;
                }

                const nav_link_t *other_link = Nav_GetLink(link->target, node);

                Q_assert(other_link);
                
                // simple link
                if (!link->traversal && !other_link->traversal) {
                    R_AddDebugLine(s, e, line_color, SV_FRAMETIME, true);
                } else {
                    // one or both are traversals
                    // render a->b
                    if (!link->traversal) {
                        R_AddDebugArrow(s, e, 8.0f, line_color, arrow_color, SV_FRAMETIME, true);
                    } else {
                        vec3_t ctrl;

                        if (s[2] > e[2]) {
                            VectorCopy(e, ctrl);
                            ctrl[2] = s[2];
                        } else {
                            VectorCopy(s, ctrl);
                            ctrl[2] = e[2];
                        }

                        R_AddDebugCurveArrow(s, ctrl, e, 8.0f, traversal_color, arrow_color, SV_FRAMETIME, true);
                    }

                    // render b->a
                    if (!other_link->traversal) {
                        R_AddDebugArrow(e, s, 8.0f, line_color, arrow_color, SV_FRAMETIME, true);
                    } else {
                        vec3_t ctrl;

                        if (s[2] > e[2]) {
                            VectorCopy(e, ctrl);
                            ctrl[2] = s[2];
                        } else {
                            VectorCopy(s, ctrl);
                            ctrl[2] = e[2];
                        }

                        // raise the other side's points slightly
                        s[2] += 32.f;
                        ctrl[2] += 32.f;
                        e[2] += 32.f;

                        R_AddDebugCurveArrow(e, ctrl, s, 8.0f, traversal_color, arrow_color, SV_FRAMETIME, true);

                        s[2] -= 32.f;
                        e[2] -= 32.f;
                    }
                }
            } else {
                // one-way link
                if (link->traversal) {
                    vec3_t ctrl;

                    if (s[2] > e[2]) {
                        VectorCopy(e, ctrl);
                        ctrl[2] = s[2];
                    } else {
                        VectorCopy(s, ctrl);
                        ctrl[2] = e[2];
                    }

                    R_AddDebugCurveArrow(s, ctrl, e, 8.0f, traversal_color, arrow_color, SV_FRAMETIME, true);
                } else {
                    R_AddDebugArrow(s, e, 8.0f, one_way_line_color, arrow_color, SV_FRAMETIME, true);
                }
            }
        }
    }

}
#endif

static void Nav_UpdateConditionalNode(nav_node_t *node)
{
    node->flags &= ~NodeFlag_Disabled;

    //NodeFlag_CheckDoorLinks | NodeFlag_CheckForHazard | NodeFlag_CheckHasFloor | NodeFlag_CheckInLiquid | NodeFlag_CheckInSolid

    vec3_t mins, maxs, origin;
    Nav_GetNodeBounds(node, mins, maxs);
    Nav_GetNodeTraceOrigin(node, origin);

    if (node->flags & NodeFlag_CheckInSolid) {
        trace_t tr = SV_Trace(origin, mins, maxs, origin, NULL, MASK_SOLID);

        if (tr.startsolid || tr.allsolid) {
            node->flags |= NodeFlag_Disabled;
            return;
        }
    }

    if (node->flags & NodeFlag_CheckInLiquid) {
        trace_t tr = SV_Trace(origin, mins, maxs, origin, NULL, MASK_WATER);

        if (!(tr.startsolid || tr.allsolid)) {
            node->flags |= NodeFlag_Disabled;
            return;
        }
    }

    if (node->flags & NodeFlag_CheckForHazard) {
        trace_t tr = SV_Trace(origin, mins, maxs, origin, NULL, CONTENTS_SLIME | CONTENTS_LAVA);

        if (tr.startsolid || tr.allsolid) {
            node->flags |= NodeFlag_Disabled;
        } else {
            vec3_t absmin, absmax;
            VectorAdd(origin, mins, absmin);
            VectorAdd(origin, maxs, absmax);

            for (size_t i = 0; i < nav_data.num_registered_edicts; i++) {
                const edict_t *e = nav_data.registered_edicts[i];

                if (!e)
                    continue;

                if (!(e->sv.ent_flags & SVFL_TRAP_DANGER))
                    continue;

                if (e->s.renderfx & RF_BEAM) {
                    if (e->svflags & SVF_NOCLIENT)
                        continue;

                    if (IntersectBoundLine(absmin, absmax, e->s.origin, e->s.old_origin)) {
                        node->flags |= NodeFlag_Disabled;
                        return;
                    }
                } else if (e->solid == SOLID_TRIGGER) {
                    if (IntersectBounds(e->absmin, e->absmax, absmin, absmax)) {
                        node->flags |= NodeFlag_Disabled;
                        return;
                    }
                }
            }
        }
    }

    if (node->flags & NodeFlag_CheckHasFloor) {
        vec3_t flat_mins, flat_maxs;
        flat_mins[0] = mins[0];
        flat_mins[1] = mins[1];
        flat_maxs[0] = maxs[0];
        flat_maxs[1] = maxs[1];
        flat_mins[2] = flat_maxs[2] = 0.f;

        vec3_t floor_end;
        VectorCopy(origin, floor_end);
        floor_end[2] -= NavFloorDistance;

        trace_t tr = SV_Trace(origin, flat_mins, flat_maxs, floor_end, NULL, MASK_SOLID);

        if (tr.fraction == 1.0f) {
            node->flags |= NodeFlag_Disabled;
            return;
        }
    }

    if (node->flags & NodeFlag_CheckDoorLinks) {
        for (nav_link_t *link = node->links; link != node->links + node->num_links; link++) {
            if (!link->edict)
                continue;
            else if (!link->edict->game_edict)
                continue;
            
            const edict_t *game_edict = link->edict->game_edict;

            if (!game_edict->inuse)
                continue;

            if (game_edict->sv.ent_flags & SVFL_IS_LOCKED_DOOR) {
                node->flags |= NodeFlag_Disabled;
                return;
            }
        }
    }
}

static void Nav_SetupEntities(void)
{
    nav_data.setup_entities = true;

    for (int i = 0; i < nav_data.num_edicts; i++) {
        nav_edict_t *e = &nav_data.edicts[i];

        for (int n = 0; n < ge->num_edicts; n++) {
            edict_t *game_e = EDICT_NUM(n);

            if (!game_e->inuse)
                continue;
            else if (game_e->solid != SOLID_TRIGGER && game_e->solid != SOLID_BSP)
                continue;

            if (game_e->s.modelindex == e->model) {
                e->game_edict = game_e;
                break;
            }
        }

        if (!e->game_edict)
            Com_WPrintf("Nav entity %i appears to be missing (needs entity with model %i)\n", i, e->model);
    }
}

void Nav_Frame(void)
{
    nav_data.nav_frame++;

    if (nav_data.nav_frame > sv_fps->integer)
        if (!nav_data.setup_entities)
            Nav_SetupEntities();

    for (int i = 0; i < nav_data.num_conditional_nodes; i++)
        Nav_UpdateConditionalNode(nav_data.conditional_nodes[i]);

#if USE_REF
    Nav_Debug();
#endif
}

void Nav_Init(void)
{
#if USE_REF
    nav_debug = Cvar_Get("nav_debug", "0", 0);
    nav_debug_range = Cvar_Get("nav_debug_range", "512", 0);
#endif
}

void Nav_Shutdown(void)
{
    Z_LeakTest(TAG_NAV);
}

void Nav_RegisterEdict(const edict_t *edict)
{
    size_t free_slot = nav_data.num_registered_edicts;

    for (size_t i = 0; i < nav_data.num_registered_edicts; i++) {
        if (nav_data.registered_edicts[i] == edict) {
            return;
        } else if (nav_data.registered_edicts[i] == NULL) {
            free_slot = i;
        }
    }

    nav_data.registered_edicts[free_slot] = edict;

    if (free_slot == nav_data.num_registered_edicts) {
        nav_data.num_registered_edicts++;
    }
}

void Nav_UnRegisterEdict(const edict_t *edict)
{
    for (int i = 0; i < nav_data.num_edicts; i++) {
        if (nav_data.edicts[i].game_edict == edict) {
            nav_data.edicts[i].game_edict = NULL;
            break;
        }
    }

    for (size_t i = 0; i < nav_data.num_registered_edicts; i++) {
        if (nav_data.registered_edicts[i] == edict) {
            nav_data.registered_edicts[i] = NULL;

            for (i = nav_data.num_registered_edicts - 1; i >= 0; --i) {
                if (nav_data.registered_edicts[i] == NULL) {
                    nav_data.num_registered_edicts--;
                    continue;
                }

                return;
            }
            break;
        }
    }
}
