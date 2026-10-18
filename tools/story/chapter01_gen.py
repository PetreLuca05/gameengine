"""Chapter 1 "10th Floor" (docs/CHAPTER_01.md): writes assets/story/chapters/01_tenth_floor.json.

Single player. Day 1 is an ordinary morning in flat 10A until the emergency broadcast; then seven days
inside: each evening the bed becomes usable, sleeping fades out, the night changes the world (the city's
day state, the weather, the power, one or two more of the dead in the corridor) and the next morning
starts beside the bed with the day's card. The morning of day 8 a helicopter lands on Orchard House's
roof across the street: down the corridor, the stairs, across the street and up its stairs to the roof.

Run: python tools/story/chapter01_gen.py   (then rebuild: assets are copied at build time)
Texts live in assets/story/scripts/01.story; volumes / points / interactables in data/levels/story_01.json
(tools/story/build_story01.py, site01.py); vista states in assets/story/vista/sequences.json.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "assets", "story", "chapters", "01_tenth_floor.json")

DAYS = 7                                    # days inside; the rescue comes the morning after the last
HALL = [0, 0, 1, 2, 4, 5, 7, 9, 12]         # zombies in the corridor from that morning on (index = day)
HALL_LEFT = 5                               # the rescue morning: the rest followed the helicopter's noise
STREET = [0, 0, 0, 2, 4, 6, 8, 10, 12]      # zombies wandering the street below (lean out to see them)
WEATHER = {2: "cloudy", 3: "light_rain", 4: "cloudy+fog", 5: "rain", 6: "overcast+fog", 7: "storm", 8: "little_cloudy"}
POWER_OFF_NIGHT = 4                         # the building loses power during this night
DAY_LEN = 330.0                             # seconds of play from morning (07:30) to 21:00
TIRED = "19:00"                             # the bed takes you from then on


def chapter():
    objectives, triggers, timeline, prompts = [], [], [], []
    checkpoints = ["start"] + ["day_%d" % d for d in range(2, DAYS + 2)] + ["street", "end"]

    # ------------------------------------------------------------ day 1: an ordinary morning
    objectives += [
        {"id": "wake", "text": "obj.wake", "complete_on": {"any": [{"timer": 6}, {"signal": "player_moved"}]}},
        {"id": "morning", "text": "obj.morning", "requires": "wake", "group": {"need": 2}, "children": [
            {"id": "coffee", "text": "obj.coffee", "optional": True, "complete_on": {"interact": "coffee_maker"}},
            {"id": "phone", "text": "obj.phone", "optional": True, "complete_on": {"interact": "phone_sofa"}},
            {"id": "tv", "text": "obj.tv", "optional": True, "complete_on": {"interact": "tv_living"}},
            {"id": "window", "text": "obj.window", "optional": True,
             "complete_on": {"any": [{"entity": "win_master", "on": "opened"}, {"entity": "win_teen", "on": "opened"},
                                     {"entity": "win_child", "on": "opened"}, {"interact": "lean"}]}},
        ]},
        {"id": "broadcast", "text": "obj.broadcast", "activate": "manual", "marker": "tv_living"},
        {"id": "lock_door", "text": "obj.lock_door", "activate": "manual", "marker": "door_lock_10a",
         "complete_on": {"interact": "door_lock_10a"}},
        {"id": "peek_hall", "text": "obj.peek_hall", "requires": "lock_door", "optional": True, "complete_on": {"interact": "peephole_10a"}},
        {"id": "look_out", "text": "obj.look_out", "requires": "lock_door", "optional": True,
         "complete_on": {"any": [{"interact": "lean"}, {"interact": "lean_over"}]}},
    ]
    timeline += [
        {"beat": "wake", "day": 1, "from": "07:40", "to": "07:45", "expected": 30},
        {"beat": "morning", "day": 1, "from": "07:45", "to": "10:30", "expected": 240},
        {"beat": "broadcast", "day": 1, "from": "10:30", "to": "10:40", "expected": 60},
        {"beat": "lock_door", "day": 1, "from": "10:40", "to": "11:00", "expected": 60},
    ]
    triggers += [
        {"id": "intro", "when": True, "do": [
            {"fade": "in", "time": 3.0},
            {"title": "title.day_1", "time": 4.5},
            {"audio": "phone_buzz", "at": "phone_sofa", "kind": "sfx"},
            {"sequence": "day_1"},
            {"sequence": "traffic_build"},
        ]},
        {"id": "broadcast_start", "arm": {"objective": "wake"}, "when": {"objective": "morning"}, "fallback": 240, "do": [
            {"objective": "broadcast", "set": "active"},
            {"audio": "emergency_tone", "at": "tv_living", "kind": "sfx", "volume": 1.0},
            {"wait": 2.0},
            {"audio": "vo.eas_broadcast", "at": "tv_living", "kind": "vo"},
            {"subtitle": "vo.eas_broadcast"},
            {"sequence": "traffic_stall"},
            {"audio": "sirens_swell", "kind": "ambient", "volume": 0.6},
            {"wait": 10.0},
            {"sequence": "heli_north_pass"},
            {"wait": 14.0},
            {"objective": "broadcast", "set": "done"},
            {"objective": "lock_door", "set": "active"},
            {"subtitle": "sub.lock_door"},
        ]},
        {"id": "door_locked", "when": {"objective": "lock_door"}, "do": [
            {"lock": "door_10a"},
            {"audio": "lock_click", "at": "door_10a", "kind": "sfx"},
            {"objective": "day_1", "set": "active"},
            {"weather": "little_cloudy", "blend": 300},
        ]},
    ]

    # ------------------------------------------------------------ days 1..7: stay inside, sleep
    for d in range(1, DAYS + 1):
        objectives.append({"id": "day_%d" % d, "text": "obj.day_%d" % d, "activate": "manual"})
        objectives.append({"id": "sleep_%d" % d, "text": "obj.sleep", "activate": "manual", "marker": "bed_master",
                           "complete_on": {"signal": "slept", "ever": False}})
        if d == 1:
            timeline.append({"beat": "day_1", "day": 1, "from": "11:00", "to": "21:00", "expected": 280})
        else:
            timeline.append({"beat": "day_%d" % d, "day": d, "from": "07:30", "to": "21:00", "expected": DAY_LEN})
        timeline.append({"beat": "sleep_%d" % d, "day": d, "from": TIRED, "to": "23:40", "expected": 300})
        triggers.append({"id": "tired_%d" % d, "arm": {"objective": "day_%d" % d, "state": "active"}, "when": {"time": TIRED}, "do": [
            {"objective": "sleep_%d" % d, "set": "active"},
            {"subtitle": "sub.tired_%d" % d if d in (1, 4, 7) else "sub.tired"},
        ]})
        n = d + 1
        night = [
            {"fade": "out", "time": 2.5},
            {"wait": 3.0},
            {"objective": "day_%d" % d, "set": "done"},
        ]
        # While asleep: the world gets worse.
        night.append({"sequence": "day_%d" % n})
        for k in range(HALL[d] + 1, HALL[n] + 1):
            night.append({"spawn": "hall_z%d" % k, "character": "zombie_walker", "wander": 1.6})
        for k in range(STREET[d] + 1, STREET[n] + 1):
            night.append({"spawn": "street_z%d" % k, "character": "zombie_walker"})
        if d == POWER_OFF_NIGHT:
            night += [{"power": "apt_10a", "state": "off"}, {"power": "building", "state": "off"}]
        if n in WEATHER:
            night.append({"weather": WEATHER[n], "blend": 0})
        if n == DAYS + 1:
            night += rescue_night()
        night += [
            {"time": "07:30"},
            {"teleport": "spawn_bed"},
            {"checkpoint": "day_%d" % n},
        ]
        if n <= DAYS:
            night.append({"objective": "day_%d" % n, "set": "active"})
        night += [
            {"wait": 1.5},
            {"fade": "in", "time": 3.0},
            {"title": "title.day_%d" % n, "time": 4.5},
        ]
        if n == DAYS + 1:
            night += rescue_morning()
        elif n in (3, 5, 7):
            night += [{"wait": 6.0}, {"subtitle": "sub.morning_%d" % n}]
        triggers.append({"id": "night_%d" % d, "when": {"objective": "sleep_%d" % d}, "do": night})

    # ------------------------------------------------------------ day 8: the rescue
    objectives.append({"id": "rescue", "text": "obj.rescue", "activate": "manual", "marker": "heli_pad", "children": [
        {"id": "leave_flat", "text": "obj.leave_flat", "complete_on": {"volume": "vol_corridor_10", "on": "enter"}},
        {"id": "stairs", "text": "obj.stairs", "requires": "leave_flat", "complete_on": {"volume": "vol_lobby_ground", "on": "enter"}},
        {"id": "street", "text": "obj.street", "requires": "stairs", "complete_on": {"volume": "vol_rb_lobby", "on": "enter"}},
        {"id": "roof", "text": "obj.roof", "requires": "street", "complete_on": {"volume": "vol_rb_roof", "on": "enter"}},
        {"id": "board", "text": "obj.board", "requires": "roof", "complete_on": {"volume": "vol_heli", "on": "enter"}},
    ]})
    timeline += [
        {"beat": "rescue", "day": DAYS + 1, "from": "07:30", "to": "08:40", "expected": 900},
    ]
    triggers += [
        # The dead wake up as the player comes near (idle until then: the door kept them out for a week).
        {"id": "chase_hall", "arm": {"objective": "rescue", "state": "active"},
         "when": {"any": [{"entity": "door_10a", "on": "opened"}, {"volume": "vol_corridor_10", "on": "enter"}]}, "do": [{"chase": "hall_"}]},
        {"id": "chase_stairs", "arm": {"objective": "rescue", "state": "active"}, "when": {"volume": "vol_stair_low", "on": "enter"}, "do": [{"chase": "stair_"}]},
        {"id": "chase_street", "arm": {"objective": "rescue", "state": "active"}, "when": {"volume": "vol_lobby_ground", "on": "enter"}, "do": [
            {"chase": "lobby_"}, {"chase": "street_"}, {"checkpoint": "street"},
        ]},
        {"id": "chase_orchard", "arm": {"objective": "rescue", "state": "active"}, "when": {"volume": "vol_rb_lobby", "on": "enter"}, "do": [{"chase": "rb_"}]},
        {"id": "lift_dead", "when": {"signal": "lift_dead"}, "do": [{"subtitle": "sub.lift_dead"}]},
        {"id": "chapter_end", "when": {"objective": "rescue"}, "do": [
            {"audio": "heli_overhead", "kind": "ambient", "volume": 1.0},
            {"subtitle": "sub.boarding"},
            {"wait": 2.0},
            {"fade": "out", "time": 2.5},
            {"wait": 2.5},
            {"title": "title.rescued", "time": 5.0},
            {"wait": 5.0},
            {"checkpoint": "end"},
            {"end_chapter": True},
        ]},
    ]

    prompts = [
        {"id": "look", "action": "look", "text": "prompt.look", "after": "wake"},
        {"id": "move", "action": "move", "text": "prompt.move", "after": "wake"},
        {"id": "interact", "action": "interact", "text": "prompt.interact", "after": "wake", "until": "morning"},
        {"id": "hold", "action": "hold_interact", "text": "prompt.hold", "after": "sleep_1", "until": "sleep_1"},
        {"id": "lean", "action": "interact", "text": "prompt.lean", "after": "lock_door", "until": "look_out"},
        {"id": "melee", "action": "attack", "text": "prompt.melee", "after": "rescue"},
        {"id": "sprint", "action": "sprint", "text": "prompt.sprint", "after": "leave_flat"},
    ]
    return {
        "_doc": "Chapter 1 (docs/CHAPTER_01.md). GENERATED by tools/story/chapter01_gen.py: edit that, not this file. Format: docs/STORY.md 'Data'. "
                "Names refer to data/levels/story_01.json; text ids to assets/story/scripts/01.story; sequences to assets/story/vista/sequences.json.",
        "id": "01_tenth_floor",
        "title": "10th Floor",
        "map": "story_01",
        "script": "assets/story/scripts/01.story",
        "players": 1,
        "zombie_sense": 9,
        "start": {"checkpoint": "start", "time": "07:40", "weather": "clear", "season": "spring", "inventory": []},
        "checkpoints": checkpoints,
        "roles": [{"slot": 0, "id": "you", "spawn": "spawn_sofa"}],
        "timeline": timeline,
        "objectives": objectives,
        "prompts": prompts,
        "triggers": triggers,
    }


def rescue_night():
    """Night 7: the stairwell, the lobby, the street and Orchard House fill up; the doors unlock."""
    acts = []
    for k in range(1, 4):
        acts.append({"spawn": "stair_z%d" % k, "character": "zombie_walker"})
    for k in range(1, 3):
        acts.append({"spawn": "lobby_z%d" % k, "character": "zombie_walker"})
    for k in range(1, 3):
        acts.append({"spawn": "rb_z%d" % k, "character": "zombie_walker"})
    acts += [{"unlock": "door_10a"}, {"unlock": "f10_fire_door"}]
    acts += [{"despawn": "hall_z%d" % k} for k in range(HALL_LEFT + 1, HALL[DAYS + 1] + 1)]   # gone after the helicopter's noise
    return acts


def rescue_morning():
    return [
        {"wait": 3.0},
        {"audio": "radio_static", "at": "radio_kitchen", "kind": "sfx"},
        {"audio": "vo.radio_rescue", "at": "radio_kitchen", "kind": "vo"},
        {"subtitle": "vo.radio_rescue"},
        {"sequence": "rescue_heli"},
        {"wait": 8.0},
        {"objective": "rescue", "set": "active"},
        {"subtitle": "sub.rescue_go"},
    ]


if __name__ == "__main__":
    ch = chapter()
    with open(OUT, "w", newline="") as f:
        json.dump(ch, f, indent=1)
        f.write("\n")
    print("01_tenth_floor: %d objectives, %d triggers, %d beats" % (
        sum(1 + len(o.get("children", [])) for o in ch["objectives"]), len(ch["triggers"]), len(ch["timeline"])))
