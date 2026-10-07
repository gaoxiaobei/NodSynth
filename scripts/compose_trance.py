import json
import os
import subprocess

def build_song():
    print("Generating complete Uplifting Trance 138 BPM composition...")

    PPQ = 480
    BAR_TICKS = 1920
    TOTAL_BARS = 224
    TOTAL_TICKS = TOTAL_BARS * BAR_TICKS

    commands = []

    # 1. PUMPS
    # bass-roll pump across entire song, skipping Verse & Breakdown (Bars 49-144)
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

    # lead-supersaw pump during Chorus (Bars 145-184)
    commands.append({
        "op": "add-pump",
        "track": "lead-supersaw",
        "startBar": 145,
        "endBar": 185,
        "period": "1/4",
        "recovery": "1/8",
        "depth": 0.55
    })

    # pad-lush pump during Chorus (Bars 145-184)
    commands.append({
        "op": "add-pump",
        "track": "pad-lush",
        "startBar": 145,
        "endBar": 185,
        "period": "1/4",
        "recovery": "1/8",
        "depth": 0.50
    })

    # 2. AUDIO MUTES for Bar 144 (1-bar dead-stop silence)
    dead_stop = [{"startTick": 143 * BAR_TICKS, "endTick": 144 * BAR_TICKS}]
    for t in ["kick", "bass-roll", "lead-supersaw", "snare-roll", "fx-riser", "pad-lush"]:
        commands.append({
            "op": "set-audio-mute",
            "track": t,
            "intervals": dead_stop,
            "fadeMs": 1.0
        })

    # 3. AUTOMATION
    # bass-roll filter opening in Intro, closing in Outro
    commands.append({
        "op": "set-parameter-automation",
        "track": "bass-roll",
        "parameter": "macro:brightness",
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

    # lead-supersaw filter opening in Build, fading in Outro
    commands.append({
        "op": "set-parameter-automation",
        "track": "lead-supersaw",
        "parameter": "macro:brightness",
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

    # 4. CHORD ENGINE
    # 16-bar harmonic cycle in F# Major / D# Minor:
    # 1-2: D#m, 3-4: B, 5-6: F#, 7-8: C#, 9-10: D#m, 11-12: B, 13-14: F#, 15-16: A#m/C#
    def get_chord_info(bar):
        # returns (root_midi, pad_notes, piano_lh, piano_rh)
        b16 = ((bar - 1) % 16) + 1

        # Specific section overrides for emotional peaks:
        if 113 <= bar <= 116: # Bridge emotional climax transition: B -> C#
            if bar <= 114:
                return (35, [47, 54, 59, 63], [35, 47], [59, 63, 66, 71]) # B
            else:
                return (37, [49, 53, 56, 61], [37, 49], [61, 65, 68, 73]) # C#
        if 117 <= bar <= 120: # D#m -> F#/A#
            if bar <= 118:
                return (39, [51, 54, 58, 63], [39, 51], [63, 66, 70, 75]) # D#m
            else:
                return (34, [46, 54, 58, 66], [34, 46], [58, 66, 70, 78]) # F#/A#
        if 137 <= bar <= 144: # Build climax pedal point on D#m
            return (39, [51, 54, 58, 63], [39, 51], [63, 66, 70, 75])
        if 177 <= bar <= 184: # Drop climax turnaround: B -> C# -> D#m -> F#
            sub = bar - 177
            if sub < 2: return (35, [47, 54, 59, 63], [35, 47], [59, 63, 66, 71])
            elif sub < 4: return (37, [49, 53, 56, 61], [37, 49], [61, 65, 68, 73])
            elif sub < 6: return (39, [51, 54, 58, 63], [39, 51], [63, 66, 70, 75])
            else: return (42, [54, 58, 61, 66], [30, 42], [61, 66, 70, 78])

        # Standard 16-bar cycle:
        if b16 in [1, 2, 9, 10]:
            # D#m: D#2=39, Notes: D#3=51, F#3=54, A#3=58, D#4=63
            return (39, [51, 54, 58, 63], [39, 51], [63, 66, 70, 75])
        elif b16 in [3, 4, 11, 12]:
            # B: B1=35, Notes: B2=47, F#3=54, B3=59, D#4=63
            return (35, [47, 54, 59, 63], [35, 47], [59, 63, 66, 71])
        elif b16 in [5, 6, 13, 14]:
            # F#: F#1=30, Notes: F#2=42, F#3=54, A#3=58, C#4=61
            return (30, [42, 54, 58, 61], [30, 42], [58, 61, 66, 70])
        elif b16 in [7, 8]:
            # C#: C#2=37, Notes: C#3=49, F3=53, G#3=56, C#4=61
            return (37, [49, 53, 56, 61], [37, 49], [56, 61, 65, 68])
        else: # 15, 16
            # A#m/C#: C#2=37, Notes: A#2=46, F3=53, A#3=58, C#4=61
            return (37, [46, 53, 58, 61], [37, 46], [58, 61, 65, 70])

    # 5. NOTE GENERATORS FOR EACH TRACK
    track_notes = {tid: [] for tid in [
        'kick', 'bass-roll', 'bass-sub', 'hat-open', 'hat-closed', 'clap', 'snare-roll',
        'percussion', 'lead-supersaw', 'lead-pluck', 'pad-lush', 'piano', 'strings',
        'choir-layer', 'vocal-lead', 'vocal-chop', 'fx-riser', 'fx-impact'
    ]}

    nid = [0]
    def make_note(tick, dur, pitch, vel):
        nid[0] += 1
        return {"id": f"n{nid[0]}", "tick": int(tick), "duration": int(dur), "pitch": int(pitch), "velocity": int(vel), "channel": 0}

    # ----------------------------------------------------
    # KICK
    # ----------------------------------------------------
    for bar in range(1, TOTAL_BARS + 1):
        if (1 <= bar <= 48) or (145 <= bar <= 184) or (185 <= bar <= 224):
            bar_start = (bar - 1) * BAR_TICKS
            for beat, vel in [(0, 118), (480, 104), (960, 112), (1440, 104)]:
                track_notes['kick'].append(make_note(bar_start + beat, 220, 36, vel))

    # ----------------------------------------------------
    # BASS-ROLL (Relentless 16th-note rolling bass)
    # ----------------------------------------------------
    for bar in range(1, TOTAL_BARS + 1):
        if (1 <= bar <= 48) or (145 <= bar <= 184) or (185 <= bar <= 224):
            bar_start = (bar - 1) * BAR_TICKS
            root_midi, _, _, _ = get_chord_info(bar)
            low_root = root_midi
            high_root = root_midi + 12
            # 16 notes per bar (4 per beat)
            for beat in range(4):
                b_off = beat * 480
                track_notes['bass-roll'].append(make_note(bar_start + b_off, 85, low_root, 88))
                track_notes['bass-roll'].append(make_note(bar_start + b_off + 120, 85, high_root, 102))
                track_notes['bass-roll'].append(make_note(bar_start + b_off + 240, 85, high_root, 112))
                track_notes['bass-roll'].append(make_note(bar_start + b_off + 360, 85, high_root, 100))

    # ----------------------------------------------------
    # BASS-SUB
    # ----------------------------------------------------
    for bar in range(1, TOTAL_BARS + 1):
        bar_start = (bar - 1) * BAR_TICKS
        root_midi, _, _, _ = get_chord_info(bar)
        sub_root = root_midi - 12 if root_midi >= 36 else root_midi

        if 1 <= bar <= 48: # Intro sustained sub
            if (bar - 1) % 2 == 0:
                track_notes['bass-sub'].append(make_note(bar_start, BAR_TICKS * 2 - 40, sub_root, 80))
        elif 49 <= bar <= 80: # Verse pulsing 8th-note sub bass
            for step in range(8):
                track_notes['bass-sub'].append(make_note(bar_start + step * 240, 160, sub_root, 95))
        elif 121 <= bar <= 143: # Build sub drone
            if (bar - 1) % 2 == 0:
                track_notes['bass-sub'].append(make_note(bar_start, BAR_TICKS * 2 - 40, sub_root, 85))
        elif 145 <= bar <= 184: # Chorus drop sub foundation
            for beat in range(4):
                track_notes['bass-sub'].append(make_note(bar_start + beat * 480, 420, sub_root, 98))
        elif 185 <= bar <= 224: # Outro sub rumble
            if (bar - 1) % 2 == 0:
                track_notes['bass-sub'].append(make_note(bar_start, BAR_TICKS * 2 - 40, sub_root, 82))

    # ----------------------------------------------------
    # HAT-OPEN (Prominent off-beat open hat)
    # ----------------------------------------------------
    for bar in range(1, TOTAL_BARS + 1):
        if (33 <= bar <= 48) or (145 <= bar <= 184) or (185 <= bar <= 208):
            bar_start = (bar - 1) * BAR_TICKS
            for off in [240, 720, 1200, 1680]:
                track_notes['hat-open'].append(make_note(bar_start + off, 200, 46, 106))

    # ----------------------------------------------------
    # HAT-CLOSED (Driving 16th shaker loops)
    # ----------------------------------------------------
    for bar in range(1, TOTAL_BARS + 1):
        if (33 <= bar <= 48) or (145 <= bar <= 184) or (185 <= bar <= 216):
            bar_start = (bar - 1) * BAR_TICKS
            for step in range(16):
                vel = 96 if (step % 2 == 1) else 68
                track_notes['hat-closed'].append(make_note(bar_start + step * 120, 70, 42, vel))

    # ----------------------------------------------------
    # CLAP (Sharp claps on beats 2 and 4)
    # ----------------------------------------------------
    for bar in range(1, TOTAL_BARS + 1):
        if (33 <= bar <= 48) or (145 <= bar <= 184) or (185 <= bar <= 208):
            bar_start = (bar - 1) * BAR_TICKS
            track_notes['clap'].append(make_note(bar_start + 480, 240, 39, 112))
            track_notes['clap'].append(make_note(bar_start + 1440, 240, 39, 114))

    # ----------------------------------------------------
    # SNARE-ROLL (Transitions and escalating build)
    # ----------------------------------------------------
    # Bar 32 transition fill
    b32_start = 31 * BAR_TICKS
    for i in range(8):
        track_notes['snare-roll'].append(make_note(b32_start + 960 + i * 120, 90, 38, 70 + i * 5))

    # Bar 48 transition roll into Verse
    b48_start = 47 * BAR_TICKS
    for i in range(16):
        track_notes['snare-roll'].append(make_note(b48_start + i * 120, 90, 38, 65 + i * 3))

    # Build section: Bars 121 - 143
    for bar in range(121, 144):
        bar_start = (bar - 1) * BAR_TICKS
        if 121 <= bar <= 128: # Quarter notes
            for beat in range(4):
                vel = 70 + (bar - 121) * 2 + beat
                track_notes['snare-roll'].append(make_note(bar_start + beat * 480, 200, 38, vel))
        elif 129 <= bar <= 136: # 8th notes
            for step in range(8):
                vel = 84 + (bar - 129) * 2 + step
                track_notes['snare-roll'].append(make_note(bar_start + step * 240, 140, 38, vel))
        elif 137 <= bar <= 140: # 16th notes
            for step in range(16):
                vel = 100 + (bar - 137) * 3 + (step // 2)
                track_notes['snare-roll'].append(make_note(bar_start + step * 120, 80, 38, min(120, vel)))
        elif 141 <= bar <= 143: # 32nd notes
            for step in range(32):
                vel = 112 + (bar - 141) * 4 + (step // 4)
                track_notes['snare-roll'].append(make_note(bar_start + step * 60, 45, 38, min(127, vel)))

    # ----------------------------------------------------
    # PERCUSSION
    # ----------------------------------------------------
    for bar in range(1, TOTAL_BARS + 1):
        if (33 <= bar <= 48) or (161 <= bar <= 184):
            bar_start = (bar - 1) * BAR_TICKS
            for off, pitch in [(360, 49), (840, 49), (1320, 49), (1800, 49)]:
                track_notes['percussion'].append(make_note(bar_start + off, 100, pitch, 88))

    # ----------------------------------------------------
    # PAD-LUSH (Lush atmospheric pads)
    # ----------------------------------------------------
    for bar in range(1, TOTAL_BARS + 1):
        if (1 <= bar <= 120) or (145 <= bar <= 184):
            if (bar - 1) % 2 == 0:
                bar_start = (bar - 1) * BAR_TICKS
                _, pad_notes, _, _ = get_chord_info(bar)
                for p in pad_notes:
                    track_notes['pad-lush'].append(make_note(bar_start, BAR_TICKS * 2 - 40, p, 78))

    # ----------------------------------------------------
    # LEAD-PLUCK (Delicate 16th-note pluck arpeggio)
    # ----------------------------------------------------
    for bar in range(1, TOTAL_BARS + 1):
        if (49 <= bar <= 80) or (81 <= bar <= 88) or (161 <= bar <= 184):
            bar_start = (bar - 1) * BAR_TICKS
            _, pad_notes, _, _ = get_chord_info(bar)
            # 16-note arpeggio cycling through chord notes + upper extensions
            arp_pool = [pad_notes[0], pad_notes[1], pad_notes[2], pad_notes[3],
                        pad_notes[1] + 12, pad_notes[2] + 12, pad_notes[3] + 12, pad_notes[2] + 12]
            for step in range(16):
                p = arp_pool[step % len(arp_pool)]
                track_notes['lead-pluck'].append(make_note(bar_start + step * 120, 95, p + 12, 85))

    # ----------------------------------------------------
    # PIANO (Grand Piano in Breakdown)
    # ----------------------------------------------------
    for bar in range(81, 121):
        bar_start = (bar - 1) * BAR_TICKS
        _, _, lh_notes, rh_notes = get_chord_info(bar)

        # Left hand: sustained octave on downbeat
        for p in lh_notes:
            track_notes['piano'].append(make_note(bar_start, BAR_TICKS - 60, p, 84))

        # Right hand: rhythmic broken chords and delicate melodic motifs
        # Downbeat chord
        for p in rh_notes:
            track_notes['piano'].append(make_note(bar_start, 460, p, 86))
        # Beat 2 syncopation
        for p in rh_notes[:2]:
            track_notes['piano'].append(make_note(bar_start + 720, 440, p + 12, 80))
        # Beat 3 chord
        for p in rh_notes:
            track_notes['piano'].append(make_note(bar_start + 960, 460, p, 84))
        # Beat 4 melodic ornament
        track_notes['piano'].append(make_note(bar_start + 1440, 220, rh_notes[-1] + 2, 88))
        track_notes['piano'].append(make_note(bar_start + 1680, 220, rh_notes[-1], 84))

    # ----------------------------------------------------
    # STRINGS (Cinematic Strings in Breakdown & Build)
    # ----------------------------------------------------
    for bar in range(81, 137):
        if (bar - 1) % 2 == 0:
            bar_start = (bar - 1) * BAR_TICKS
            _, pad_notes, lh_notes, _ = get_chord_info(bar)
            str_notes = lh_notes + pad_notes
            for p in str_notes:
                track_notes['strings'].append(make_note(bar_start, BAR_TICKS * 2 - 40, p, 82))

    # ----------------------------------------------------
    # CHOIR-LAYER (Lush unison choral layers at 2:50!)
    # ----------------------------------------------------
    for bar in range(97, 121):
        if (bar - 1) % 2 == 0:
            bar_start = (bar - 1) * BAR_TICKS
            _, pad_notes, _, _ = get_chord_info(bar)
            for p in pad_notes:
                track_notes['choir-layer'].append(make_note(bar_start, BAR_TICKS * 2 - 40, p + 12, 85))

    # ----------------------------------------------------
    # VOCAL-LEAD (Melody & Lyrics)
    # ----------------------------------------------------
    # Verse (Bars 49 - 80)
    # Fading shadows in the night (49-56)
    verse_1 = [
        (49, 0, 720, 70), (49, 960, 720, 68), (50, 0, 960, 66),
        (51, 0, 720, 63), (51, 960, 720, 66), (52, 0, 1440, 70), (53, 0, 1440, 73)
    ]
    # Searching for a spark of light (57-64)
    verse_2 = [
        (57, 0, 720, 71), (57, 960, 720, 70), (58, 0, 960, 68),
        (59, 0, 720, 66), (59, 960, 720, 68), (60, 0, 1440, 70)
    ]
    # Every heartbeat leads me through (65-72)
    verse_3 = [
        (65, 0, 720, 70), (65, 960, 720, 73), (66, 0, 960, 75),
        (67, 0, 720, 73), (67, 960, 720, 70), (68, 0, 1440, 66)
    ]
    # Walking into skies of blue (73-80)
    verse_4 = [
        (73, 0, 720, 68), (73, 960, 720, 70), (74, 0, 960, 71),
        (75, 0, 720, 70), (75, 960, 720, 68), (76, 0, 1440, 66)
    ]
    for bar_num, beat_off, dur, pitch in (verse_1 + verse_2 + verse_3 + verse_4):
        track_notes['vocal-lead'].append(make_note((bar_num - 1) * BAR_TICKS + beat_off, dur, pitch, 95))

    # Breakdown (Bars 97 - 120)
    # Hold on when the stars collide (97-102)
    bd_1 = [
        (97, 0, 720, 75), (97, 960, 720, 73), (98, 0, 960, 71),
        (99, 0, 720, 70), (99, 960, 720, 71), (100, 0, 1440, 73)
    ]
    # Leave the sorrow far behind (103-108)
    bd_2 = [
        (103, 0, 720, 73), (103, 960, 720, 71), (104, 0, 960, 70),
        (105, 0, 720, 68), (105, 960, 720, 70), (106, 0, 1440, 66)
    ]
    # Can you feel the heavens rise? (109-114)
    bd_3 = [
        (109, 0, 720, 66), (109, 960, 720, 68), (110, 0, 960, 70),
        (111, 0, 720, 73), (111, 960, 720, 75), (112, 0, 1440, 77)
    ]
    # We are infinite tonight (115-120)
    bd_4 = [
        (115, 0, 960, 78), (116, 0, 960, 77), (117, 0, 960, 75),
        (118, 0, 960, 73), (119, 0, 2800, 75)
    ]
    for bar_num, beat_off, dur, pitch in (bd_1 + bd_2 + bd_3 + bd_4):
        track_notes['vocal-lead'].append(make_note((bar_num - 1) * BAR_TICKS + beat_off, dur, pitch, 102))

    # Bar 144 dead-stop silence final vocal hook echo
    track_notes['vocal-lead'].append(make_note(143 * BAR_TICKS, 480, 75, 105)) # D#5 echoing into the void

    # Chorus / Drop Vocal Belts (Bars 145 - 176)
    # We are rising higher, through the endless sky! (145-160)
    ch_1 = [
        (145, 0, 720, 78), (145, 960, 720, 80), (146, 0, 1440, 82),
        (147, 0, 720, 85), (147, 960, 720, 82), (148, 0, 1440, 80),
        (149, 0, 720, 78), (149, 960, 720, 80), (150, 0, 2400, 82)
    ]
    # Catch the blinding fire, watch the world go by! (161-176)
    ch_2 = [
        (161, 0, 720, 83), (161, 960, 720, 82), (162, 0, 1440, 80),
        (163, 0, 720, 78), (163, 960, 720, 80), (164, 0, 1440, 82),
        (165, 0, 720, 85), (165, 960, 720, 87), (166, 0, 2400, 85)
    ]
    for bar_num, beat_off, dur, pitch in (ch_1 + ch_2):
        track_notes['vocal-lead'].append(make_note((bar_num - 1) * BAR_TICKS + beat_off, dur, pitch, 108))

    # ----------------------------------------------------
    # VOCAL-CHOP (Intro bars 33 - 48 with ping-pong delay)
    # ----------------------------------------------------
    for bar in range(33, 49):
        bar_start = (bar - 1) * BAR_TICKS
        _, pad_notes, _, _ = get_chord_info(bar)
        p1 = pad_notes[1] + 12
        p2 = pad_notes[2] + 12
        track_notes['vocal-chop'].append(make_note(bar_start + 240, 120, p1, 92))
        track_notes['vocal-chop'].append(make_note(bar_start + 720, 120, p2, 95))
        track_notes['vocal-chop'].append(make_note(bar_start + 1200, 120, p1, 90))
        track_notes['vocal-chop'].append(make_note(bar_start + 1560, 120, p2 + 2, 98))

    # ----------------------------------------------------
    # LEAD-SUPERSAW (Anthemic uplifting trance lead hook)
    # ----------------------------------------------------
    # Theme in F# Major: 16-bar hook
    theme_pattern = [
        # Bar 1: F#5 -> D#5 -> F#5 -> G#5 -> A#5 -> C#6 -> A#5 -> G#5
        [78, 75, 78, 80, 82, 85, 82, 80, 78, 80, 82, 85, 87, 85, 82, 80],
        # Bar 2: B5 -> A#5 -> G#5 -> F#5 -> D#5 -> F#5 -> G#5 -> B5
        [83, 82, 80, 78, 75, 78, 80, 83, 87, 85, 83, 82, 80, 78, 80, 82],
        # Bar 3: A#5 -> C#6 -> D#6 -> F6 -> F#6 -> F6 -> D#6 -> C#6
        [82, 85, 87, 89, 90, 89, 87, 85, 82, 85, 87, 85, 82, 80, 78, 80],
        # Bar 4: G#5 -> F#5 -> D#5 -> C#5 -> D#5 -> F#5 -> G#5 -> A#5
        [80, 78, 75, 73, 75, 78, 80, 82, 83, 82, 80, 78, 75, 73, 75, 78]
    ]

    # Plays in Build (129-143), Drop (145-184), Outro (185-200)
    for bar in range(1, TOTAL_BARS + 1):
        if (129 <= bar <= 143) or (145 <= bar <= 184) or (185 <= bar <= 200):
            bar_start = (bar - 1) * BAR_TICKS
            p_bar = theme_pattern[(bar - 1) % 4]
            vel_base = 85 if (129 <= bar <= 143) else (105 if (145 <= bar <= 184) else 75)
            for step in range(16):
                track_notes['lead-supersaw'].append(make_note(bar_start + step * 120, 100, p_bar[step], vel_base + (step % 2) * 5))

    # ----------------------------------------------------
    # FX-RISER
    # ----------------------------------------------------
    # Intro riser (45-48)
    for b in range(45, 49):
        track_notes['fx-riser'].append(make_note((b - 1) * BAR_TICKS, BAR_TICKS, 60 + (b - 45) * 4, 70 + (b - 45) * 10))

    # Build riser (129-143)
    for b in range(129, 144):
        track_notes['fx-riser'].append(make_note((b - 1) * BAR_TICKS, BAR_TICKS, 55 + (b - 129) * 2, 70 + (b - 129) * 3))

    # ----------------------------------------------------
    # FX-IMPACT (Crash cymbals on section downbeats)
    # ----------------------------------------------------
    for b in [1, 33, 49, 81, 145, 161, 177, 185]:
        track_notes['fx-impact'].append(make_note((b - 1) * BAR_TICKS, 3800, 36, 115))

    # 6. ASSEMBLE CLIPS AND ADD-NOTES
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
        "requestId": "trance-arrange-full-v1",
        "commands": commands
    }

    out_path = "compositions/uplifting-trance-138/arrange-batch.json"
    with open(out_path, "w") as f:
        json.dump(batch, f, indent=2)

    print(f"Wrote arrange batch to {out_path} ({os.path.getsize(out_path)} bytes)")

if __name__ == "__main__":
    build_song()
