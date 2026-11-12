#!/usr/bin/env python3
"""Does the private hull layer light like the game? (diag.hull_layer, Step 1; docs/research-own-hull-layer-2026-11-08.md "Step 1 build")

Usage: python tools/hull_layer_compare.py <edvr_logs\\eyes\\eye_<stamp>> [--eye 1] [--tol 0.05] [--clear-depth 0] [--log <edvr_gfx_*.log>]
       python tools/hull_layer_compare.py <edvr_logs\\eyes\\eye_<stamp>> --lights
       python tools/hull_layer_compare.py <edvr_logs\\eyes\\eye_<stamp>> --refl [--eye 1]
       python tools/hull_layer_compare.py --self-test

Reads eye_<stamp>_HullHdr.bin (the private HDR after the eye's last private lighting draw), eye_<stamp>_GameHdrLit.bin
(the game's HDR after its last lighting draw, before the sky) and eye_<stamp>_HullDepth.bin (the private depth after
the neighbour filter); --eye 1 reads the _e1 files. Hull pixels are where the private depth is not the clear value
(reversed Z: 0). On them both images hold emissive plus all the deferred lighting, so they should agree, except where
the game's AO (none on the layer) or the hull decals (not re-issued) differ.

Prints the hull pixel count, the fraction within --tol of the game's luma, the median and p95 ratio private/game,
the black fraction (private dark where the game is lit), the best whole-pixel shift within 3 px, and the own-mask
count and filtered pixels from the dump's log line (--log, else the newest edvr_gfx_*.log beside the eyes folder that
names the stamp).
Verdict and exit code: CONFIRMED 0 (median ratio 0.95..1.35 and at least half the hull pixels within --tol),
REFUTED 3 (under 500 hull pixels, more than half black, or a shift that matches 0.2 better than none),
INCONCLUSIVE 1 (also when the game's HDR is itself mostly black: the comparison point is wrong, not the layer),
bad input 2.

A separate-mode dump (diag.hull_layer = separate, Step 2: "Step 2 build") also has eye_<stamp>_GameDepth.bin, the game's
main depth after the eye's G-buffer with the own ship discarded (and _GameGbuf0.bin, its RT0). The game's HDR then has
no hull to compare, so the tool measures the main pass instead, on the private hull pixels: the share where the main
depth is farther than the hull's (reversed Z: smaller; the terrain behind it filled in), the share still at the clear
value (holes: nothing drawn there, or open sky), the share equal to the hull's depth (the hull still in the main pass)
and the share nearer (the cockpit or a neighbour in front). Verdict: TAKEN OUT 0 (at most 5% equal), NOT TAKEN OUT 3.

Since dump v2 ("Dump v2") a separate-mode dump's frame is a reference frame: the main pass kept the own ship, the files are
eye_<stamp>_RefGbuf0.bin and _RefDepth.bin instead of GameGbuf0 and GameDepth, and the tool runs the lighting comparison
above, with RefDepth (the same frame's main depth, both eyes) in place of the eye run's Z as the mask.

--refl (the reflection dump, 2026-11-10: "Reflection dump") reads eye_<stamp>_Refl.json and, beside the files above,
_GameHdrRefl / _HullHdrRefl (the HDRs right after the game's reflection dispatches and the private re-issue), _GameStencil /
_HullStencil (the texture under the pass's t4; its 8-bit stencil plane) and _GameT11 / _HullT11. On the hull pixels the game
shows it prints the luma ratio private/game before and after the pass and of the pass's own addition, the stencil's equal
share and that of the pass's test (s & cb1[26].y) == cb1[26].y with the top mismatching (game, private) pairs, and the t11.w
difference. Verdict: MATCHES 0 (after the pass median 0.9..1.15), DIFFERS 3 (and names the input that differs: a surplus
before the pass, the stencil test, t11.w), INCONCLUSIVE 1 (those agree: the pass's other inputs), NOT STAGED 2 (Refl.json says
a side was not staged, and why), bad input 2.

--lights reads eye_<stamp>_HullLights.json instead (dump v2: the draws of the dump frame's lighting windows) and lists the
draws that write the eye's HDR, by eye and vertex shader: how many, how many the layer re-issued and how many were unknown
(an HDR writer in no known lighting or stage pair), and their instances. Exit 0, or 2 on bad input.
"""
import argparse
import glob
import os
import re
import struct
import sys

MIN_HULL = 500
SHIFT = 3
MIN_GAME_LIT = 0.05   # the game's HDR lit on less of the hull than this: the comparison point, not the layer
MAX_STILL_HULL = 0.05   # separate mode: the main depth equal to the hull's on more of its pixels than this, the hull is still there


def main_pass(prefix, eye=0, clear_depth=0.0):
    """Separate mode: the main depth against the private hull depth on the private hull pixels, as a dict."""
    import numpy as np
    import eye_inputs
    suffix = '_e1' if eye else ''
    paths = {k: '%s_%s%s.bin' % (prefix, k, suffix) for k in ('HullDepth', 'GameDepth')}
    for k, p in paths.items():
        if not os.path.exists(p):
            raise ValueError('missing %s: %s' % (k, p))
    _, hull = eye_inputs.read(paths['HullDepth'])
    _, main = eye_inputs.read(paths['GameDepth'])
    hull, main = np.asarray(hull, np.float32), np.asarray(main, np.float32)
    if hull.shape != main.shape:
        raise ValueError('HullDepth %s and GameDepth %s are not one eye size' % (hull.shape, main.shape))
    clear = np.float32(clear_depth)
    mask = hull != clear
    n = int(mask.sum())
    out = {'eye': eye, 'mode': 'separate', 'hull_pixels': n}
    if n < MIN_HULL:
        out.update(verdict='NOT TAKEN OUT', why='the private depth holds %d hull pixels (under %d): nothing to measure' % (n, MIN_HULL))
        return out
    h, m = hull[mask], main[mask]
    holes = m == clear
    equal = m == h
    filled = (m < h) & ~holes
    nearer = m > h
    out.update(filled=float(filled.mean()), holes=float(holes.mean()), equal=float(equal.mean()), nearer=float(nearer.mean()))
    if out['equal'] > MAX_STILL_HULL:
        out.update(verdict='NOT TAKEN OUT', why='the main depth still equals the hull\'s on %.1f%% of its pixels: the own ship is in the main pass'
                   % (100 * out['equal']))
    else:
        out.update(verdict='TAKEN OUT', why='the own ship is out of the main pass: %.1f%% of its pixels now hold something farther, %.1f%% are holes'
                   % (100 * out['filled'], 100 * out['holes']))
    return out


