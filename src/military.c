/* ************************************************************************
*   File: military.c                                     EmpireMUD 2.0b5 *
*  Usage: citizen garrisons and military response                          *
*                                                                         *
*  EmpireMUD code base by Paul Clarke, (C) 2000-2024                        *
*  All rights reserved.  See license.doc for complete information.          *
*                                                                         *
*  EmpireMUD based upon CircleMUD 3.0, bpl 17, by Jeremy Elson.             *
*  CircleMUD (C) 1993, 94 by the Trustees of the Johns Hopkins University   *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.                 *
************************************************************************ */

#include "conf.h"
#include "sysdep.h"

#include "structs.h"
#include "utils.h"
#include "comm.h"
#include "interpreter.h"
#include "handler.h"
#include "db.h"
#include "skills.h"
#include "vnums.h"
#include "constants.h"
#include "dg_scripts.h"

/**
* Contents:
*   Data
*   Helpers
*   Routes
*   Soldier Handlers
*   Military Update
*   Commands
*/


 /////////////////////////////////////////////////////////////////////////////
//// DATA ///////////////////////////////////////////////////////////////////

#define MILITARY_MAX_ROUTE  4096
#define MILITARY_ROUTES_PER_UPDATE  16

// Only room vnums and character UIDs survive between updates.
struct military_garrison {
	room_vnum vnum;
	empire_vnum empire;
	int target;
	int blocked_target;
	time_t last_threat;
	time_t retry_after;
	bool marked;
	UT_hash_handle hh;
};

struct military_unit {
	int id;
	bool returning;
	time_t blocked_since;
	time_t retry_after;
	time_t waiting_since;
	room_vnum destination;
	room_vnum path_origin;
	char *path;
	int path_pos;
	time_t path_time;
	UT_hash_handle hh;
};

struct military_path_node {
	room_vnum vnum;
	room_data *room;
	int dir;
	struct military_path_node *parent;
	struct military_path_node *next;
	UT_hash_handle hh;
};

static struct military_garrison *military_garrisons = NULL;
static struct military_unit *military_units = NULL;
static bool military_registry_changed = TRUE;
static time_t military_registry_time = 0;
static int military_routes_left = 0;
static int military_route_turn = 0;


 /////////////////////////////////////////////////////////////////////////////
//// HELPERS /////////////////////////////////////////////////////////////////

/**
* Marks the registry for rebuilding after a building or ownership change.
*/
void military_refresh(void) {
	military_registry_changed = TRUE;
}


/**
* @param room_data *home The soldier's barracks.
* @return bool TRUE if military food is available on its island.
*/
static bool military_is_supplied(room_data *home) {
	empire_data *emp = home ? ROOM_OWNER(home) : NULL;

	return emp && (EMPIRE_ADMIN_FLAGGED(emp, EADM_FREE_NEEDS) || !empire_has_needs_status(emp, GET_ISLAND_ID(home), ENEED_MILITARY, ENEED_STATUS_UNSUPPLIED));
}


/**
* @param struct empire_npc_data *npc The enlisted citizen.
* @return bool TRUE if the citizen still has a position in this garrison.
*/
static bool military_has_position(struct empire_npc_data *npc) {
	struct empire_territory_data *ter;
	struct empire_npc_data *iter;
	room_data *home = npc ? npc->home : NULL;
	int count = 0, limit;

	if (!home || !is_garrison(home) || npc->empire_id != EMPIRE_VNUM(ROOM_OWNER(home)) || !(ter = find_territory_entry(ROOM_OWNER(home), home))) {
		return FALSE;
	}
	limit = get_garrison_limit(home);
	LL_FOREACH(ter->npcs, iter) {
		if (iter->vnum != SOLDIER) {
			continue;
		}
		if (iter == npc) {
			return count < limit;
		}
		++count;
	}
	return FALSE;
}


/**
* Ordinary guards are unaffected. Withdrawing or unsupplied soldiers may not
* start another fight through the normal cityguard event.
*
* @param char_data *ch The guard.
* @return bool TRUE if it may initiate an engagement.
*/
bool military_can_aggro(char_data *ch) {
	struct military_unit *unit;
	room_data *home;
	int id;

	if (!is_military_soldier(ch)) {
		return TRUE;
	}
	home = GET_EMPIRE_NPC_DATA(ch)->home;
	if (!military_has_position(GET_EMPIRE_NPC_DATA(ch)) || !military_is_supplied(home) || !IN_ROOM(ch) || ROOM_OWNER(IN_ROOM(ch)) != GET_LOYALTY(ch) || compute_distance(home, IN_ROOM(ch)) > config_get_int("military_radius")) {
		return FALSE;
	}
	id = char_script_id(ch);
	HASH_FIND_INT(military_units, &id, unit);
	return !unit || !unit->returning;
}


