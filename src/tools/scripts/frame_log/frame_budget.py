#!/usr/bin/env python3
"""Break one frame-log capture down against the 60 fps budget.

Where compare_flog.py tells two captures apart, this reads one and answers which
frames miss vsync and what they were doing. Takes a raw PCSX2 emulog or a .flog
written by summarize_flog.py --rows.

A frame's EE work is Frame - VSync. The vsync spin sits in the middle of the Frame
scope (the present is deferred to the next GL_BeginRendering), so Frame alone reads one
field plus however much the work after the spin grew since the last frame; EE work is the
stable measure. A frame whose work passes one field (16683 us) waits for the next one, and
Frame lands near 33.4 ms: that is a dropped frame.

Host_ServerFrame and CL_ReadFromServer run before GL_BeginRendering rolls the profiler
over, so the Server, ClParse and ClScene columns land one row early (see
src/ps2/debug/engine_profile.h). They are shifted back here before anything is added up.
ClParticles, SndMix and Music run after the rollover and need no shift. Sound is the
feeder thread's time, spread across the other columns, so it is never subtracted.

The map cycle's perf pass (ps2_testmaps 2) writes a FLOG#view marker before each
viewpoint it turns around at, and the rows after it are that viewpoint's. The first 15
of them (the teleport there, and what it uploads) count as settling, as a map's first
30 do. Its "0,end" marker ends a level's tour: the rows from there to the next map
(the next level's load starts in one that still draws this one) are never steady.

Prints, in order:
  - dropped frames, frames over budget and frames with no margin left, for steady frames
    and for the settling ones (each map's first 30 frames, each viewpoint's first 15) apart
  - EE work percentiles and a 1 ms histogram
  - the same per map, with its slowest viewpoint
  - for a perf pass, the 20 slowest viewpoints, with what their frames drew
  - every column's mean over the over-budget frames against all steady frames
  - runs of consecutive over-budget frames, and the 15 worst frames broken down
  - every FLOG#open note charged to a steady frame or the one before it (a mid-level load):
    the file, the ClParse and FsIo of the row it was charged to, and the EE work of that row
    and the next, which is what the load cost. The notes from loads and settling frames
    are only counted, unless --all-opens lists them too.

Usage: frame_budget.py <emulog.txt|capture.flog> [--all-opens]
"""
import sys, statistics, collections

# summarize_flog lives beside this; keep the import from leaving a __pycache__
# in src/tools/scripts/frame_log.
sys.dont_write_bytecode = True
from summarize_flog import extract

FIELD = 16683

# Rows after a marker that count as settling: a map's loading work, and a viewpoint's teleport.
MAP_SETTLE = 30
VIEW_SETTLE = 15

def load(path):
    hdr, lines, map_lines, view_lines, open_lines, _ = extract(path)
    if hdr is None:
        sys.exit(f"{path}: no FLOG#hdr - not a frame-log capture")
    cols = hdr.split(',')[1:]
    rows = []
    for line in lines:
        v = line.split(',')[1:]
        if len(v) == len(cols):
            rows.append({c: int(x) for c, x in zip(cols, v)})
    maps = []
    for line in map_lines:
        _, f, name = line.split(',', 2)
        maps.append((int(f), name))
    # FLOG#view,<row>,<n>,<kind>,<x y z>
    views = []
    for line in view_lines:
        _, f, n, kind, origin = line.split(',', 4)
        views.append((int(f), int(n), kind, origin))
    opens = []
    for line in open_lines:
        _, f, name = line.split(',', 2)
        opens.append((int(f), name))
    return rows, maps, views, opens

def label_rows(rows, maps, views):
    """Gives each row its map, and the viewpoint it looks from (None outside a perf pass's
    tour): a marker's row and the ones after it are its, until the next marker. Rows after
    a tour's end marker are flagged 'leaving'."""
    marks = sorted([(f, 0, ('map', n)) for f, n in maps] +
                   [(f, 1, ('view', (n, kind, origin))) for f, n, kind, origin in views])
    i, cur_map, cur_view, leaving = 0, '?', None, False
    for r in sorted(rows, key=lambda r: r['frame']):
        while i < len(marks) and marks[i][0] <= r['frame']:
            what, value = marks[i][2]
            if what == 'map':
                cur_map, cur_view, leaving = value, None, False
            else:
                leaving = value[0] == 0
                cur_view = None if leaving else value
            i += 1
        r['map'] = cur_map
        r['viewpoint'] = (cur_map,) + cur_view if cur_view else None
        r['leaving'] = leaving