def luma(rgb):
    return 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]


def within_fraction(priv, game, mask, tol):
    import numpy as np
    if not mask.any():
        return 0.0
    rel = np.abs(priv[mask] - game[mask]) / np.maximum(game[mask], 1e-3)
    return float((rel <= tol).mean())


def shown_mask(prefix, eye, depth, clear_depth, out):
    """The hull pixels the game shows: the private depth not at its clear value and, when a scene depth is there, equal to it."""
    import numpy as np
    import eye_inputs
    suffix = '_e1' if eye else ''
    mask = depth != np.float32(clear_depth)
    out['private_pixels'] = int(mask.sum())
    # Where the game shows the hull: the eye run's scene depth (eye_<stamp>_Z.bin, written for eye 0) equals the private
    # depth. Elsewhere the cockpit or canopy is in front in the game's image, and the two HDRs differ by design (the
    # 2026-11-08 20:30 flight: 27% of the private pixels visible, 85-98% of those bit-exact).
    # A separate-mode dump since dump v2 is a reference frame: the main pass kept the own ship, and eye_<stamp>_RefDepth.bin
    # (_e1 for eye 1) is its main depth after the eye's G-buffer, the same frame as HullDepth. It is the mask when present.
    zpath = '%s_Z.bin' % prefix
    refpath = '%s_RefDepth%s.bin' % (prefix, suffix)
    if os.path.exists(refpath):
        _, z = eye_inputs.read(refpath)
        z = np.asarray(z, np.float32)
        if z.shape != depth.shape:
            raise ValueError('RefDepth %s and HullDepth %s are not one eye size' % (z.shape, depth.shape))
        mask &= np.abs(z - depth) <= 1e-6 * np.maximum(np.abs(depth), np.float32(1e-9)) + np.float32(1e-9)
        out['visible_only'] = True
        out['scene_depth'] = 'RefDepth'
    elif os.path.exists(zpath):
        zmeta, z = eye_inputs.read(zpath)
        if zmeta.get('eye', 0) == eye and np.asarray(z).shape == depth.shape:
            z = np.asarray(z, np.float32)
            mask &= np.abs(z - depth) <= 1e-6 * np.maximum(np.abs(depth), np.float32(1e-9)) + np.float32(1e-9)
            out['visible_only'] = True
    return mask


def compare(prefix, eye=0, tol=0.05, clear_depth=0.0):
    """The measurement and verdict as a dict; raises ValueError on missing or mismatched files."""
    import numpy as np
    import eye_inputs
    suffix = '_e1' if eye else ''
    paths = {k: '%s_%s%s.bin' % (prefix, k, suffix) for k in ('HullHdr', 'GameHdrLit', 'HullDepth')}
    for k, p in paths.items():
        if not os.path.exists(p):
            raise ValueError('missing %s: %s' % (k, p))
    _, hull = eye_inputs.read(paths['HullHdr'])
    _, game = eye_inputs.read(paths['GameHdrLit'])
    _, depth = eye_inputs.read(paths['HullDepth'])
    if hull.ndim != 3 or game.ndim != 3 or hull.shape != game.shape or depth.shape != hull.shape[:2]:
        raise ValueError('the three files do not share one eye size (%s, %s, %s)' % (hull.shape, game.shape, depth.shape))
    out = {'eye': eye, 'tol': tol}
    mask = shown_mask(prefix, eye, depth, clear_depth, out)
    lh, lg = luma(np.nan_to_num(hull)), luma(np.nan_to_num(game))
    n = int(mask.sum())
    out['hull_pixels'] = n
    if n < MIN_HULL:
        out.update(verdict='REFUTED', why='the private depth holds %d hull pixels (under %d): the block was not drawn into it' % (n, MIN_HULL))
        return out
    lit = mask & (lg > 1e-4)
    game_lit = float(lit.sum()) / n
    out['game_lit'] = game_lit
    if game_lit < MIN_GAME_LIT:
        out.update(verdict='INCONCLUSIVE', why='the game\'s HDR is lit on only %.1f%% of the hull pixels: the comparison point is wrong, not the layer'
                   % (100 * game_lit))
        return out
    black = float(((lh <= 1e-6) & lit).sum()) / max(int(lit.sum()), 1)
    ratio = lh[lit] / np.maximum(lg[lit], 1e-6)
    within = within_fraction(lh, lg, mask, tol)
    best, best_frac = (0, 0), within
    h, w = lh.shape
    for dy in range(-SHIFT, SHIFT + 1):
        for dx in range(-SHIFT, SHIFT + 1):
            if (dx, dy) == (0, 0):
                continue
            # the private image moved by (dx, dy): its pixel (x, y) against the game's (x + dx, y + dy)
            ys, yd = slice(max(0, -dy), h - max(0, dy)), slice(max(0, dy), h - max(0, -dy))
            xs, xd = slice(max(0, -dx), w - max(0, dx)), slice(max(0, dx), w - max(0, -dx))
            frac = within_fraction(lh[ys, xs], lg[yd, xd], mask[ys, xs], tol)
            if frac > best_frac:
                best, best_frac = (dx, dy), frac
    out.update(within=within, black=black, median=float(np.median(ratio)) if ratio.size else 0.0,
               p95=float(np.percentile(ratio, 95)) if ratio.size else 0.0, shift=best, shift_within=best_frac)
    if black > 0.5:
        out.update(verdict='REFUTED', why='%.0f%% of the lit hull pixels are black in the private layer: unlit (the lighting bindings)' % (100 * black))
    elif best != (0, 0) and best_frac > within + 0.2:
        out.update(verdict='REFUTED', why='the layer matches %.0f%% shifted by %s against %.0f%% in place: offset' % (100 * best_frac, best, 100 * within))
    elif 0.95 <= out['median'] <= 1.35 and within >= 0.5:
        out.update(verdict='CONFIRMED', why='the private layer lights like the game on the hull (AO and decals aside)')
    else:
        out.update(verdict='INCONCLUSIVE', why='neither black nor offset, but the luma does not agree closely enough')
    return out


