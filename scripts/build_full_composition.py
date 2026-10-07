import json
import os
import subprocess
import sys

def main():
    print("=== Step 1: Re-creating fresh song.json ===")
    song_dir = "compositions/uplifting-trance-138"
    song_json = os.path.join(song_dir, "song.json")
    os.makedirs(song_dir, exist_ok=True)

    cmd_create = [
        "./build/release/nod.exe", "song", "create", song_json,
        "--bpm", "138", "--meter", "4/4", "--bars", "224", "--ppq", "480", "--json"
    ]
    res = subprocess.run(cmd_create, capture_output=True, text=True)
    if res.returncode != 0:
        print("Failed to create song:", res.stderr)
        sys.exit(1)
    print("Song created successfully.")

    print("=== Step 2: Generating complete composition batch ===")
    PPQ = 480
    BAR_TICKS = 1920
    TOTAL_BARS = 224
    TOTAL_TICKS = TOTAL_BARS * BAR_TICKS

    commands = []

    # 1. TRACKS CREATION & INSTRUMENT ASSIGNMENT
    tracks_def = [
      ('kick', 'Kick', 'kick', 'patch', 'patches/kick-trance.json', 0.88, 0.0),
      ('bass-roll', 'Rolling Bass', 'bass', 'preset', 'production-offbeat-bass', 0.82, 0.0),
      ('bass-sub', 'Sub Bass', 'sub', 'preset', 'production-sub', 0.72, 0.0),
      ('hat-open', 'Open Hi-Hat', 'open-hat', 'preset', 'open-hat', 0.42, 0.15),
      ('hat-closed', 'Closed Hi-Hat', 'closed-hat', 'preset', 'closed-hat', 0.35, -0.15),
      ('clap', 'Clap', 'clap', 'patch', 'patches/clap-trance.json', 0.58, 0.0),
      ('snare-roll', 'Snare Roll', 'snare', 'patch', 'patches/clap-trance.json', 0.65, 0.0),
      ('percussion', 'Percussion', 'percussion', 'preset', 'closed-hat', 0.40, -0.25),
      ('lead-supersaw', 'Supersaw Lead', 'lead', 'preset', 'production-lead', 0.90, 0.0),
      ('lead-pluck', 'Pluck Arp', 'pluck', 'preset', 'production-pluck', 0.45, 0.20),
      ('pad-lush', 'Lush Pad', 'pad', 'patch', 'patches/pad-lush.json', 0.55, 0.0),
      ('piano', 'Grand Piano', 'piano', 'patch', 'patches/piano.json', 0.70, -0.10),
      ('strings', 'Cinematic Strings', 'strings', 'patch', 'patches/strings.json', 0.60, 0.10),
      ('choir-layer', 'Choir Harmony', 'choir', 'patch', 'patches/vocal-choir.json', 0.50, 0.0),
      ('vocal-lead', 'Lead Vocal', 'lead', 'patch', 'patches/vocal-lead.json', 0.75, 0.0),
      ('vocal-chop', 'Vocal Chop', 'lead', 'patch', 'patches/vocal-chop.json', 0.48, 0.15),
      ('fx-riser', 'FX Riser', 'riser', 'patch', 'patches/riser-noise.json', 0.40, 0.0),
      ('fx-impact', 'Crash Impact', 'impact', 'preset', 'production-impact', 0.50, 0.0)
    ]

    for tid, name, role, kind, target, gain, pan in tracks_def:
        commands.append({'op': 'create-track', 'id': tid, 'name': name})
        commands.append({'op': 'set-role', 'track': tid, 'role': role})
        if kind == 'patch':
            commands.append({'op': 'set-instrument', 'track': tid, 'patch': target, 'pathBase': 'song-directory'})
        else:
            commands.append({'op': 'bind-preset', 'track': tid, 'preset': target})
        commands.append({'op': 'set-gain', 'track': tid, 'gain': gain})
        commands.append({'op': 'set-pan', 'track': tid, 'pan': pan})
        commands.append({'op': 'set-mix-mode', 'track': tid, 'gainMode': 'multiply', 'panMode': 'balance'})

    # 2. BUSES & SENDS & MASTER INSERTS
    commands.append({
      'op': 'create-bus', 'id': 'bus-reverb', 'name': 'Reverb Return', 'return': True,
      'inserts': [{'id': 'rev', 'type': 'reverb', 'parameters': {'decay': 3.8, 'damping': 6000, 'lowCut': 200, 'highCut': 14000, 'wet': 1.0}}]
    })
    commands.append({
      'op': 'create-bus', 'id': 'bus-delay', 'name': 'Delay Return', 'return': True,
      'inserts': [{'id': 'del', 'type': 'delay', 'parameters': {'timeMs': 435, 'syncBeats': 0.75, 'pingPong': 1, 'feedback': 0.45, 'lowCut': 200, 'highCut': 10000, 'wet': 1.0}}]
    })

    sends_map = {
      'vocal-lead': [{'target': 'bus-reverb', 'gain': 0.32, 'position': 'post-fader'}, {'target': 'bus-delay', 'gain': 0.25, 'position': 'post-fader'}],
      'vocal-chop': [{'target': 'bus-reverb', 'gain': 0.28, 'position': 'post-fader'}, {'target': 'bus-delay', 'gain': 0.42, 'position': 'post-fader'}],
      'piano': [{'target': 'bus-reverb', 'gain': 0.24, 'position': 'post-fader'}],
      'strings': [{'target': 'bus-reverb', 'gain': 0.30, 'position': 'post-fader'}],
      'choir-layer': [{'target': 'bus-reverb', 'gain': 0.35, 'position': 'post-fader'}],
      'lead-supersaw': [{'target': 'bus-reverb', 'gain': 0.20, 'position': 'post-fader'}, {'target': 'bus-delay', 'gain': 0.15, 'position': 'post-fader'}],
      'lead-pluck': [{'target': 'bus-reverb', 'gain': 0.18, 'position': 'post-fader'}, {'target': 'bus-delay', 'gain': 0.25, 'position': 'post-fader'}],
      'pad-lush': [{'target': 'bus-reverb', 'gain': 0.25, 'position': 'post-fader'}]
    }
    for tid, sends in sends_map.items():
        commands.append({'op': 'set-sends', 'target': tid, 'sends': sends})

    commands.append({
      'op': 'set-inserts', 'target': 'master',
      'inserts': [
        {'id': 'm-eq', 'type': 'eq', 'parameters': {'mode': 1, 'frequency': 24, 'gainDb': 0, 'q': 0.707, 'wet': 1.0}},
        {'id': 'm-sat', 'type': 'saturation', 'quality': 'high', 'parameters': {'driveDb': 1.5, 'outputDb': -0.5, 'wet': 1.0}},
        {'id': 'm-lim', 'type': 'limiter', 'quality': 'high', 'parameters': {'ceilingDb': -0.5, 'lookaheadMs': 5.0, 'releaseMs': 80.0}}
      ]
    })

    # 3. SECTIONS
    sections = [
      ('intro', 'Intro (0:00 - 1:23)', 0, 92160),
      ('verse', 'Verse (1:23 - 2:18)', 92160, 153600),
      ('bridge', 'Bridge Breakdown (2:18 - 3:28)', 153600, 230400),
      ('build', 'Build (3:28 - 4:10)', 230400, 276480),
      ('chorus', 'Chorus Drop (4:10 - 5:20)', 276480, 353280),
      ('outro', 'Outro (5:20 - 6:30)', 353280, 430080)
    ]
    for sid, sname, stick, etick in sections:
        commands.append({'op': 'set-section', 'id': sid, 'name': sname, 'startTick': stick, 'endTick': etick})

    # 4. PUMPS
    commands.append({
        "op": "add-pump",
        "track": "bass-roll",
        "startBar": 1,
        "endBar": 225,
        "period": "1/4",
        "recovery": "1/8",
        "depth": 0.82,
        "skipIntervals": [{"startTick": 48 * BAR_TICKS, "endTick": 144 * BAR_TICKS}]
    })

    commands.append({
        "op": "add-pump",
        "track": "lead-supersaw",
        "startBar": 145,
        "endBar": 185,
        "period": "1/4",
        "recovery": "1/8",
        "depth": 0.55
    })

    commands.append({
        "op": "add-pump",
        "track": "pad-lush",
        "startBar": 145,
        "endBar": 185,
        "period": "1/4",
        "recovery": "1/8",
        "depth": 0.50
    })

    # 5. AUDIO MUTES for Bar 144 (1-bar dead stop silence)
    dead_stop = [{"startTick": 143 * BAR_TICKS, "endTick": 144 * BAR_TICKS}]
    for t in ["kick", "bass-roll", "lead-supersaw", "snare-roll", "fx-riser", "pad-lush"]:
        commands.append({
            "op": "set-audio-mute",
            "track": t,
            "intervals": dead_stop,
            "fadeMs": 1.0
        })

    # 6. AUTOMATION
    # bass-roll macro:tone opening in Intro, closing in Outro
    commands.append({
        "op": "set-parameter-automation",
        "track": "bass-roll",
        "parameter": "macro:tone",
        "valueDomain": "normalized",
        "interpolation": "linear",
        "points": [
            {"tick": 0, "value": 0.15},
            {"tick": 32 * BAR_TICKS, "value": 0.55},
            {"tick": 48 * BAR_TICKS, "value": 0.85},
            {"tick": 184 * BAR_TICKS, "value": 0.85},
            {"tick": 208 * BAR_TICKS, "value": 0.50},
            {"tick": 224 * BAR_TICKS, "value": 0.08}
        ]
    })

    # lead-supersaw macro:tone opening in Build, fading in Outro
    commands.append({
        "op": "set-parameter-automation",
        "track": "lead-supersaw",
        "parameter": "macro:tone",
        "valueDomain": "normalized",
        "interpolation": "linear",
        "points": [
            {"tick": 128 * BAR_TICKS, "value": 0.12},
            {"tick": 143 * BAR_TICKS, "value": 0.98},
            {"tick": 144 * BAR_TICKS, "value": 0.98},
            {"tick": 184 * BAR_TICKS, "value": 0.95},
            {"tick": 200 * BAR_TICKS, "value": 0.05}
        ]
    })

    # lead-supersaw volume fade in Outro
    commands.append({
        "op": "set-gain-automation",
        "track": "lead-supersaw",
        "points": [
            {"tick": 0, "gain": 0.90},
            {"tick": 184 * BAR_TICKS, "gain": 0.90},
            {"tick": 200 * BAR_TICKS, "gain": 0.0}
        ]
    })

    # 7. HARMONIC ENGINE & NOTE GENERATION
    def get_chord_info(bar):
        b16 = ((bar - 1) % 16) + 1
        if 113 <= bar <= 116:
            if bar <= 114:
                return (35, [47, 54, 59, 63], [35, 47], [59, 63, 66, 71]) # B
            else:
                return (37, [49, 53, 56, 61], [37, 49], [61, 65, 68, 73]) # C#
        if 117 <= bar <= 120:
            if bar <= 118:
                return (39, [51, 54, 58, 63], [39, 51], [63, 66, 70, 75]) # D#m
            else:
                return (34, [46, 54, 58, 66], [34, 46], [58, 66, 70, 78]) # F#/A#
        if 137 <= bar <= 144:
            return (39, [51, 54, 58, 63], [39, 51], [63, 66, 70, 75])
        if 177 <= bar <= 184:
            sub = bar - 177
            if sub < 2: return (35, [47, 54, 59, 63], [35, 47], [59, 63, 66, 71])
            elif sub < 4: return (37, [49, 53, 56, 61], [37, 49], [61, 65, 68, 73])
            elif sub < 6: return (39, [51, 54, 58, 63], [39, 51], [63, 66, 70, 75])
            else: return (42, [54, 58, 61, 66], [30, 42], [61, 66, 70, 78])

        if b16 in [1, 2, 9, 10]:
            return (39, [51, 54, 58, 63], [39, 51], [63, 66, 70, 75])
        elif b16 in [3, 4, 11, 12]:
            return (35, [47, 54, 59, 63], [35, 47], [59, 63, 66, 71])
        elif b16 in [5, 6, 13, 14]:
            return (30, [42, 54, 58, 61], [30, 42], [58, 61, 66, 70])
        elif b16 in [7, 8]:
            return (37, [49, 53, 56, 61], [37, 49], [56, 61, 65, 68])
        else:
            return (37, [46, 53, 58, 61], [37, 46], [58, 61, 65, 70])

    track_notes = {tid: [] for tid, _, _, _, _, _, _ in tracks_def}
    nid = [0]
    def make_note(tick, dur, pitch, vel):
        nid[0] += 1
        return {"id": f"n{nid[0]}", "tick": int(tick), "duration": int(dur), "pitch": int(pitch), "velocity": int(vel), "channel": 0}

    # KICK
    for bar in range(1, TOTAL_BARS + 1):
        if (1 <= bar <= 48) or (145 <= bar <= 184) or (185 <= bar <= 224):
            bar_start = (bar - 1) * BAR_TICKS
            for beat, vel in [(0, 118), (480, 104), (960, 112), (1440, 104)]:
                track_notes['kick'].append(make_note(bar_start + beat, 220, 36, vel))

    # BASS-ROLL (16th-note rolling bass)
    for bar in range(1, TOTAL_BARS + 1):
        if (1 <= bar <= 48) or (145 <= bar <= 184) or (185 <= bar <= 224):
            bar_start = (bar - 1) * BAR_TICKS
            root_midi, _, _, _ = get_chord_info(bar)
            low_root = root_midi
            high_root = root_midi + 12
            for beat in range(4):
                b_off = beat * 480
                track_notes['bass-roll'].append(make_note(bar_start + b_off, 85, low_root, 88))
                track_notes['bass-roll'].append(make_note(bar_start + b_off + 120, 85, high_root, 102))
                track_notes['bass-roll'].append(make_note(bar_start + b_off + 240, 85, high_root, 112))
                track_notes['bass-roll'].append(make_note(bar_start + b_off + 360, 85, high_root, 100))

    # BASS-SUB
    for bar in range(1, TOTAL_BARS + 1):
        bar_start = (bar - 1) * BAR_TICKS
        root_midi, _, _, _ = get_chord_info(bar)
        sub_root = root_midi - 12 if root_midi >= 36 else root_midi
        if 1 <= bar <= 48:
            if (bar - 1) % 2 == 0:
                track_notes['bass-sub'].append(make_note(bar_start, BAR_TICKS * 2 - 40, sub_root, 80))
        elif 49 <= bar <= 80:
            for step in range(8):
                track_notes['bass-sub'].append(make_note(bar_start + step * 240, 160, sub_root, 95))
        elif 121 <= bar <= 143:
            if (bar - 1) % 2 == 0:
                track_notes['bass-sub'].append(make_note(bar_start, BAR_TICKS * 2 - 40, sub_root, 85))
        elif 145 <= bar <= 184:
            for beat in range(4):
                track_notes['bass-sub'].append(make_note(bar_start + beat * 480, 420, sub_root, 98))
        elif 185 <= bar <= 224:
            if (bar - 1) % 2 == 0:
                track_notes['bass-sub'].append(make_note(bar_start, BAR_TICKS * 2 - 40, sub_root, 82))

    # HAT-OPEN (Off-beat open hats)
    for bar in range(1, TOTAL_BARS + 1):
        if (33 <= bar <= 48) or (145 <= bar <= 184) or (185 <= bar <= 208):
            bar_start = (bar - 1) * BAR_TICKS
            for off in [240, 720, 1200, 1680]:
                track_notes['hat-open'].append(make_note(bar_start + off, 200, 46, 106))

    # HAT-CLOSED (Driving 16th shaker loops)
    for bar in range(1, TOTAL_BARS + 1):
        if (33 <= bar <= 48) or (145 <= bar <= 184) or (185 <= bar <= 216):
            bar_start = (bar - 1) * BAR_TICKS
            for step in range(16):
                vel = 96 if (step % 2 == 1) else 68
                track_notes['hat-closed'].append(make_note(bar_start + step * 120, 70, 42, vel))

    # CLAP (Sharp claps on beats 2 and 4)
    for bar in range(1, TOTAL_BARS + 1):
        if (33 <= bar <= 48) or (145 <= bar <= 184) or (185 <= bar <= 208):
            bar_start = (bar - 1) * BAR_TICKS
            track_notes['clap'].append(make_note(bar_start + 480, 240, 39, 112))
            track_notes['clap'].append(make_note(bar_start + 1440, 240, 39, 114))

    # SNARE-ROLL (Transitions & build)
    # Bar 32 fill
    b32_start = 31 * BAR_TICKS
    for i in range(8):
        track_notes['snare-roll'].append(make_note(b32_start + 960 + i * 120, 90, 38, 70 + i * 5))
    # Bar 48 roll
    b48_start = 47 * BAR_TICKS
    for i in range(16):
        track_notes['snare-roll'].append(make_note(b48_start + i * 120, 90, 38, 65 + i * 3))
    # Build 121 - 143
    for bar in range(121, 144):
        bar_start = (bar - 1) * BAR_TICKS
        if 121 <= bar <= 128:
            for beat in range(4):
                vel = 70 + (bar - 121) * 2 + beat
                track_notes['snare-roll'].append(make_note(bar_start + beat * 480, 200, 38, vel))
        elif 129 <= bar <= 136:
            for step in range(8):
                vel = 84 + (bar - 129) * 2 + step
                track_notes['snare-roll'].append(make_note(bar_start + step * 240, 140, 38, vel))
        elif 137 <= bar <= 140:
            for step in range(16):
                vel = 100 + (bar - 137) * 3 + (step // 2)
                track_notes['snare-roll'].append(make_note(bar_start + step * 120, 80, 38, min(120, vel)))
        elif 141 <= bar <= 143:
            for step in range(32):
                vel = 112 + (bar - 141) * 4 + (step // 4)
                track_notes['snare-roll'].append(make_note(bar_start + step * 60, 45, 38, min(127, vel)))

    # PERCUSSION
    for bar in range(1, TOTAL_BARS + 1):
        if (33 <= bar <= 48) or (161 <= bar <= 184):
            bar_start = (bar - 1) * BAR_TICKS
            for off, pitch in [(360, 49), (840, 49), (1320, 49), (1800, 49)]:
                track_notes['percussion'].append(make_note(bar_start + off, 100, pitch, 88))

    # PAD-LUSH
    for bar in range(1, TOTAL_BARS + 1):
        if (1 <= bar <= 120) or (145 <= bar <= 184):
            if (bar - 1) % 2 == 0:
                bar_start = (bar - 1) * BAR_TICKS
                _, pad_notes, _, _ = get_chord_info(bar)
                for p in pad_notes:
                    track_notes['pad-lush'].append(make_note(bar_start, BAR_TICKS * 2 - 40, p, 78))

    # LEAD-PLUCK
    for bar in range(1, TOTAL_BARS + 1):
        if (49 <= bar <= 80) or (81 <= bar <= 88) or (161 <= bar <= 184):
            bar_start = (bar - 1) * BAR_TICKS
            _, pad_notes, _, _ = get_chord_info(bar)
            arp_pool = [pad_notes[0], pad_notes[1], pad_notes[2], pad_notes[3],
                        pad_notes[1] + 12, pad_notes[2] + 12, pad_notes[3] + 12, pad_notes[2] + 12]
            for step in range(16):
                p = arp_pool[step % len(arp_pool)]
                track_notes['lead-pluck'].append(make_note(bar_start + step * 120, 95, p + 12, 85))

    # PIANO (Breakdown)
    for bar in range(81, 121):
        bar_start = (bar - 1) * BAR_TICKS
        _, _, lh_notes, rh_notes = get_chord_info(bar)
        for p in lh_notes:
            track_notes['piano'].append(make_note(bar_start, BAR_TICKS - 60, p, 84))
        for p in rh_notes:
            track_notes['piano'].append(make_note(bar_start, 460, p, 86))
        for p in rh_notes[:2]:
            track_notes['piano'].append(make_note(bar_start + 720, 440, p + 12, 80))
        for p in rh_notes:
            track_notes['piano'].append(make_note(bar_start + 960, 460, p, 84))
        track_notes['piano'].append(make_note(bar_start + 1440, 220, rh_notes[-1] + 2, 88))
        track_notes['piano'].append(make_note(bar_start + 1680, 220, rh_notes[-1], 84))

    # STRINGS
    for bar in range(81, 137):
        if (bar - 1) % 2 == 0:
            bar_start = (bar - 1) * BAR_TICKS
            _, pad_notes, lh_notes, _ = get_chord_info(bar)
            for p in (lh_notes + pad_notes):
                track_notes['strings'].append(make_note(bar_start, BAR_TICKS * 2 - 40, p, 82))

    # CHOIR-LAYER (Breakdown at 2:50!)
    for bar in range(97, 121):
        if (bar - 1) % 2 == 0:
            bar_start = (bar - 1) * BAR_TICKS
            _, pad_notes, _, _ = get_chord_info(bar)
            for p in pad_notes:
                track_notes['choir-layer'].append(make_note(bar_start, BAR_TICKS * 2 - 40, p + 12, 85))

    # VOCAL-LEAD
    verse_1 = [
        (49, 0, 720, 70), (49, 960, 720, 68), (50, 0, 960, 66),
        (51, 0, 720, 63), (51, 960, 720, 66), (52, 0, 1440, 70), (53, 0, 1440, 73)
    ]
    verse_2 = [
        (57, 0, 720, 71), (57, 960, 720, 70), (58, 0, 960, 68),
        (59, 0, 720, 66), (59, 960, 720, 68), (60, 0, 1440, 70)
    ]
    verse_3 = [
        (65, 0, 720, 70), (65, 960, 720, 73), (66, 0, 960, 75),
        (67, 0, 720, 73), (67, 960, 720, 70), (68, 0, 1440, 66)
    ]
    verse_4 = [
        (73, 0, 720, 68), (73, 960, 720, 70), (74, 0, 960, 71),
        (75, 0, 720, 70), (75, 960, 720, 68), (76, 0, 1440, 66)
    ]
    for bar_num, beat_off, dur, pitch in (verse_1 + verse_2 + verse_3 + verse_4):
        track_notes['vocal-lead'].append(make_note((bar_num - 1) * BAR_TICKS + beat_off, dur, pitch, 95))

    bd_1 = [
        (97, 0, 720, 75), (97, 960, 720, 73), (98, 0, 960, 71),
        (99, 0, 720, 70), (99, 960, 720, 71), (100, 0, 1440, 73)
    ]
    bd_2 = [
        (103, 0, 720, 73), (103, 960, 720, 71), (104, 0, 960, 70),
        (105, 0, 720, 68), (105, 960, 720, 70), (106, 0, 1440, 66)
    ]
    bd_3 = [
        (109, 0, 720, 66), (109, 960, 720, 68), (110, 0, 960, 70),
        (111, 0, 720, 73), (111, 960, 720, 75), (112, 0, 1440, 77)
    ]
    bd_4 = [
        (115, 0, 960, 78), (116, 0, 960, 77), (117, 0, 960, 75),
        (118, 0, 960, 73), (119, 0, 2800, 75)
    ]
    for bar_num, beat_off, dur, pitch in (bd_1 + bd_2 + bd_3 + bd_4):
        track_notes['vocal-lead'].append(make_note((bar_num - 1) * BAR_TICKS + beat_off, dur, pitch, 102))

    # Bar 144 dead-stop silence final vocal hook echo
    track_notes['vocal-lead'].append(make_note(143 * BAR_TICKS, 480, 75, 105))

    # Chorus Drop Vocal Belts
    ch_1 = [
        (145, 0, 720, 78), (145, 960, 720, 80), (146, 0, 1440, 82),
        (147, 0, 720, 85), (147, 960, 720, 82), (148, 0, 1440, 80),
        (149, 0, 720, 78), (149, 960, 720, 80), (150, 0, 2400, 82)
    ]
    ch_2 = [
        (161, 0, 720, 83), (161, 960, 720, 82), (162, 0, 1440, 80),
        (163, 0, 720, 78), (163, 960, 720, 80), (164, 0, 1440, 82),
        (165, 0, 720, 85), (165, 960, 720, 87), (166, 0, 2400, 85)
    ]
    for bar_num, beat_off, dur, pitch in (ch_1 + ch_2):
        track_notes['vocal-lead'].append(make_note((bar_num - 1) * BAR_TICKS + beat_off, dur, pitch, 108))

    # VOCAL-CHOP
    for bar in range(33, 49):
        bar_start = (bar - 1) * BAR_TICKS
        _, pad_notes, _, _ = get_chord_info(bar)
        p1 = pad_notes[1] + 12
        p2 = pad_notes[2] + 12
        track_notes['vocal-chop'].append(make_note(bar_start + 240, 120, p1, 92))
        track_notes['vocal-chop'].append(make_note(bar_start + 720, 120, p2, 95))
        track_notes['vocal-chop'].append(make_note(bar_start + 1200, 120, p1, 90))
        track_notes['vocal-chop'].append(make_note(bar_start + 1560, 120, p2 + 2, 98))

    # LEAD-SUPERSAW
    theme_pattern = [
        [78, 75, 78, 80, 82, 85, 82, 80, 78, 80, 82, 85, 87, 85, 82, 80],
        [83, 82, 80, 78, 75, 78, 80, 83, 87, 85, 83, 82, 80, 78, 80, 82],
        [82, 85, 87, 89, 90, 89, 87, 85, 82, 85, 87, 85, 82, 80, 78, 80],
        [80, 78, 75, 73, 75, 78, 80, 82, 83, 82, 80, 78, 75, 73, 75, 78]
    ]
    for bar in range(1, TOTAL_BARS + 1):
        if (129 <= bar <= 143) or (145 <= bar <= 184) or (185 <= bar <= 200):
            bar_start = (bar - 1) * BAR_TICKS
            p_bar = theme_pattern[(bar - 1) % 4]
            vel_base = 85 if (129 <= bar <= 143) else (105 if (145 <= bar <= 184) else 75)
            for step in range(16):
                track_notes['lead-supersaw'].append(make_note(bar_start + step * 120, 100, p_bar[step], vel_base + (step % 2) * 5))

    # FX-RISER
    for b in range(45, 49):
        track_notes['fx-riser'].append(make_note((b - 1) * BAR_TICKS, BAR_TICKS, 60 + (b - 45) * 4, 70 + (b - 45) * 10))
    for b in range(129, 144):
        track_notes['fx-riser'].append(make_note((b - 1) * BAR_TICKS, BAR_TICKS, 55 + (b - 129) * 2, 70 + (b - 129) * 3))

    # FX-IMPACT
    for b in [1, 33, 49, 81, 145, 161, 177, 185]:
        track_notes['fx-impact'].append(make_note((b - 1) * BAR_TICKS, 3800, 36, 115))

    # 8. CLIPS AND ADD-NOTES
    total_notes_count = 0
    for tid, notes in track_notes.items():
        total_notes_count += len(notes)
        commands.append({
            "op": "create-clip",
            "track": tid,
            "id": f"{tid}-clip",
            "startTick": 0,
            "length": TOTAL_TICKS
        })
        if notes:
            commands.append({
                "op": "add-notes",
                "track": tid,
                "clip": f"{tid}-clip",
                "notes": notes
            })

    print(f"Total notes generated across 18 tracks: {total_notes_count}")
    print(f"Total commands in batch: {len(commands)}")

    batch = {
        "schemaVersion": 1,
        "requestId": "trance-masterpiece-full-v1",
        "commands": commands
    }

    batch_path = os.path.join(song_dir, "complete-composition.json")
    with open(batch_path, "w") as f:
        json.dump(batch, f, indent=2)
    print(f"Wrote batch to {batch_path} ({os.path.getsize(batch_path)} bytes)")

    print("=== Step 3: Applying complete batch to song.json ===")
    cmd_apply = [
        "./build/release/nod.exe", "song", "apply", song_json,
        "--commands", batch_path,
        "--expect-revision", "1",
        "--json"
    ]
    res_apply = subprocess.run(cmd_apply, capture_output=True, text=True)
    if res_apply.returncode != 0:
        print("Failed to apply batch:", res_apply.stderr or res_apply.stdout)
        sys.exit(1)
    print("Batch applied successfully!")

    print("=== Step 4: Validating song render readiness ===")
    cmd_val = [
        "./build/release/nod.exe", "song", "validate", song_json,
        "--render-ready", "--json"
    ]
    res_val = subprocess.run(cmd_val, capture_output=True, text=True)
    val_data = json.loads(res_val.stdout)
    diagnostics = val_data.get("diagnostics", [])
    print(f"Validation status: {val_data.get('status')}, diagnostics count: {len(diagnostics)}")
    if diagnostics:
        print("Diagnostics:", json.dumps(diagnostics, indent=2))
        sys.exit(1)
    else:
        print("Validation PASSED with ZERO diagnostics! Song is 100% render-ready!")

if __name__ == "__main__":
    main()