/**
* @param char_data *ch The soldier.
* @return bool TRUE if it is free to move or begin responding.
*/
static bool military_can_act(char_data *ch) {
	char_data *iter;

	if (!ch || EXTRACTED(ch) || IS_DEAD(ch) || FIGHTING(ch) || GET_POS(ch) < POS_STANDING || ch->desc || GET_FED_ON_BY(ch) || GET_LED_BY(ch) || MOB_FLAGGED(ch, MOB_TIED) || AFF_FLAGGED(ch, AFF_CHARM | AFF_STUNNED | AFF_HARD_STUNNED | AFF_IMMOBILIZED)) {
		return FALSE;
	}
	DL_FOREACH2(ROOM_PEOPLE(IN_ROOM(ch)), iter, next_in_room) {
		if (FIGHTING(iter) == ch) {
			return FALSE;
		}
	}
	return TRUE;
}


/**
* Damaging effects may keep a soldier active without preventing movement.
*
* @param char_data *ch The soldier.
* @return bool TRUE if it is safe to remove its active mob.
*/
static bool military_can_stand_down(char_data *ch) {
	return military_can_act(ch) && !ch->over_time_effects;
}


/**
* Rebuilds the small registry from owned territory, not from the world map.
*/
static void refresh_military_garrisons(void) {
	struct military_garrison *garrison, *next_garrison;
	struct empire_territory_data *ter, *next_ter;
	empire_data *emp, *next_emp;
	room_data *home;
	time_t now = time(0);

	if (!military_registry_changed && military_registry_time + 61 > now) {
		return;
	}
	military_registry_changed = FALSE;
	military_registry_time = now;
	HASH_ITER(hh, military_garrisons, garrison, next_garrison) {
		garrison->marked = FALSE;
	}
	HASH_ITER(hh, empire_table, emp, next_emp) {
		HASH_ITER(hh, EMPIRE_TERRITORY_LIST(emp), ter, next_ter) {
			home = ter->room;
			if (!home || HOME_ROOM(home) != home || !is_garrison(home) || ROOM_OWNER(home) != emp) {
				continue;
			}
			reconcile_garrison(home);
			HASH_FIND_INT(military_garrisons, &ter->vnum, garrison);
			if (!garrison) {
				CREATE(garrison, struct military_garrison, 1);
				garrison->vnum = ter->vnum;
				HASH_ADD_INT(military_garrisons, vnum, garrison);
			}
			if (garrison->empire != EMPIRE_VNUM(emp)) {
				garrison->target = 0;
				garrison->last_threat = 0;
				garrison->blocked_target = 0;
				garrison->retry_after = 0;
			}
			garrison->empire = EMPIRE_VNUM(emp);
			garrison->marked = TRUE;
		}
	}
	HASH_ITER(hh, military_garrisons, garrison, next_garrison) {
		if (!garrison->marked) {
			HASH_DEL(military_garrisons, garrison);
			free(garrison);
		}
	}
}


/**
* @param room_data *home The barracks.
* @param char_data *vict The potential threat.
* @return bool TRUE if this is a visible threat in the defended area.
*/
static bool military_is_threat(room_data *home, char_data *vict) {
	char_data *ally;
	room_data *room;
	empire_data *emp = ROOM_OWNER(home);

	if (!vict || EXTRACTED(vict) || IS_DEAD(vict) || !(room = IN_ROOM(vict)) || ROOM_OWNER(room) != emp || GET_ROOM_VEHICLE(room) || IS_ADVENTURE_ROOM(room) || GET_ISLAND_ID(room) != GET_ISLAND_ID(home) || compute_distance(home, room) > config_get_int("military_radius")) {
		return FALSE;
	}
	if (ISLAND_FLAGGED(room, ISLE_NO_AGGRO) || RMT_FLAGGED(room, RMT_PEACEFUL) || ROOM_SECT_FLAGGED(room, SECTF_START_LOCATION) || NOHASSLE(vict) || (!IS_NPC(vict) && (IS_IMMORTAL(vict) || PRF_FLAGGED(vict, PRF_WIZHIDE) || !vict->desc)) || AFF_FLAGGED(vict, AFF_INVISIBLE | AFF_HIDDEN | AFF_IMMUNE_PHYSICAL | AFF_NO_ATTACK | AFF_NO_SEE_IN_ROOM | AFF_NO_TARGET_IN_ROOM)) {
		return FALSE;
	}
	if (GET_LOYALTY(vict) == emp || (GET_LOYALTY(vict) && empire_is_friendly(emp, GET_LOYALTY(vict)))) {
		return FALSE;
	}
	if (empire_guard_is_enemy(emp, vict, room)) {
		return TRUE;
	}
	DL_FOREACH2(ROOM_PEOPLE(room), ally, next_in_room) {
		if (GET_LOYALTY(ally) == emp && FIGHTING(ally) == vict && !(!IS_NPC(ally) && !IS_NPC(vict) && IS_PVP_FLAGGED(ally))) {
			return TRUE;
		}
	}
	return FALSE;
}