LOG_DUMP = re.compile(r"hull layer: dump (?P<stamp>\S+) eye (?P<eye>\d) frame \d+: .*?; own=(?P<own>-?\d+) filtered_px=(?P<filtered>-?\d+)")


def own_from_log(prefix, eye, log=None):
    """(own records, filtered pixels) from the dump's log line, or None: --log, else the edvr_gfx_*.log files beside the eyes folder, newest first."""
    stamp = os.path.basename(prefix)[len('eye_'):] if os.path.basename(prefix).startswith('eye_') else os.path.basename(prefix)
    logs = [log] if log else sorted(glob.glob(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(prefix))), 'edvr_gfx_*.log')),
                                    key=os.path.getmtime, reverse=True)
    for path in logs:
        try:
            text = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for m in LOG_DUMP.finditer(text):
            if m.group('stamp') == stamp and int(m.group('eye')) == eye:
                return int(m.group('own')), int(m.group('filtered'))
    return None


def report(r):
    if r.get('mode') == 'separate':
        line = 'hull layer compare (separate mode, the main pass): eye %d, %d private hull pixel(s)' % (r['eye'], r['hull_pixels'])
        if r.get('own') is not None:
            line += ', own mask %d record(s)' % r['own'][0]
        if 'filled' in r:
            line += ('; main depth farther (filled in) %.1f%%, at the clear value (holes) %.1f%%, equal to the hull (still there) %.1f%%, nearer '
                     '(cockpit or neighbour in front) %.1f%%' % (100 * r['filled'], 100 * r['holes'], 100 * r['equal'], 100 * r['nearer']))
        return line + '\n%s: %s' % (r['verdict'], r['why'])
    line = 'hull layer compare: eye %d, %d hull pixel(s)' % (r['eye'], r['hull_pixels'])
    if r.get('visible_only'):
        line += ' the game shows (of %d private; mask %s)' % (r['private_pixels'], r.get('scene_depth', 'Z'))
    if r.get('own') is not None:
        line += ', own mask %d record(s), %d pixel(s) filtered as neighbours' % r['own']
    if 'game_lit' in r:
        line += ', the game lit on %.1f%% of them' % (100 * r['game_lit'])
    if 'within' in r:
        line += ('; %.1f%% within %.0f%% of the game, ratio private/game median %.3f p95 %.3f, black %.1f%%, best shift %s (%.1f%% within)'
                 % (100 * r['within'], 100 * r['tol'], r['median'], r['p95'], 100 * r['black'], r['shift'], 100 * r['shift_within']))
    return line + '\n%s: %s' % (r['verdict'], r['why'])


REFL_MATCH = (0.9, 1.15)   # --refl: the private HDR after the pass within this of the game's (median ratio): it matches
REFL_SURPLUS = 1.15        # --refl: a median ratio before the pass above this is a surplus from before it
REFL_STENCIL = 0.95        # --refl: the stencil test agreeing on fewer hull pixels than this differs
REFL_T11 = 0.02            # --refl: a median |t11.w| difference above this differs


def stencil_plane(path):
    """The 8-bit stencil plane of a dumped depth-stencil (D32S8, D24S8) or one-byte copy, as uint8 (h, w)."""
    import numpy as np
    import eye_inputs
    meta, data = eye_inputs._load(path)
    fmt, w, h = meta['format'], meta['width'], meta['height']
    if fmt in (19, 20):
        return np.frombuffer(data, 'u1', offset=44).reshape(h, w, 8)[..., 4].copy()
    if fmt in (44, 45):
        return (np.frombuffer(data, '<u4', offset=44).reshape(h, w) >> 24).astype(np.uint8)
    if fmt in (60, 61, 62):
        return np.frombuffer(data, 'u1', offset=44).reshape(h, w).copy()
    raise ValueError('%s: DXGI format %d carries no stencil plane' % (path, fmt))