def view_tag(r):
    return f"#{r['viewpoint'][1]}" if r.get('viewpoint') else ''

def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q))] if v else 0

def report_budget(rows, maps, views):
    """Prints the budget report and returns the steady frames' row numbers."""
    # Shift the early columns back to the row whose Frame holds their time, so a row's
    # columns add up to its own Frame.
    prev = {}
    for r in rows:
        for c in ('Server', 'ClParse', 'ClScene'):
            if c in r:
                r[c], prev[c] = prev.get(c, 0), r[c]
    for r in rows:
        r['ee'] = r['Frame'] - r['VSync']
        # Music (BGM_Update) runs just before S_Update, outside SndMix.
        eng = sum(r.get(c, 0) for c in ('Server', 'ClParse', 'ClScene', 'ClParticles'))
        snd = r.get('SndMix', 0) + r.get('Music', 0)
        r['rest'] = r['ee'] - r['View'] - r['Ui'] - r['Overlay'] - snd - eng

    view = [r for r in rows if r['View'] > 0]
    noview = len(rows) - len(view)
    # The first frames of a map are loading work (precache, first-touch VRAM uploads,
    # lightmap builds), not steady-state rendering - reported apart. So is everything
    # before the first map: the boot, and the console it draws. A perf pass's teleport to
    # each viewpoint is the same kind of work, in a smaller dose.
    settle_ids = {r['frame'] for r in view if (maps and r['frame'] < maps[0][0]) or r['leaving']}
    frames = [r['frame'] for r in view]
    starts = [f for f, n, _, _ in views if n > 0]
    for marks, n in (([f for f, _ in maps], MAP_SETTLE), (starts, VIEW_SETTLE)):
        for f in marks:
            settle_ids.update([x for x in frames if x >= f][:n])
    steady = [r for r in view if r['frame'] not in settle_ids]
    settle = [r for r in view if r['frame'] in settle_ids]

    def classify(rs):
        dropped = [r for r in rs if r['Frame'] > FIELD * 1.5]
        over = [r for r in rs if r['ee'] > FIELD and r['Frame'] <= FIELD * 1.5]
        tight = [r for r in rs if 15000 < r['ee'] <= FIELD and r['Frame'] <= FIELD * 1.5]
        return dropped, over, tight

    print(f"rows {len(rows)}  with a 3D view {len(view)}  without (console/loading) {noview}")
    tour = f", a viewpoint's first {VIEW_SETTLE}, a tour's way out" if views else ""
    print(f"  settling (a map's first {MAP_SETTLE} frames{tour}) {len(settle)}  steady {len(steady)}")
    for label, rs in (('steady', steady), ('settling', settle)):
        d, o, t = classify(rs)
        print(f"\n[{label}] {len(rs)} frames")
        print(f"  dropped (Frame > 25 ms):            {len(d):5d}  {100*len(d)/max(1,len(rs)):5.2f}%")
        print(f"  EE work > 16.68 ms, not dropped:    {len(o):5d}  {100*len(o)/max(1,len(rs)):5.2f}%")
        print(f"  EE work 15.0-16.68 ms (no margin):  {len(t):5d}  {100*len(t)/max(1,len(rs)):5.2f}%")
        ee = [r['ee'] for r in rs]
        if ee:
            print(f"  EE work  mean {statistics.mean(ee):7.0f}  p50 {pct(ee,.5):6d}  p90 {pct(ee,.9):6d}"
                  f"  p95 {pct(ee,.95):6d}  p99 {pct(ee,.99):6d}  max {max(ee):6d}")

    if not steady:
        return {r['frame'] for r in steady}

    # Histogram of EE work, steady frames.
    print("\nEE work histogram, steady frames (1 ms buckets):")
    h = collections.Counter(min(r['ee'] // 1000, 40) for r in steady)
    for k in sorted(h):
        bar = '#' * max(1, h[k] * 60 // max(h.values()))
        print(f"  {k:2d}{'+' if k == 40 else ' '}ms {h[k]:5d} {bar}")

    # Per viewpoint, for a perf pass: steady frames grouped by the viewpoint they look from.
    by_view = collections.defaultdict(list)
    for r in steady:
        if r['viewpoint']:
            by_view[r['viewpoint']].append(r)
    view_p95 = {k: pct([r['ee'] for r in rs], .95) for k, rs in by_view.items()}

    # Per map. A map the log enters twice (a demo replayed) is listed once.
    print("\nPer map (steady frames):")
    print(f"  {'map':<10}{'frames':>7}{'dropped':>9}{'ee>16.7':>9}{'ee>15':>7}{'ee p50':>8}{'ee p95':>8}"
          f"{'ee p99':>8}{'ee max':>8}{'  slowest viewpoint (ee p95)' if by_view else ''}")
    for name in dict.fromkeys(n for _, n in maps):
        rs = [r for r in steady if r['map'] == name]
        if not rs:
            continue
        d, o, t = classify(rs)
        ee = [r['ee'] for r in rs]
        worst = max((k for k in by_view if k[0] == name), key=lambda k: view_p95[k], default=None)
        slowest = f"  #{worst[1]} {worst[2]} ({view_p95[worst]})" if worst else ''
        print(f"  {name:<10}{len(rs):7d}{len(d):9d}{len(o):9d}{len(t):7d}{pct(ee,.5):8d}{pct(ee,.95):8d}"
              f"{pct(ee,.99):8d}{max(ee):8d}{slowest}")

    if by_view:
        over_views = [k for k, rs in by_view.items() if any(r['ee'] > FIELD or r['Frame'] > FIELD * 1.5 for r in rs)]
        print(f"\nViewpoints: {len(by_view)}, {len(over_views)} with a steady frame over budget."
              f" The 20 slowest by EE p95, with their frames' means:")
        print(f"  {'map':<8}{'#':>3} {'kind':<13}{'origin':<20}{'frames':>7}{'ee>16.7':>8}{'ee>15':>6}"
              f"{'ee p50':>8}{'ee p95':>8}{'ee max':>8}{'View':>7}{'World':>7}{'Ent':>6}{'Sky':>6}{'tris':>6}")
        for k in sorted(by_view, key=lambda k: view_p95[k], reverse=True)[:20]:
            rs = by_view[k]
            d, o, t = classify(rs)
            ee = [r['ee'] for r in rs]
            mean = lambda c: statistics.mean(r[c] for r in rs)
            print(f"  {k[0]:<8}{k[1]:>3} {k[2]:<13}{k[3]:<20}{len(rs):7d}{len(d) + len(o):8d}{len(t):6d}"
                  f"{pct(ee,.5):8d}{view_p95[k]:8d}{max(ee):8d}{mean('View'):7.0f}{mean('World'):7.0f}"
                  f"{mean('Entities'):6.0f}{mean('Sky'):6.0f}{mean('tris'):6.0f}")

    # Where the time goes: over-budget steady frames vs the average steady frame.
    d, o, t = classify(steady)
    bad = d + o
    cols = ['ee', 'View', 'World', 'Vis', 'TexChains', 'LmChains', 'Entities', 'EntCull', 'EntShade',
            'EntGeom', 'EntShadow', 'EntBrush', 'Particles', 'TurbSurfs', 'Sky', 'Ui', 'Overlay',
            'Server', 'ClParse', 'ClScene', 'ClParticles', 'SndMix', 'Sound', 'Music', 'FsIo',
            'GsWait', 'DmaSend', 'DmaFlush', 'rest',
            'tris', 'batches', 'particles', 'vramUploads', 'vramOomSyncs', 'vramResident',
            'chainKB', 'chainKicks', 'chainDrains']
    print(f"\nOver-budget steady frames ({len(bad)}) vs all steady frames, means:")
    print(f"  {'column':<13}{'all':>9}{'over':>9}{'delta':>9}")
    for c in [c for c in cols if c in steady[0]]:
        a = statistics.mean(r[c] for r in steady)
        b = statistics.mean(r[c] for r in bad) if bad else 0
        print(f"  {c:<13}{a:9.0f}{b:9.0f}{b-a:+9.0f}")

    # Streaks: consecutive over-budget frames, so one-off spikes and sustained heavy scenes
    # can be told apart.
    badset = {r['frame'] for r in bad}
    streaks, cur = [], []
    for r in steady:
        if r['frame'] in badset and (not cur or r['frame'] == cur[-1]['frame'] + 1):
            cur.append(r)
        else:
            if cur: streaks.append(cur)
            cur = [r] if r['frame'] in badset else []
    if cur: streaks.append(cur)
    lens = collections.Counter(min(len(s), 10) for s in streaks)
    print(f"\nStreaks of over-budget frames: {len(streaks)}")
    for k in sorted(lens):
        print(f"  length {k}{'+' if k == 10 else ' '}: {lens[k]}")
    print("\nLongest streaks:")
    for s in sorted(streaks, key=len, reverse=True)[:12]:
        ee = [r['ee'] for r in s]
        print(f"  {s[0]['map']:<8} frames {s[0]['frame']:5d}-{s[-1]['frame']:5d} ({len(s):3d})"
              f"  ee mean {statistics.mean(ee):6.0f} max {max(ee):6d}"
              f"  View {statistics.mean(r['View'] for r in s):6.0f}"
              f"  Ent {statistics.mean(r['Entities'] for r in s):5.0f}"
              f"  World {statistics.mean(r['World'] for r in s):5.0f}"
              f"  Mix {statistics.mean(r.get('SndMix', 0) for r in s):5.0f}"
              f"  Sv {statistics.mean(r.get('Server', 0) for r in s):5.0f}"
              f"  Parse {statistics.mean(r.get('ClParse', 0) for r in s):5.0f}"
              f"  Scene {statistics.mean(r.get('ClScene', 0) for r in s):5.0f}"
              f"  PartSim {statistics.mean(r.get('ClParticles', 0) for r in s):5.0f}"
              f"  rest {statistics.mean(r['rest'] for r in s):5.0f}"
              f"  tris {statistics.mean(r['tris'] for r in s):5.0f}")

    print("\nWorst 15 steady frames by EE work (map, viewpoint, row):")
    for r in sorted(steady, key=lambda r: r['ee'], reverse=True)[:15]:
        print(f"  {r['map']:<8}{view_tag(r):<4} row {r['frame']:5d} Frame {r['Frame']:6d} ee {r['ee']:6d} View {r['View']:5d}"
              f" World {r['World']:5d} Ent {r['Entities']:5d} Part {r['Particles']:4d} Ui {r['Ui']:4d}"
              f" Mix {r.get('SndMix', 0):5d} Sv {r.get('Server', 0):5d} Parse {r.get('ClParse', 0):5d}"
              f" Scene {r.get('ClScene', 0):5d} PartSim {r.get('ClParticles', 0):5d} Fs {r.get('FsIo', 0):5d}"
              f" rest {r['rest']:5d} particles {r['particles']} vramUp {r['vramUploads']} oom {r['vramOomSyncs']}")
    return {r['frame'] for r in steady}

def report_opens(rows, opens, steady_ids, all_opens):
    # Takes the rows as logged, not shifted: FLOG#open names the row charged with the read,
    # and a load in CL_ReadFromServer/Host_ServerFrame is charged to it with the ClParse and
    # FsIo it caused, while the time itself stretches the Frame of the row after. So show both,
    # and count a note as mid-level when either of them is a steady frame.
    if not opens:
        return
    by = {r['frame']: r for r in rows}
    mid = [(f, n) for f, n in opens if f in steady_ids or f + 1 in steady_ids]
    listed = opens if all_opens else mid
    print(f"\nFiles opened: {len(opens)} notes, {len(mid)} of them mid-level"
          f"{'' if all_opens else ' (the rest are loads and settling frames; --all-opens lists them)'}.")
    if listed:
        print("The charged row's ClParse and FsIo, and the EE work of it and the row after:")
    for f, name in listed:
        a, b = by.get(f), by.get(f + 1)
        ee = lambda r: f"{r['Frame'] - r['VSync']:6d}" if r else "     -"
        col = lambda r, c: f"{r.get(c, 0):6d}" if r else "     -"
        print(f"  row {f:5d}  parse {col(a, 'ClParse')}  fs {col(a, 'FsIo')}  ee {ee(a)} / {ee(b)} us  {name}")

def main():
    args = [a for a in sys.argv[1:] if a != '--all-opens']
    if len(args) != 1:
        sys.exit(__doc__.rstrip())
    rows, maps, views, opens = load(args[0])
    label_rows(rows, maps, views)
    logged = [dict(r) for r in rows] # report_budget shifts columns in place
    steady_ids = report_budget(rows, maps, views)
    report_opens(logged, opens, steady_ids, '--all-opens' in sys.argv[1:])

if __name__ == '__main__':
    main()