/**
* @param room_data *home The defended barracks.
* @param struct military_garrison *garrison Its current assignment.
* @return char_data* The highest priority threat, or NULL.
*/
static char_data *find_military_threat(room_data *home, struct military_garrison *garrison) {
	char_data *vict, *best = NULL;
	int priority, best_priority = -1, distance, best_distance = INT_MAX, id;

	DL_FOREACH(character_list, vict) {
		if (!military_is_threat(home, vict)) {
			continue;
		}
		id = char_script_id(vict);
		if (garrison->blocked_target == id && garrison->retry_after > time(0)) {
			continue;
		}
		priority = FIGHTING(vict) && GET_LOYALTY(FIGHTING(vict)) == ROOM_OWNER(home) ? 1 : 0;
		distance = compute_distance(home, IN_ROOM(vict));
		if (!best || priority > best_priority || (priority == best_priority && (distance < best_distance || (distance == best_distance && id < char_script_id(best))))) {
			best = vict;
			best_priority = priority;
			best_distance = distance;
		}
	}
	return best;
}


 /////////////////////////////////////////////////////////////////////////////
//// ROUTES //////////////////////////////////////////////////////////////////

/**
* @param char_data *ch The soldier.
* @param room_data *from The previous room.
* @param int dir Which exit.
* @param room_data *to The next room.
* @return bool TRUE if this edge is within the leash and normally traversable.
*/
static bool military_route_edge(char_data *ch, room_data *from, int dir, room_data *to) {
	room_data *home = GET_EMPIRE_NPC_DATA(ch)->home;
	struct room_direction_data *ex;

	if (!to || !home || GET_ROOM_VEHICLE(to) || IS_ADVENTURE_ROOM(to) || (ROOM_OWNER(to) && ROOM_OWNER(to) != GET_LOYALTY(ch)) || GET_ISLAND_ID(to) != GET_ISLAND_ID(home) || compute_distance(home, to) > config_get_int("military_radius")) {
		return FALSE;
	}
	if (ROOM_IS_CLOSED(from) && (ex = find_exit(from, dir)) && EXIT_FLAGGED(ex, EX_CLOSED)) {
		return FALSE;
	}
	return validate_mobile_move_from(ch, from, dir, to, FALSE);
}


/**
* Builds a bounded breadth-first route. Each byte is a direction plus one;
* there are no portals or vehicle transitions in this route.
*
* @param char_data *ch The soldier.
* @param room_data *destination Where to go.
* @return char* Allocated directions, or NULL when no route is available.
*/
static char *find_military_route(char_data *ch, room_data *destination) {
	struct military_path_node *nodes = NULL, *node, *next_node, *tail, *found, *iter;
	room_data *to;
	char *path = NULL;
	int dir, count = 1, length, pos;
	room_vnum vnum;

	if (IN_ROOM(ch) == destination) {
		return str_dup("");
	}
	CREATE(node, struct military_path_node, 1);
	node->room = IN_ROOM(ch);
	node->vnum = GET_ROOM_VNUM(node->room);
	HASH_ADD_INT(nodes, vnum, node);
	tail = node;
	found = NULL;
	while (node && !found && count < MILITARY_MAX_ROUTE) {
		for (dir = 0; dir < (ROOM_IS_CLOSED(node->room) ? NUM_NATURAL_DIRS : NUM_2D_DIRS); ++dir) {
			if (!(to = dir_to_room(node->room, dir, FALSE)) || !military_route_edge(ch, node->room, dir, to)) {
				continue;
			}
			vnum = GET_ROOM_VNUM(to);
			HASH_FIND_INT(nodes, &vnum, iter);
			if (iter) {
				continue;
			}
			CREATE(iter, struct military_path_node, 1);
			iter->vnum = vnum;
			iter->room = to;
			iter->dir = dir;
			iter->parent = node;
			HASH_ADD_INT(nodes, vnum, iter);
			tail->next = iter;
			tail = iter;
			++count;
			if (to == destination) {
				found = iter;
				break;
			}
			if (count >= MILITARY_MAX_ROUTE) {
				break;
			}
		}
		node = node->next;
	}
	if (found) {
		length = 0;
		for (iter = found; iter->parent; iter = iter->parent) {
			++length;
		}
		CREATE(path, char, length + 1);
		pos = length;
		for (iter = found; iter->parent; iter = iter->parent) {
			path[--pos] = iter->dir + 1;
		}
	}
	HASH_ITER(hh, nodes, node, next_node) {
		HASH_DEL(nodes, node);
		free(node);
	}
	return path;
}


/**
* @param struct military_unit *unit A soldier's assignment.
*/
static void clear_military_route(struct military_unit *unit) {
	if (unit->path) {
		free(unit->path);
		unit->path = NULL;
	}
	unit->path_pos = 0;
	unit->destination = NOWHERE;
}