def reflection(prefix, eye=0, clear_depth=0.0):
    """--refl: the private HDR against the game's before and after the reflection pass, and the pass's inputs, on the hull pixels."""
    import json
    import numpy as np
    import eye_inputs
    from collections import Counter
    suffix = '_e1' if eye else ''
    jpath = '%s_Refl%s.json' % (prefix, suffix)
    if not os.path.exists(jpath):
        raise ValueError('missing %s: no reflection dump (a build before the reflection dump, or no eye staged)' % jpath)
    try:
        with open(jpath, encoding='utf-8') as f:
            meta = json.load(f)
    except (OSError, ValueError) as e:
        raise ValueError('%s: %s' % (jpath, e))
    cb = meta.get('cb1_26')
    out = {'eye': eye, 'mode': 'reflection', 'game_steps': meta.get('game_steps', 0), 'hull_steps': meta.get('hull_steps', 0),
           'stencil_mask': cb[1] if isinstance(cb, list) and len(cb) == 4 else None, 'u1_clear': meta.get('u1_clear'),
           'u1_from_game': meta.get('u1_clear_from_game')}
    if not out['game_steps'] or not out['hull_steps']:
        out.update(verdict='NOT STAGED', why='the reflection pass was not staged on both sides (game %s, private %s steps): %s'
                   % (out['game_steps'], out['hull_steps'], meta.get('why') or 'no reason given'))
        return out
    paths = {k: '%s_%s%s.bin' % (prefix, k, suffix) for k in ('HullHdr', 'GameHdrLit', 'HullDepth', 'HullHdrRefl', 'GameHdrRefl')}
    for k, p in paths.items():
        if not os.path.exists(p):
            raise ValueError('missing %s: %s' % (k, p))
    img = {k: np.nan_to_num(np.asarray(eye_inputs.read(p)[1], np.float32)) for k, p in paths.items()}
    depth = img['HullDepth']
    for k in ('HullHdr', 'GameHdrLit', 'HullHdrRefl', 'GameHdrRefl'):
        if img[k].ndim != 3 or img[k].shape[:2] != depth.shape:
            raise ValueError('%s %s and HullDepth %s are not one eye size' % (k, img[k].shape, depth.shape))
    mask = shown_mask(prefix, eye, depth, clear_depth, out)
    n = int(mask.sum())
    out['hull_pixels'] = n
    if n < MIN_HULL:
        out.update(verdict='INCONCLUSIVE', why='%d hull pixels the game shows (under %d): nothing to measure' % (n, MIN_HULL))
        return out
    lh, lg = luma(img['HullHdr']), luma(img['GameHdrLit'])
    lhr, lgr = luma(img['HullHdrRefl']), luma(img['GameHdrRefl'])

    def median_ratio(a, b, where):
        sel = where & (b > 1e-4)
        return float(np.median(a[sel] / b[sel])) if sel.any() else None

    out['before'] = median_ratio(lh, lg, mask)
    out['after'] = median_ratio(lhr, lgr, mask)
    ch, cg = lhr - lh, lgr - lg   # the pass's own addition on each side
    out['pass_median'] = median_ratio(ch, cg, mask)
    sg = float(cg[mask].sum())
    out['pass_sum'] = float(ch[mask].sum()) / sg if sg > 1e-6 else None
    out['pass_share_game'] = float(cg[mask].sum() / max(float(lgr[mask].sum()), 1e-6))
    cand = []
    if out['before'] is not None and out['before'] > REFL_SURPLUS:
        cand.append('a surplus before the pass (median %.3f)' % out['before'])
    st = {k: '%s_%s%s.bin' % (prefix, k, suffix) for k in ('GameStencil', 'HullStencil')}
    if all(os.path.exists(p) for p in st.values()):
        g, h = stencil_plane(st['GameStencil']), stencil_plane(st['HullStencil'])
        if g.shape != depth.shape or h.shape != depth.shape:
            raise ValueError('the stencils %s %s and HullDepth %s are not one eye size' % (g.shape, h.shape, depth.shape))
        g, h = g[mask], h[mask]
        out['stencil_equal'] = float((g == h).mean())
        m = out['stencil_mask']
        if m is not None:
            m &= 0xff
            tg, th = (g & m) == m, (h & m) == m
            out['stencil_test_equal'] = float((tg == th).mean())
            out['stencil_test_game'], out['stencil_test_hull'] = float(tg.mean()), float(th.mean())
        diff = g != h
        out['stencil_pairs'] = Counter(zip(g[diff].tolist(), h[diff].tolist())).most_common(5)
        agree = out.get('stencil_test_equal', out['stencil_equal'])
        if agree < REFL_STENCIL:
            cand.append('the stencil (%s agrees on %.1f%%)' % ('the mask test' if m is not None else 'the value', 100 * agree))
    else:
        out['stencil_missing'] = True
    tp = {k: '%s_%s%s.bin' % (prefix, k, suffix) for k in ('GameT11', 'HullT11')}
    if all(os.path.exists(p) for p in tp.values()):
        g, h = (np.nan_to_num(np.asarray(eye_inputs.read(p)[1], np.float32)) for p in (tp['GameT11'], tp['HullT11']))
        if g.ndim == 3 and h.ndim == 3 and g.shape == h.shape and g.shape[2] >= 4 and g.shape[:2] == depth.shape:
            dw = h[..., 3][mask] - g[..., 3][mask]
            out['t11w_median_abs'], out['t11w_mean'] = float(np.median(np.abs(dw))), float(dw.mean())
            out['t11w_game'], out['t11w_hull'] = float(np.median(g[..., 3][mask])), float(np.median(h[..., 3][mask]))
            if out['t11w_median_abs'] > REFL_T11:
                cand.append('t11.w (median |difference| %.3f)' % out['t11w_median_abs'])
        else:
            out['t11_unread'] = 'shapes %s %s' % (g.shape, h.shape)
    else:
        out['t11_missing'] = True
    out['candidates'] = cand
    after = 'n/a' if out['after'] is None else '%.3f' % out['after']
    lo, hi = REFL_MATCH
    if out['after'] is not None and lo <= out['after'] <= hi:
        out.update(verdict='MATCHES', why='the private HDR after the reflection pass matches the game\'s on the hull (median %s)' % after)
    elif cand:
        out.update(verdict='DIFFERS', why='after the pass median %s; the differing input(s): %s' % (after, '; '.join(cand)))
    else:
        out.update(verdict='INCONCLUSIVE', why='after the pass median %s, but the stencil, t11.w and the light before it agree: the pass\'s other '
                   'inputs (the probes, the cubemaps, u1 if it reads it)' % after)
    return out