// Releases an old search request when a soldier no longer needs that search.
static void clear_military_wait(struct military_unit *unit) {
	unit->waiting_since = 0;
	if (military_route_turn == unit->id) {
		military_route_turn = 0;
	}
}


/**
* Applies accepted immortal config changes to pending recruitment and routes.
*
* @param char *key The military config key that changed.
*/
void military_config_changed(char *key) {
	struct empire_territory_data *ter, *next_ter;
	struct military_garrison *garrison, *next_garrison;
	struct military_unit *unit, *next_unit;
	empire_data *emp, *next_emp;

	if (!str_cmp(key, "military_recruitment_interval")) {
		HASH_ITER(hh, empire_table, emp, next_emp) {
			HASH_ITER(hh, EMPIRE_TERRITORY_LIST(emp), ter, next_ter) {
				if (ter->room && is_garrison(ter->room) && ROOM_OWNER(ter->room) == emp) {
					reset_garrison_recruitment(ter->room);
				}
			}
		}
	}
	if (!str_cmp(key, "military_radius") || !str_cmp(key, "military_route_retry_time")) {
		HASH_ITER(hh, military_garrisons, garrison, next_garrison) {
			garrison->blocked_target = 0;
			garrison->retry_after = 0;
		}
		HASH_ITER(hh, military_units, unit, next_unit) {
			clear_military_route(unit);
			clear_military_wait(unit);
			unit->retry_after = 0;
		}
	}
	military_refresh();
}


/**
* Walks one step, sharing a newly calculated route with other soldiers that
* start in the same place. A busy update defers searches rather than failing.
*
* @return int One for a move, zero for no route, minus one for a deferred search.
*/
static int move_military_soldier(char_data *ch, struct military_unit *unit, room_data *destination) {
	struct military_unit *other, *next_other;
	room_data *to;
	room_vnum origin = GET_ROOM_VNUM(IN_ROOM(ch)), destination_vnum = GET_ROOM_VNUM(destination);
	int dir, id = unit->id;
	time_t now = time(0);
	bool moved;

	if (IN_ROOM(ch) == destination) {
		clear_military_wait(unit);
		return 1;
	}
	if (unit->destination != GET_ROOM_VNUM(destination)) {
		clear_military_route(unit);
		unit->retry_after = 0;
	}
	if (unit->retry_after > now) {
		clear_military_wait(unit);
		return 0;
	}
	if (!unit->path || !unit->path[unit->path_pos]) {
		clear_military_route(unit);
		HASH_ITER(hh, military_units, other, next_other) {
			if (other != unit && other->path && other->path_origin == origin && other->destination == GET_ROOM_VNUM(destination) && other->path_time == now) {
				unit->path = str_dup(other->path);
				break;
			}
		}
		if (!unit->path) {
			if ((military_route_turn && military_route_turn != unit->id) || military_routes_left <= 0) {
				if (!unit->waiting_since) {
					unit->waiting_since = now;
				}
				return -1;
			}
			clear_military_wait(unit);
			--military_routes_left;
			unit->path = find_military_route(ch, destination);
		}
		clear_military_wait(unit);
		unit->destination = GET_ROOM_VNUM(destination);
		unit->path_origin = origin;
		unit->path_time = now;
		if (!unit->path) {
			unit->retry_after = now + config_get_int("military_route_retry_time");
			return 0;
		}
	}
	dir = (unsigned char)unit->path[unit->path_pos] - 1;
	to = dir_to_room(IN_ROOM(ch), dir, FALSE);
	if (!military_route_edge(ch, IN_ROOM(ch), dir, to)) {
		clear_military_route(unit);
		unit->destination = GET_ROOM_VNUM(destination);
		unit->retry_after = now + config_get_int("military_route_retry_time");
		return 0;
	}
	moved = perform_move(ch, dir, to, MOVE_WANDER);
	ch = find_char(id);
	HASH_FIND_INT(military_units, &id, unit);
	if (!ch || !unit || EXTRACTED(ch) || IS_DEAD(ch) || !is_military_soldier(ch)) {
		return -2;	// a movement trigger removed the soldier or its assignment
	}
	if (!moved) {
		clear_military_route(unit);
		unit->destination = destination_vnum;
		unit->retry_after = now + config_get_int("military_route_retry_time");
		return 0;
	}
	++unit->path_pos;
	return 1;
}


 /////////////////////////////////////////////////////////////////////////////
//// SOLDIER HANDLERS ////////////////////////////////////////////////////////

/**
* Copies live health into the citizen record before save or stand-down.
*
* @param char_data *ch A possible soldier.
*/
void military_snapshot_soldier(char_data *ch) {
	struct empire_npc_data *npc;
	empire_data *emp;
	int health, scale;
	time_t recovery;

	if (!is_military_soldier(ch) || IS_DEAD(ch) || GET_MAX_HEALTH(ch) <= 0) {
		return;
	}
	npc = GET_EMPIRE_NPC_DATA(ch);
	health = MAX(1, MIN(1000, (int)round(1000.0 * GET_HEALTH(ch) / GET_MAX_HEALTH(ch))));
	scale = GET_CURRENT_SCALE_LEVEL(ch);
	recovery = health < 1000 ? npc->soldier_recovery : 0;
	if (health < 1000 && !recovery) {
		recovery = time(0) + MOB_RESTORE_INTERVAL;
	}
	if (npc->soldier_health != health || npc->soldier_scale != scale || npc->soldier_recovery != recovery) {
		npc->soldier_health = health;
		npc->soldier_scale = scale;
		npc->soldier_recovery = recovery;
		if ((emp = real_empire(npc->empire_id))) {
			EMPIRE_NEEDS_SAVE(emp) = TRUE;
		}
	}
}


/**
* @param char_data *ch The soldier that took damage.
*/
void military_note_damage(char_data *ch) {
	if (is_military_soldier(ch)) {
		GET_EMPIRE_NPC_DATA(ch)->soldier_recovery = time(0) + MOB_RESTORE_INTERVAL;
		military_snapshot_soldier(ch);
	}
}


/**
* Frees a character's transient assignment before extraction. Safe on other
* mobs, including a soldier whose citizen record has already been deleted.
*
* @param char_data *ch The character being extracted.
*/
void military_forget_soldier(char_data *ch) {
	struct military_unit *unit;
	int id;

	if (!ch || !IS_NPC(ch)) {
		return;
	}
	id = char_script_id(ch);
	HASH_FIND_INT(military_units, &id, unit);
	if (unit) {
		clear_military_wait(unit);
		clear_military_route(unit);
		HASH_DEL(military_units, unit);
		free(unit);
	}
}


/**
* @param char_data *ch The soldier.
* @return struct military_unit* Its transient assignment.
*/
static struct military_unit *get_military_unit(char_data *ch) {
	struct military_unit *unit;
	int id = char_script_id(ch);

	HASH_FIND_INT(military_units, &id, unit);
	if (!unit) {
		CREATE(unit, struct military_unit, 1);
		unit->id = id;
		unit->destination = NOWHERE;
		HASH_ADD_INT(military_units, id, unit);
	}
	return unit;
}


/**
* @param char_data *ch The newly spawned soldier.
* @param char_data *target Its first opponent, used for ordinary mob scaling.
*/
static void restore_military_soldier(char_data *ch, char_data *target) {
	struct empire_npc_data *npc = GET_EMPIRE_NPC_DATA(ch);
	int level = npc->soldier_scale;

	if (level <= 0) {
		level = target ? determine_best_scale_level(target, TRUE) : GET_MIN_SCALE_LEVEL(ch);
	}
	scale_mob_to_level(ch, level);
	if (npc->soldier_recovery && npc->soldier_recovery <= time(0)) {
		npc->soldier_health = 1000;
		npc->soldier_recovery = 0;
	}
	set_health(ch, MAX(1, (int)round(GET_MAX_HEALTH(ch) * MAX(1, MIN(1000, npc->soldier_health)) / 1000.0)));
	if (GET_HEALTH(ch) < GET_MAX_HEALTH(ch)) {
		schedule_reset_mob(ch);
	}
	get_military_unit(ch);
}


/**
* @param char_data *ch The soldier to dematerialize.
*/
static void stand_down_soldier(char_data *ch) {
	military_snapshot_soldier(ch);
	act("$n stands down and returns to quarters.", TRUE, ch, NULL, NULL, TO_ROOM | DG_NO_TRIG);
	military_forget_soldier(ch);
	despawn_mob(ch);
}


 /////////////////////////////////////////////////////////////////////////////
//// MILITARY UPDATE /////////////////////////////////////////////////////////

/**
* Checks the optional seconds-based recruitment timer once per second.
* The normal empire population update retains its original cadence at zero.
*/
void military_recruitment_update(void) {
	struct military_garrison *garrison, *next_garrison;
	struct empire_territory_data *ter;
	empire_data *emp;
	room_data *home;
	int idle_time = config_get_int("time_to_empire_emptiness") * SECS_PER_REAL_WEEK;

	if (config_get_int("military_recruitment_interval") <= 0) {
		return;
	}
	refresh_military_garrisons();
	HASH_ITER(hh, military_garrisons, garrison, next_garrison) {
		home = real_room(garrison->vnum);
		emp = home ? ROOM_OWNER(home) : NULL;
		if (!emp || !EMPIRE_HAS_TECH(emp, TECH_CITIZENS) || EMPIRE_LAST_LOGON(emp) + idle_time < time(0) || ROOM_PRIVATE_OWNER(home) != NOBODY) {
			continue;
		}
		if ((ter = find_territory_entry(emp, home))) {
			populate_garrison(emp, ter, FALSE);
		}
	}
}