def reflection_report(r):
    def f(v):
        return 'n/a' if v is None else '%.3f' % v
    line = 'hull layer compare (reflection pass): eye %d, steps staged game %s private %s' % (r['eye'], r['game_steps'], r['hull_steps'])
    if 'hull_pixels' in r:
        line += ', %d hull pixel(s) the game shows (of %d private)' % (r['hull_pixels'], r.get('private_pixels', 0))
    if 'before' in r:
        line += ('\n  luma private/game: before the pass median %s, after it %s; the pass\'s own addition private/game median %s, sum %s '
                 '(the pass is %.0f%% of the game\'s light after it)' % (f(r['before']), f(r['after']), f(r['pass_median']), f(r['pass_sum']),
                                                                         100 * r['pass_share_game']))
    if 'stencil_equal' in r:
        line += '\n  stencil (t4): equal on %.1f%%' % (100 * r['stencil_equal'])
        if 'stencil_test_equal' in r:
            m = r['stencil_mask'] & 0xff
            line += (', the test (s & 0x%02X) == 0x%02X agrees on %.1f%% (true on %.1f%% game, %.1f%% private)'
                     % (m, m, 100 * r['stencil_test_equal'], 100 * r['stencil_test_game'], 100 * r['stencil_test_hull']))
        else:
            line += ', cb1[26].y not read'
        if r['stencil_pairs']:
            line += '; top (game, private) mismatches: ' + ', '.join('(%d, %d) x%d' % (a, b, c) for (a, b), c in r['stencil_pairs'])
    elif r.get('stencil_missing'):
        line += '\n  stencil (t4): not dumped'
    if 't11w_median_abs' in r:
        line += ('\n  t11.w: median game %.3f private %.3f, |difference| median %.3f, mean private-game %+.3f'
                 % (r['t11w_game'], r['t11w_hull'], r['t11w_median_abs'], r['t11w_mean']))
    elif r.get('t11_missing') or r.get('t11_unread'):
        line += '\n  t11: %s' % ('not dumped' if r.get('t11_missing') else 'no w channel to compare (%s)' % r['t11_unread'])
    if r.get('u1_clear') is not None:
        line += '\n  u1 (R16): the private copy cleared to %g (%s)' % (r['u1_clear'], 'the game\'s clear' if r.get('u1_from_game') else 'no game clear seen')
    return line + '\n%s: %s' % (r['verdict'], r['why'])


def pack_r11g11b10(rgb):
    """float RGB (non-negative, normal or zero) as R11G11B10_FLOAT words: the self-test's encoder."""
    import numpy as np
    words = np.zeros(rgb.shape[:-1], dtype=np.uint32)
    for c, (shift, bits) in enumerate(((0, 6), (11, 6), (22, 5))):
        v = rgb[..., c].astype(np.float64)
        e = np.where(v > 0, np.floor(np.log2(np.where(v > 0, v, 1.0))), -15).astype(np.int64)
        mant = np.where(v > 0, np.round((v / np.exp2(e) - 1.0) * (1 << bits)), 0).astype(np.int64)
        e, mant = np.where(mant == (1 << bits), e + 1, e), np.where(mant == (1 << bits), 0, mant)
        field = np.where(v > 0, ((e + 15) << bits) | mant, 0).astype(np.uint32)
        words |= field << shift
    return words


def lights(prefix):
    """HullLights.json's HDR writers grouped by (eye, VS): [(eye, vs, draws, reissued, unknown, instances, ps set)], and the file's head."""
    import json
    path = prefix + '_HullLights.json'
    try:
        with open(path, encoding='utf-8') as f:
            doc = json.load(f)
        draws = doc['draws']
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise ValueError('%s: %s' % (path, e))
    groups = {}
    for d in draws:
        if not d.get('writes_hdr'):
            continue
        g = groups.setdefault((d['eye'], d['vs']), [0, 0, 0, 0, set()])
        g[0] += 1
        g[1] += bool(d.get('reissued'))
        g[2] += bool(d.get('unknown'))
        g[3] += int(d.get('instances', 0))
        g[4].add(d.get('ps', '?'))
    rows = [(eye, vs, g[0], g[1], g[2], g[3], sorted(g[4])) for (eye, vs), g in sorted(groups.items())]
    return {'stamp': doc.get('stamp'), 'frame': doc.get('frame'), 'reference_frame': doc.get('reference_frame'), 'capped': doc.get('capped'),
            'draws': len(draws), 'rows': rows}


def lights_report(r):
    out = ['HullLights %s frame %s (reference frame %s%s): %d draw(s) in the lighting windows; the HDR writers by eye and VS:'
           % (r['stamp'], r['frame'], r['reference_frame'], ', CAPPED' if r['capped'] else '', r['draws'])]
    out.append('  eye  vs                draws  reissued  unknown  instances  ps')
    for eye, vs, n, re_, unk, inst, ps in r['rows']:
        out.append('  %3s  %-16s  %5d  %8d  %7d  %9d  %s' % (eye, vs, n, re_, unk, inst, ','.join(p[:8] for p in ps)))
    if not r['rows']:
        out.append('  (none: the layer was asleep or no lighting window opened on the dump frame)')
    return '\n'.join(out)