/**
* Activates and directs garrisons independently of player-driven mob spawning.
* Called on the configurable military response interval from the heartbeat.
*/
void military_update(void) {
	struct military_garrison *garrison, *next_garrison;
	struct military_unit *unit, *next_unit;
	struct empire_territory_data *ter;
	struct empire_npc_data *npc;
	char_data *ch, *next_ch, *target;
	room_data *home;
	empire_data *emp;
	int count, limit, moved;
	time_t now = time(0);
	time_t oldest_wait = now + 1;
	bool retiring, supplied;

	refresh_military_garrisons();
	military_routes_left = MILITARY_ROUTES_PER_UPDATE;
	military_route_turn = 0;
	HASH_ITER(hh, military_units, unit, next_unit) {
		if (unit->waiting_since && unit->retry_after <= now && find_char(unit->id) && (unit->waiting_since < oldest_wait || (unit->waiting_since == oldest_wait && unit->id < military_route_turn))) {
			military_route_turn = unit->id;
			oldest_wait = unit->waiting_since;
		}
	}
	HASH_ITER(hh, military_garrisons, garrison, next_garrison) {
		home = real_room(garrison->vnum);
		garrison->target = 0;
		if (!home || !is_garrison(home) || EMPIRE_VNUM(ROOM_OWNER(home)) != garrison->empire || get_garrison_limit(home) <= 0 || !military_is_supplied(home)) {
			continue;
		}
		target = find_military_threat(home, garrison);
		if (!target) {
			continue;
		}
		garrison->target = char_script_id(target);
		garrison->last_threat = now;
		emp = ROOM_OWNER(home);
		if (!(ter = find_territory_entry(emp, home)) || !mob_proto(SOLDIER)) {
			continue;
		}
		for (count = 0, limit = get_garrison_limit(home); count < limit; ++count) {
			// Resolve the roster again after each spawn's load trigger.
			home = real_room(garrison->vnum);
			if (!home || !is_garrison(home) || ROOM_OWNER(home) != emp || !(ter = find_territory_entry(emp, home))) {
				break;
			}
			LL_FOREACH(ter->npcs, npc) {
				if (npc->vnum == SOLDIER && !npc->mob && military_has_position(npc)) {
					break;
				}
			}
			if (!npc) {
				break;
			}
			ch = spawn_empire_npc_to_room(emp, npc, home, NOTHING);
			if (ch && !EXTRACTED(ch) && is_military_soldier(ch)) {
				target = find_char(garrison->target);
				restore_military_soldier(ch, target);
			}
			else {
				break;
			}
		}
	}

	DL_FOREACH_SAFE(character_list, ch, next_ch) {
		if (!is_military_soldier(ch) || EXTRACTED(ch) || IS_DEAD(ch)) {
			continue;
		}
		npc = GET_EMPIRE_NPC_DATA(ch);
		home = npc->home;
		emp = real_empire(npc->empire_id);
		unit = get_military_unit(ch);
		military_snapshot_soldier(ch);
		if (FIGHTING(ch)) {
			clear_military_wait(unit);
			unit->blocked_since = 0;
			continue;
		}
		if (!military_can_act(ch)) {
			clear_military_wait(unit);
			continue;
		}
		retiring = !military_has_position(npc);
		supplied = home && military_is_supplied(home);
		garrison = NULL;
		if (home) {
			room_vnum vnum = GET_ROOM_VNUM(home);
			HASH_FIND_INT(military_garrisons, &vnum, garrison);
		}
		target = garrison && garrison->target ? find_char(garrison->target) : NULL;
		if (!retiring && supplied && target && military_is_threat(home, target)) {
			unit->returning = FALSE;
			if (IN_ROOM(ch) == IN_ROOM(target)) {
				clear_military_wait(unit);
				unit->blocked_since = 0;
				if (CAN_SEE(ch, target) && can_fight(ch, target)) {
					engage_combat(ch, target, FALSE);
				}
				else {
					garrison->blocked_target = garrison->target;
					garrison->retry_after = now + config_get_int("military_route_retry_time");
				}
				continue;
			}
			moved = move_military_soldier(ch, unit, IN_ROOM(target));
			if (moved == 0) {
				garrison->blocked_target = garrison->target;
				garrison->retry_after = now + config_get_int("military_route_retry_time");
			}
			if (moved > 0) {
				unit->blocked_since = 0;
				target = find_char(garrison->target);
				if (!EXTRACTED(ch) && target && !EXTRACTED(target) && !IS_DEAD(target) && IN_ROOM(ch) == IN_ROOM(target) && CAN_SEE(ch, target) && can_fight(ch, target)) {
					engage_combat(ch, target, FALSE);
				}
			}
			continue;
		}
		if (!retiring && supplied && garrison && garrison->last_threat + config_get_int("military_stand_down_time") > now) {
			clear_military_wait(unit);
			continue;
		}
		unit->returning = TRUE;
		if (!home || !is_garrison(home) || ROOM_OWNER(home) != emp) {
			clear_military_wait(unit);
			if (emp && military_can_stand_down(ch)) {
				discharge_soldier(emp, npc);
			}
			continue;
		}
		if (IN_ROOM(ch) == home) {
			clear_military_wait(unit);
			if (!military_can_stand_down(ch)) {
				continue;
			}
			if (retiring) {
				discharge_soldier(emp, npc);
			}
			else {
				stand_down_soldier(ch);
			}
			continue;
		}
		moved = move_military_soldier(ch, unit, home);
		if (moved > 0) {
			unit->blocked_since = 0;
		}
		else if (moved == 0) {
			if (!unit->blocked_since) {
				unit->blocked_since = now;
			}
			else if (unit->blocked_since + config_get_int("military_blocked_return_time") <= now && military_can_stand_down(ch)) {
				if (retiring) {
					discharge_soldier(emp, npc);
				}
				else {
					stand_down_soldier(ch);
				}
			}
		}
	}
}


 /////////////////////////////////////////////////////////////////////////////
//// COMMANDS ////////////////////////////////////////////////////////////////

/**
* @param struct empire_npc_data *npc The enlisted citizen.
* @return const char* A display status for the roster.
*/
static const char *military_soldier_status(struct empire_npc_data *npc) {
	struct military_unit *unit;
	int id;

	if (npc->mob && !EXTRACTED(npc->mob)) {
		if (FIGHTING(npc->mob)) {
			return "fighting";
		}
		id = char_script_id(npc->mob);
		HASH_FIND_INT(military_units, &id, unit);
		return unit && unit->returning ? "returning" : "responding";
	}
	if (!military_has_position(npc)) {
		return "awaiting discharge";
	}
	return military_is_supplied(npc->home) ? "off duty" : "unsupplied";
}


/**
* @param char_data *ch The player.
* @param room_data *home The barracks to display.
* @param bool roster Include named soldiers.
*/
static void show_military_garrison(char_data *ch, room_data *home, bool roster) {
	struct empire_territory_data *ter;
	struct empire_npc_data *npc;
	struct generic_name_data *names;
	struct military_garrison *garrison;
	const char *name, *status;
	char limit[32];
	int active = 0, ready = 0, target, health, interval, remaining;
	room_vnum vnum = GET_ROOM_VNUM(home);

	if (!(ter = find_territory_entry(ROOM_OWNER(home), home))) {
		return;
	}
	target = get_room_extra_data(home, ROOM_EXTRA_GARRISON_LIMIT);
	if (target == -1) {
		strcpy(limit, "max");
	}
	else if (target <= 0) {
		strcpy(limit, "off");
	}
	else {
		snprintf(limit, sizeof(limit), "%d", target);
	}
	LL_FOREACH(ter->npcs, npc) {
		if (npc->vnum == SOLDIER && npc->mob && !EXTRACTED(npc->mob)) {
			++active;
		}
		else if (npc->vnum == SOLDIER && military_has_position(npc) && military_is_supplied(home)) {
			++ready;
		}
	}
	status = !get_garrison_limit(home) ? "recruitment disabled" : (!military_is_supplied(home) ? "insufficient food" : "ready");
	HASH_FIND_INT(military_garrisons, &vnum, garrison);
	if (garrison && garrison->retry_after > time(0)) {
		status = "no valid route or target";
	}
	build_page_display(ch, "%s%s: %d/%d enlisted, %d ready, %d active, target %s (%s)", get_room_name(home, FALSE), coord_display_room(ch, home, FALSE), get_garrison_population(home), get_garrison_capacity(home), ready, active, limit, status);
	if (roster) {
		interval = config_get_int("military_recruitment_interval");
		build_page_display(ch, "  Housing: %d base + %d bedroom%s. Recruitment: %s", GET_BLD_GARRISON_CAPACITY(GET_BUILDING(home)), get_garrison_capacity(home) - GET_BLD_GARRISON_CAPACITY(GET_BUILDING(home)), PLURAL(get_garrison_capacity(home) - GET_BLD_GARRISON_CAPACITY(GET_BUILDING(home))), interval > 0 ? "military timer running" : (ter->population_timer > 0 ? "population timer running" : "waiting for an eligible citizen"));
		if (get_garrison_population(home) < get_garrison_limit(home) && interval > 0) {
			remaining = MAX(0, get_room_extra_data(home, ROOM_EXTRA_GARRISON_RECRUIT_TIME) - time(0));
			build_page_display(ch, "  Next recruitment check in about %d second%s.", remaining, PLURAL(remaining));
		}
		else if (get_garrison_population(home) < get_garrison_limit(home) && ter->population_timer > 0) {
			build_page_display(ch, "  Next recruitment check in about %d minute%s.", ter->population_timer, PLURAL(ter->population_timer));
		}
		LL_FOREACH(ter->npcs, npc) {
			if (npc->vnum != SOLDIER) {
				continue;
			}
			names = get_best_name_list(mob_proto(SOLDIER) ? MOB_NAME_SET(mob_proto(SOLDIER)) : 0, npc->sex);
			name = names && npc->name >= 0 && npc->name < names->size ? names->names[npc->name] : "Soldier";
			health = npc->soldier_health;
			if (npc->mob && !EXTRACTED(npc->mob) && GET_MAX_HEALTH(npc->mob) > 0) {
				health = round(1000.0 * GET_HEALTH(npc->mob) / GET_MAX_HEALTH(npc->mob));
			}
			else if (npc->soldier_recovery && npc->soldier_recovery <= time(0)) {
				health = 1000;
			}
			build_page_display(ch, "  %-20s %3d%% health, %s", name, health / 10, military_soldier_status(npc));
		}
	}
}