def self_test():
    import tempfile
    try:
        import numpy as np
    except ImportError:
        print('hull layer compare self-test skipped: NumPy is not installed')
        return 0
    failures = []

    def write(path, fmt, eye, payload, w, h, px):
        with open(path, 'wb') as f:
            f.write(b'EDVRTEX1' + struct.pack('<9I', 1, w, h, fmt, w * px, 7, eye, 0, 0) + payload)

    def case(name, priv_rgb, game_rgb, depth, want, eye=0, scene_z=None, ref_depth=None):
        with tempfile.TemporaryDirectory() as temp:
            prefix = os.path.join(temp, 'eye_123456789001')
            sfx = '_e1' if eye else ''
            h, w = depth.shape
            write(prefix + '_HullHdr%s.bin' % sfx, 26, eye, pack_r11g11b10(priv_rgb).tobytes(), w, h, 4)
            write(prefix + '_GameHdrLit%s.bin' % sfx, 26, eye, pack_r11g11b10(game_rgb).tobytes(), w, h, 4)
            d = np.zeros((h, w, 2), dtype=np.float32)
            d[..., 0] = depth
            write(prefix + '_HullDepth%s.bin' % sfx, 19, eye, d.tobytes(), w, h, 8)
            if scene_z is not None:
                write(prefix + '_Z.bin', 41, eye, scene_z.astype(np.float32).tobytes(), w, h, 4)
            if ref_depth is not None:   # dump v2's reference frame: the main depth with the own ship drawn, the D32S8 layout
                rd = np.zeros((h, w, 2), dtype=np.float32)
                rd[..., 0] = ref_depth
                write(prefix + '_RefDepth%s.bin' % sfx, 19, eye, rd.tobytes(), w, h, 8)
            r = compare(prefix, eye=eye)
            if r['verdict'] != want:
                failures.append('%s: %s, wanted %s (%r)' % (name, r['verdict'], want, r))
            if ref_depth is not None and r.get('scene_depth') != 'RefDepth':
                failures.append('%s: the reference depth was not the mask (%r)' % (name, r))

    h, w = 48, 64
    yy, xx = np.mgrid[0:h, 0:w]
    # A textured lit scene: luma varies by more than the tolerance between neighbours, so a shift is visible.
    base = 0.2 + 0.8 * ((xx * 7 + yy * 13) % 11) / 10.0
    game = np.stack([base, base * 0.9, base * 0.8], axis=-1)
    depth = np.zeros((h, w), dtype=np.float32)
    depth[8:40, 10:60] = 0.25   # 1600 hull pixels
    hull = np.where(depth[..., None] > 0, game, 0.0)
    case('the same light on the hull', hull, game, depth, 'CONFIRMED')
    case('the same light, eye 1', hull, game, depth, 'CONFIRMED', eye=1)
    case('a black layer', np.zeros_like(game), game, depth, 'REFUTED')
    shifted = np.where(depth[..., None] > 0, np.roll(np.roll(game, -2, axis=1), 1, axis=0), 0.0)
    case('a layer two pixels off', shifted, game, depth, 'REFUTED')
    case('an empty private depth', hull, game, np.zeros_like(depth), 'REFUTED')
    case('a layer at half the light', hull * 0.5, game, depth, 'INCONCLUSIVE')
    case('the game black too (the wrong comparison point)', np.zeros_like(game), np.zeros_like(game), depth, 'INCONCLUSIVE')
    # The cockpit in front of most of the hull in the game's image (reversed Z: nearer is larger): its pixels differ by
    # design. With the scene depth the comparison keeps the hull the game shows and confirms; without it, it cannot.
    cockpit = depth > 0
    cockpit[8:40, 10:44] = False
    occluded_game = np.where(cockpit[..., None], game, 0.05)
    scene_z = np.where(cockpit, depth, 0.9).astype(np.float32)
    case('a cockpit in front, scene depth given', hull, occluded_game, depth, 'CONFIRMED', scene_z=scene_z)
    case('a cockpit in front, no scene depth', hull, occluded_game, depth, 'INCONCLUSIVE')
    # Dump v2: a separate-mode reference frame gives RefDepth (eye 1 too), and it wins over a scene depth of another frame.
    case('a cockpit in front, the reference frame\'s depth given', hull, occluded_game, depth, 'CONFIRMED', ref_depth=scene_z)
    case('a cockpit in front, eye 1, the reference depth', hull, occluded_game, depth, 'CONFIRMED', eye=1, ref_depth=scene_z)
    case('the reference depth wins over a stale Z', hull, occluded_game, depth, 'CONFIRMED', scene_z=np.full_like(scene_z, 0.9), ref_depth=scene_z)
    # The own-mask count comes from the dump's log line beside the eyes folder.
    with tempfile.TemporaryDirectory() as temp:
        os.makedirs(os.path.join(temp, 'eyes'))
        with open(os.path.join(temp, 'edvr_gfx_20261108_120000.log'), 'w', encoding='utf-8') as f:
            f.write('[12:00:01.000] hull layer: dump 123456789001 eye 1 frame 9: HullHdr 64x48 format 26 written, GameHdrLit written, HullDepth written; '
                    'own=54 filtered_px=321 seed=3/3 hull_draws=84 light_draws=11\n')
        got = own_from_log(os.path.join(temp, 'eyes', 'eye_123456789001'), 1)
        if got != (54, 321):
            failures.append('the own count from the log: %r, wanted (54, 321)' % (got,))
        if own_from_log(os.path.join(temp, 'eyes', 'eye_123456789001'), 0) is not None:
            failures.append('the own count of eye 0 was read off eye 1\'s line')
    # Separate mode: the main depth with the hull discarded (terrain farther, sky at the clear value) against one that kept it.
    def separate_case(name, main_depth, want, eye=0):
        with tempfile.TemporaryDirectory() as temp:
            prefix = os.path.join(temp, 'eye_123456789001')
            sfx = '_e1' if eye else ''
            for label, plane in (('HullDepth', depth), ('GameDepth', main_depth)):
                d = np.zeros((h, w, 2), dtype=np.float32)
                d[..., 0] = plane
                write(prefix + '_%s%s.bin' % (label, sfx), 19, eye, d.tobytes(), w, h, 8)
            r = main_pass(prefix, eye=eye)
            if r['verdict'] != want:
                failures.append('%s: %s, wanted %s (%r)' % (name, r['verdict'], want, r))
            return r
    terrain = np.where(xx < 35, 0.1, 0.0).astype(np.float32)   # the terrain behind the hull's left part, open sky on the right
    got = separate_case('the hull taken out of the main pass', terrain, 'TAKEN OUT')
    want = {'filled': float((terrain[8:40, 10:60] > 0).mean()), 'holes': float((terrain[8:40, 10:60] == 0).mean())}
    if abs(got.get('filled', -1) - want['filled']) > 1e-9 or abs(got.get('holes', -1) - want['holes']) > 1e-9:
        failures.append('the filled and hole shares: %r, wanted %r' % (got, want))
    separate_case('the hull taken out, eye 1', terrain, 'TAKEN OUT', eye=1)
    separate_case('the hull still in the main pass', np.maximum(depth, terrain), 'NOT TAKEN OUT')
    # A missing file is bad input.
    with tempfile.TemporaryDirectory() as temp:
        try:
            compare(os.path.join(temp, 'eye_x'))
            failures.append('a missing file was accepted')
        except ValueError:
            pass
    # --lights: HullLights.json in the layout writeLights prints; the HDR writers grouped by eye and VS, stencil-only draws left out.
    with tempfile.TemporaryDirectory() as temp:
        prefix = os.path.join(temp, 'eye_123456789001')

        def rec(eye, vs, ps, inst, hdr, reissued, unknown, stencil=False):
            return ('{"eye": %d, "seq": 1, "vs": "%s", "ps": "%s", "kind": "I", "count": 36, "instances": %d, "start_instance": 0, "writes_hdr": %s, '
                    '"stencil_only": %s, "stencil_ref": 0, "known_light": %s, "stage_pair": false, "reissued": %s, "unknown": %s}'
                    % (eye, vs, ps, inst, 'true' if hdr else 'false', 'true' if stencil else 'false', 'false' if unknown else 'true',
                       'true' if reissued else 'false', 'true' if unknown else 'false'))
        with open(prefix + '_HullLights.json', 'w', encoding='utf-8') as f:
            f.write('{"version": 1, "stamp": "123456789001", "frame": 42, "reference_frame": true, "capped": false, "cap": 4096,\n "draws": [\n  '
                    + ',\n  '.join([rec(0, 'A1B7CFCD0BE7493E', '2DB678B6B558B604', 200, True, True, False),
                                    rec(0, 'A1B7CFCD0BE7493E', '2DB678B6B558B604', 26, True, True, False),
                                    rec(0, '2CECEC3000000000', '1111111111111111', 1, True, False, True),
                                    rec(0, 'A1B7CFCD0BE7493E', '0000000000000000', 200, False, False, False, stencil=True),
                                    rec(1, 'A1B7CFCD0BE7493E', '2DB678B6B558B604', 198, True, False, False)]) + '\n ]\n}\n')
        got = lights(prefix)['rows']
        want = [(0, '2CECEC3000000000', 1, 0, 1, 1, ['1111111111111111']), (0, 'A1B7CFCD0BE7493E', 2, 2, 0, 226, ['2DB678B6B558B604']),
                (1, 'A1B7CFCD0BE7493E', 1, 0, 0, 198, ['2DB678B6B558B604'])]
        if got != want:
            failures.append('--lights rows %r, wanted %r' % (got, want))
        if 'A1B7CFCD0BE7493E      2         2        0        226' not in lights_report(lights(prefix)):
            failures.append('--lights report: %s' % lights_report(lights(prefix)))
        try:
            lights(os.path.join(temp, 'eye_x'))
            failures.append('--lights accepted a missing file')
        except ValueError:
            pass
    # --refl: the reflection dump. The game's hull at `base`, the pass adding `add` on each side; stencils in the D32S8 (game) and
    # D24S8 (private) layouts; t11 as RGBA8; Refl.json as writeReflection prints it.
    def refl_case(name, want, before=1.0, pass_gain=1.0, hull_stencil=0x04, t11w=1.0, steps=(2, 2), mask=4, eye=0, check=None):
        with tempfile.TemporaryDirectory() as temp:
            prefix = os.path.join(temp, 'eye_123456789001')
            sfx = '_e1' if eye else ''
            lit, add = game * 0.4, game * 0.6
            write(prefix + '_GameHdrLit%s.bin' % sfx, 26, eye, pack_r11g11b10(lit).tobytes(), w, h, 4)
            write(prefix + '_GameHdrRefl%s.bin' % sfx, 26, eye, pack_r11g11b10(lit + add).tobytes(), w, h, 4)
            write(prefix + '_HullHdr%s.bin' % sfx, 26, eye, pack_r11g11b10(lit * before).tobytes(), w, h, 4)
            write(prefix + '_HullHdrRefl%s.bin' % sfx, 26, eye, pack_r11g11b10(lit * before + add * pass_gain).tobytes(), w, h, 4)
            d = np.zeros((h, w, 2), dtype=np.float32)
            d[..., 0] = depth
            write(prefix + '_HullDepth%s.bin' % sfx, 19, eye, d.tobytes(), w, h, 8)
            gs = np.zeros((h, w, 8), dtype=np.uint8)
            gs[..., 4] = 0x04
            write(prefix + '_GameStencil%s.bin' % sfx, 20, eye, gs.tobytes(), w, h, 8)
            hs = (np.full((h, w), hull_stencil, dtype=np.uint32) << 24) | 0x123456
            write(prefix + '_HullStencil%s.bin' % sfx, 44, eye, hs.astype('<u4').tobytes(), w, h, 4)
            for label, a in (('GameT11', 1.0), ('HullT11', t11w)):
                t = np.zeros((h, w, 4), dtype=np.uint8)
                t[..., 3] = int(round(a * 255))
                write(prefix + '_%s%s.bin' % (label, sfx), 28, eye, t.tobytes(), w, h, 4)
            with open(prefix + '_Refl%s.json' % sfx, 'w', encoding='utf-8') as f:
                f.write('{"version": 1, "stamp": "123456789001", "frame": 7, "eye": %d, "game_steps": %d, "hull_steps": %d, "cb1_first": 0, '
                        '"cb1_26": [0, %d, 0, 0], "u1_clear": 0.000000, "u1_clear_from_game": false, "why": "%s"}\n'
                        % (eye, steps[0], steps[1], mask, '' if steps[1] else 'the step was not re-issued'))
            r = reflection(prefix, eye=eye)
            text = reflection_report(r)
            if r['verdict'] != want:
                failures.append('--refl %s: %s, wanted %s (%s)' % (name, r['verdict'], want, text))
            elif check and check not in text:
                failures.append('--refl %s: %r not in the report: %s' % (name, check, text))
    refl_case('the private pass like the game\'s', 'MATCHES', check='before the pass median 1.000, after it 1.000')
    refl_case('eye 1', 'MATCHES', eye=1)
    refl_case('the pass adds twice, same inputs', 'INCONCLUSIVE', pass_gain=2.0, check='own addition private/game median 1.99')
    refl_case('a surplus before the pass', 'DIFFERS', before=2.0, check='a surplus before the pass')
    refl_case('the private stencil fails the mask test', 'DIFFERS', pass_gain=2.0, hull_stencil=0x00, check='(4, 0) x1600')
    refl_case('a stencil differing outside the mask', 'INCONCLUSIVE', pass_gain=2.0, hull_stencil=0x0C, check='agrees on 100.0%')
    refl_case('t11.w differs', 'DIFFERS', pass_gain=2.0, t11w=0.5, check='t11.w (median |difference| 0.49')
    refl_case('the re-issue never ran', 'NOT STAGED', steps=(2, 0), check='the step was not re-issued')
    with tempfile.TemporaryDirectory() as temp:
        try:
            reflection(os.path.join(temp, 'eye_x'))
            failures.append('--refl accepted a dump with no Refl.json')
        except ValueError:
            pass
    for f in failures:
        print('hull layer compare self-test FAILED: ' + f)
    if not failures:
        print('hull layer compare self-test passed (29 cases)')
    return 1 if failures else 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('prefix', nargs='?', help='the eye run prefix: <edvr_logs>\\eyes\\eye_<stamp>')
    p.add_argument('--eye', type=int, default=0, choices=(0, 1))
    p.add_argument('--tol', type=float, default=0.05, help='relative luma tolerance (default 0.05: R11G11B10 rounds to about 1.6%%)')
    p.add_argument('--clear-depth', type=float, default=0.0, help='the private depth\'s clear value (reversed Z: 0)')
    p.add_argument('--log', help='the graphics log holding the dump\'s line (default: the newest edvr_gfx_*.log beside the eyes folder)')
    p.add_argument('--lights', action='store_true', help='list HullLights.json\'s HDR writers by eye and VS instead of comparing')
    p.add_argument('--refl', action='store_true', help='compare the two sides of the reflection pass (eye_<stamp>_Refl.json and its files)')
    p.add_argument('--self-test', action='store_true')
    a = p.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.prefix:
        p.error('give the eye run prefix, or --self-test')
    if a.lights:
        try:
            print(lights_report(lights(a.prefix)))
        except ValueError as e:
            print('hull layer compare: %s' % e)
            return 2
        return 0
    if a.refl:
        try:
            r = reflection(a.prefix, eye=a.eye, clear_depth=a.clear_depth)
        except ValueError as e:
            print('hull layer compare: %s' % e)
            return 2
        print(reflection_report(r))
        return {'MATCHES': 0, 'DIFFERS': 3, 'NOT STAGED': 2}.get(r['verdict'], 1)
    try:
        separate = os.path.exists('%s_GameDepth%s.bin' % (a.prefix, '_e1' if a.eye else ''))
        r = main_pass(a.prefix, eye=a.eye, clear_depth=a.clear_depth) if separate else compare(a.prefix, eye=a.eye, tol=a.tol, clear_depth=a.clear_depth)
        r['own'] = own_from_log(a.prefix, a.eye, a.log)
    except ValueError as e:
        print('hull layer compare: %s' % e)
        return 2
    print(report(r))
    return {'CONFIRMED': 0, 'TAKEN OUT': 0, 'REFUTED': 3, 'NOT TAKEN OUT': 3}.get(r['verdict'], 1)


if __name__ == '__main__':
    sys.exit(main())