ACMD(do_military) {
	char command[MAX_INPUT_LENGTH], value[MAX_INPUT_LENGTH];
	struct empire_territory_data *ter, *next_ter;
	room_data *home;
	empire_data *emp = GET_LOYALTY(ch);
	int limit, count = 0;
	long requested;
	bool roster;

	if (IS_NPC(ch)) {
		return;
	}
	if (!emp) {
		msg_to_char(ch, "You must belong to an empire to manage its military.\r\n");
		return;
	}
	argument = any_one_arg(argument, command);
	skip_spaces(&argument);
	home = HOME_ROOM(IN_ROOM(ch));
	if (is_abbrev(command, "limit") && *command) {
		one_argument(argument, value);
		if (!is_garrison(home) || ROOM_OWNER(home) != emp) {
			msg_to_char(ch, "You must be in one of your empire's barracks or mountain garrisons.\r\n");
		}
		else if (!has_permission(ch, PRIV_WORKFORCE, home)) {
			msg_to_char(ch, "You don't have permission to manage the military here.\r\n");
		}
		else if (!*value || (str_cmp(value, "off") && str_cmp(value, "max") && (!isdigit(*value) || !is_number(value)))) {
			msg_to_char(ch, "Usage: military limit <off | max | number>\r\n");
		}
		else {
			requested = !str_cmp(value, "off") ? 0 : (!str_cmp(value, "max") ? -1 : strtol(value, NULL, 10));
			if (requested < -1 || requested > get_garrison_capacity(home)) {
				msg_to_char(ch, "This garrison can house up to %d soldiers.\r\n", get_garrison_capacity(home));
				return;
			}
			limit = requested;
			set_room_extra_data(home, ROOM_EXTRA_GARRISON_LIMIT, limit);
			reset_garrison_recruitment(home);
			reconcile_garrison(home);
			military_refresh();
			EMPIRE_NEEDS_SAVE(emp) = TRUE;
			request_world_save(GET_ROOM_VNUM(home), WSAVE_ROOM);
			log_to_empire(emp, ELOG_MILITARY, "%s set the garrison%s recruitment target to %s", PERS(ch, ch, TRUE), coord_display_room(ch, home, FALSE), value);
			msg_to_char(ch, "Recruitment target set to %s. Excess soldiers will return to civilian life when safe.\r\n", value);
		}
		return;
	}
	roster = *command && is_abbrev(command, "roster");
	if ((*command && is_abbrev(command, "here")) || (roster && (!*argument || is_abbrev(argument, "here")))) {
		if (!is_garrison(home) || ROOM_OWNER(home) != emp) {
			msg_to_char(ch, "There is no garrison belonging to your empire here.\r\n");
			return;
		}
		show_military_garrison(ch, home, TRUE);
	}
	else if (roster) {
		home = parse_room_from_coords(argument);
		home = home ? HOME_ROOM(home) : NULL;
		if (!home || !is_garrison(home) || ROOM_OWNER(home) != emp) {
			msg_to_char(ch, "There is no garrison belonging to your empire at those coordinates.\r\n");
			return;
		}
		show_military_garrison(ch, home, TRUE);
	}
	else if (*command) {
		msg_to_char(ch, "Usage: military [here | roster [here | coordinates] | limit <off | max | number>]\r\n");
		return;
	}
	else {
		build_page_display(ch, "Military score: %d. Soldiers: %d enlisted, %d active.", EMPIRE_MILITARY(emp), get_empire_soldiers(emp, FALSE), get_empire_soldiers(emp, TRUE));
		HASH_ITER(hh, EMPIRE_TERRITORY_LIST(emp), ter, next_ter) {
			if (HOME_ROOM(ter->room) == ter->room && is_garrison(ter->room) && ROOM_OWNER(ter->room) == emp) {
				show_military_garrison(ch, ter->room, FALSE);
				++count;
			}
		}
		if (!count) {
			build_page_display(ch, "Your empire has no completed barracks or mountain garrisons.");
		}
	}
	send_page_display(ch);
}
