#!/usr/bin/env python3
"""Score one hull bench press (hotkey.hull_bench, TEMPORARY) into a table and a static HTML page.

Usage: python tools/hull_bench_report.py <edvr_logs>\\eyes\\eye_<stamp> [--out file.html] [--eye 0|1] [--expect-build REF]
           [--dry-run]
       python tools/hull_bench_report.py --self-test

Reads eye_<stamp>_Bench.json (schema 1: order, variants with status, frames, gpu_ms, mb), per variant
eye_<stamp>_Bench_<id>_fNN.bin (eye 0, the image after the hull composite, EDVRTEX1) or _f00_e1.bin (eye 1, one frame),
_final.bin / _final_e1.bin (the headset frame), and eye_<stamp>_Bench_Mask.bin (_e1): the hull coverage in alpha, at the
captures' size. Without the manifest the variants are found by file name in the bench's order.

Regions (output pixels, from the mask): interior = erode(alpha > 0.99, 4); silhouette = dilate(2) - erode(2) of
alpha > 0.5; wing band = dilate(14) - dilate(2), the terrain just outside the hull edge. Luma is Rec.709 on the decoded
values; L_t is frame t's luma, Lm the temporal mean. 8-bit units are luma x 255.

  shimmer int/sil  mean per-pixel temporal std of luma over the frames, interior / silhouette (8-bit units)
  sharp            Laplacian energy of Lm on the interior (hull_taa_sim.hf_energy), / sep_taa's
  stair            silhouette: mean over frames of |L_t - G1(L_t)| (G1 a 1-px Gaussian), / local contrast (5x5 range of Lm)
  crawl            silhouette: temporal std of L_t - G1(L_t), / the same local contrast
  smear            wing band: mean |Lm - R| / mean |R - mean R|, R the per-pixel median of the hull-free variants'
                   Lm (HULL_FREE: the sep_* variants, dlss_full, dlss_sub; orig_a when none of them was captured)
  band hf          wing band Laplacian energy of Lm / R's (1 = the terrain's detail kept, < 1 smeared)
  dluma / drgb     interior: mean Lm / orig_a's, and mean |RGB mean - orig_a's| (8-bit units)
  still            the variant's largest head rotation (deg/frame, from the manifest)
  stages / A/B     the layer's GPU ms by stage (layer_stage); sep_norefl, sep_nopre, sep_norect against sep_taa: dluma,
                   layer ms and each stage's ms as differences (the row's reason says whether its feature was in fact off)
The press is UNRELIABLE (pilot moved) when orig_a and orig_b differ by more than 1 8-bit unit and more than 3 times
their temporal noise / sqrt(frames) on interior + silhouette + wing band.

Writes the HTML (default eye_<stamp>_Bench.html beside the captures; --dry-run writes nothing). Exit: 0, 3 pilot moved
(the press is unreliable), 2 bad input (no mask, nothing captured, a manifest that does not parse, --expect-build
mismatch).
"""
import argparse
import base64
import glob
import json
import math
import os
import re
import struct
import sys
import zlib

ORDER = ('orig_a', 'sep_taa', 'sep_norefl', 'sep_nopre', 'sep_norect', 'sep_noaa', 'dlss_full', 'dlss_sub', 'native', 'native_fxaa',
         'orig_b')
HULL_FREE = ('sep_noaa', 'sep_taa', 'sep_norefl', 'sep_nopre', 'sep_norect', 'dlss_full', 'dlss_sub')
# sep_taa with one feature off (hull_bench.h): the reflection re-issue, Build B's depth prefill, Build A''s rectangle. The
# A/B lines give each one's dluma, layer ms and stage ms against sep_taa's.
AB = ('sep_norefl', 'sep_nopre', 'sep_norect')
STAGES = ('gbuffer', 'mask_filter', 'screen', 'light', 'tone', 'taa')   # hull_layer.cpp TStage, the manifest's layer_stage order
ZOOM = 256
OVERVIEW_MAX = 960
MOVED_MIN = 1.0       # 8-bit units
MOVED_SIGMAS = 3.0

# key, header, better: 'low' | 'high' | 'one' (closest to 1), digits
COLUMNS = (('layer_ms', 'layer ms', 'low', 3), ('var_ms', 'variant ms', 'low', 3), ('hull_dlss', 'hull DLSS ms', 'low', 3),
           ('main_dlss', 'main DLSS ms', 'low', 3), ('frame_ms', 'frame ms', 'low', 3), ('mb_own', 'MB own', 'low', 1),
           ('mb_ngx', 'MB ngx', 'low', 1), ('shim_int', 'shimmer int', 'low', 2), ('shim_sil', 'shimmer sil', 'low', 2),
           ('sharp', 'sharp', 'high', 3), ('stair', 'stair', 'low', 3), ('crawl', 'crawl', 'low', 3), ('smear', 'smear', 'low', 3),
           ('band_hf', 'band hf', 'one', 3), ('dluma', 'dluma', 'one', 3), ('drgb', 'drgb', 'low', 2), ('still', 'still', 'low', 3))
STDOUT_COLS = ('var_ms', 'frame_ms', 'mb_own', 'mb_ngx', 'shim_int', 'shim_sil', 'sharp', 'stair', 'crawl', 'smear', 'band_hf', 'dluma',
               'drgb')


class BadInput(ValueError):
    pass


# ---- reading -------------------------------------------------------------------------------------------------------------

def read_tex(path):
    """(meta, image) through eye_inputs.read; the composite's formats it lacks (R10G10B10A2_UNORM 24, B8G8R8A8 87/91) here."""
    import numpy as np
    import eye_inputs
    try:
        return eye_inputs.read(path)
    except ValueError as e:
        err = e
    data = open(path, 'rb').read()
    if data[:8] != b'EDVRTEX1' or len(data) < 44:
        raise err
    meta = dict(zip(('version', 'width', 'height', 'format', 'row_bytes', 'frame', 'eye', 'ui_bound', 'ui_flags'),
                    struct.unpack_from('<9I', data, 8)))
    w, h, fmt = meta['width'], meta['height'], meta['format']
    if fmt not in (24, 87, 91) or meta['row_bytes'] != w * 4 or len(data) != 44 + w * h * 4:
        raise err
    if fmt == 24:
        words = np.frombuffer(data, '<u4', offset=44).reshape(h, w)
        img = np.stack([((words >> s) & m) / float(m) for s, m in ((0, 1023), (10, 1023), (20, 1023), (30, 3))], axis=-1)
    else:
        img = (np.frombuffer(data, 'u1', offset=44).reshape(h, w, 4).astype(np.float32) / 255.0)[..., [2, 1, 0, 3]]
    return meta, img.astype(np.float32)


def rgb_of(img):
    import numpy as np
    img = np.asarray(img, np.float32)
    if img.ndim == 2:
        img = img[..., None]
    if img.shape[2] < 3:
        img = np.concatenate([img] + [img[..., :1]] * (3 - img.shape[2]), axis=2)
    return np.nan_to_num(img[..., :3], nan=0.0, posinf=1.0, neginf=0.0)


def luma(rgb):
    from hull_taa_sim import luma as l709
    return l709(rgb)


def hf_energy(l, mask):
    from hull_taa_sim import hf_energy as hf
    return hf(l, mask)


# ---- filters -------------------------------------------------------------------------------------------------------------

def _run(a, r, axis, fn):
    """fn (np.maximum / np.minimum) over a 2r+1 window along axis, edges extended."""
    import numpy as np
    if r <= 0:
        return a
    pad = [(0, 0)] * a.ndim
    pad[axis] = (r, r)
    p = np.pad(a, pad, mode='edge')
    n = a.shape[axis]

    def sl(k):
        return tuple(slice(k, k + n) if i == axis else slice(None) for i in range(a.ndim))
    out = p[sl(0)].copy()
    for k in range(1, 2 * r + 1):
        fn(out, p[sl(k)], out=out)
    return out


def dilate(m, r):
    import numpy as np
    return _run(_run(m, r, 0, np.maximum), r, 1, np.maximum)


def erode(m, r):
    import numpy as np
    return _run(_run(m, r, 0, np.minimum), r, 1, np.minimum)


def gauss(a, sigma):
    """Separable Gaussian over the first two axes, edges extended."""
    import numpy as np
    r = max(1, int(math.ceil(3 * sigma)))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    out = a.astype(np.float32)
    for axis in (0, 1):
        pad = [(0, 0)] * out.ndim
        pad[axis] = (r, r)
        p = np.pad(out, pad, mode='edge')
        n = out.shape[axis]
        acc = np.zeros_like(out)
        for i, w in enumerate(k):
            acc += w * p[tuple(slice(i, i + n) if j == axis else slice(None) for j in range(out.ndim))]
        out = acc
    return out


def regions(alpha):
    import numpy as np
    hull = alpha > 0.5
    d2 = dilate(hull, 2)
    return {'interior': erode(alpha > 0.99, 4), 'sil': d2 & ~erode(hull, 2), 'wing': dilate(hull, 14) & ~d2}


# ---- the manifest and the files ------------------------------------------------------------------------------------------

def num(v):
    """A manifest number: a scalar, a list (summed), or a dict ('median' if present, else its numbers summed)."""
    if isinstance(v, bool) or v is None:
        return None
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, dict):
        if 'median' in v:
            return num(v['median'])
        vals = [num(x) for x in v.values()]
    elif isinstance(v, (list, tuple)):
        vals = [num(x) for x in v]
    else:
        return None
    vals = [x for x in vals if x is not None]
    return sum(vals) if vals else None


def load_manifest(prefix):
    path = prefix + '_Bench.json'
    if not os.path.isfile(path):
        return None
    try:
        with open(path, encoding='utf-8') as f:
            m = json.load(f)
    except (OSError, ValueError) as e:
        raise BadInput('%s does not parse: %s' % (path, e))
    if not isinstance(m, dict):
        raise BadInput('%s is not a JSON object' % path)
    return m


def capture_files(prefix, vid, entry, eye):
    d = os.path.dirname(prefix)
    if eye == 1:
        paths = [prefix + '_Bench_%s_f00_e1.bin' % vid]
    elif entry and entry.get('frames'):
        paths = [os.path.join(d, os.path.basename(f['file'])) for f in entry['frames'] if isinstance(f, dict) and f.get('file')]
    else:
        paths = sorted(p for p in glob.glob(glob.escape(prefix) + '_Bench_%s_f[0-9][0-9].bin' % vid))
    return [p for p in paths if os.path.isfile(p)]


def final_file(prefix, vid, entry, eye):
    ff = (entry or {}).get('final_files') or []
    if len(ff) > eye and ff[eye]:
        p = os.path.join(os.path.dirname(prefix), os.path.basename(ff[eye]))
    else:
        p = prefix + '_Bench_%s_final%s.bin' % (vid, '_e1' if eye else '')
    return p if os.path.isfile(p) else None


def frame_gap(entry):
    lf = [f.get('layer_frame') for f in (entry or {}).get('frames') or [] if isinstance(f, dict)]
    lf = [x for x in lf if isinstance(x, int)]
    return any(b - a != 1 for a, b in zip(lf, lf[1:]))


# ---- measuring -----------------------------------------------------------------------------------------------------------

def measure(paths, shape, reg):
    """Accumulate one variant's frames: temporal mean/std of luma, the edge residual, the mean RGB on the interior."""
    import numpy as np
    n = 0
    for p in paths:
        _, img = read_tex(p)
        c = rgb_of(img)
        if c.shape[:2] != shape:
            raise BadInput('%s is %dx%d, the mask %dx%d' % (os.path.basename(p), c.shape[1], c.shape[0], shape[1], shape[0]))
        L = luma(c).astype(np.float64)
        r = L - gauss(L, 1.0)
        if n == 0:
            frame0, s, ss, rs, rss, ra = c, L.copy(), L * L, r.copy(), r * r, np.abs(r)
            rgb_int = c[reg['interior']].astype(np.float64)
        else:
            s += L
            ss += L * L
            rs += r
            rss += r * r
            ra += np.abs(r)
            rgb_int += c[reg['interior']]
        n += 1
    mean = s / n
    std = np.sqrt(np.maximum(ss / n - mean * mean, 0.0))
    rstd = np.sqrt(np.maximum(rss / n - (rs / n) ** 2, 0.0))
    return {'n': n, 'frame0': frame0, 'Lm': mean.astype(np.float32), 'std': std.astype(np.float32), 'rabs': (ra / n).astype(np.float32),
            'rstd': rstd.astype(np.float32), 'rgb_int': (rgb_int / n).astype(np.float32)}


def region_mean(a, m):
    import numpy as np
    return float(np.mean(a[m])) if m.any() else None


def best_window(score, size):
    """(x, y, w, h) of the size x size window with the largest sum of score."""
    import numpy as np
    h, w = score.shape
    sw, sh = min(size, w), min(size, h)
    ii = np.zeros((h + 1, w + 1))
    ii[1:, 1:] = score.astype(np.float64).cumsum(0).cumsum(1)
    step = max(1, size // 16)
    ys = np.unique(np.append(np.arange(0, h - sh + 1, step), h - sh))
    xs = np.unique(np.append(np.arange(0, w - sw + 1, step), w - sw))
    tot = ii[ys[:, None] + sh, xs[None, :] + sw] - ii[ys[:, None], xs[None, :] + sw] - ii[ys[:, None] + sh, xs[None, :]] + ii[ys[:, None], xs[None, :]]
    j, i = np.unravel_index(int(np.argmax(tot)), tot.shape)
    return int(xs[i]), int(ys[j]), sw, sh


# ---- images --------------------------------------------------------------------------------------------------------------

def png_uri(img):
    """A data: URI of an 8-bit RGB PNG of img (float, [0, 1]; grey when 2-D)."""
    import numpy as np
    a = np.asarray(img, np.float32)
    if a.ndim == 2:
        a = np.repeat(a[..., None], 3, axis=2)
    a = np.round(np.clip(np.nan_to_num(a[..., :3]), 0.0, 1.0) * 255.0).astype(np.uint8)
    h, w = a.shape[:2]
    raw = b''.join(b'\x00' + a[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6))
           + chunk(b'IEND', b''))
    return 'data:image/png;base64,' + base64.b64encode(png).decode('ascii')


def downscale(img, k):
    if k <= 1:
        return img
    h, w = img.shape[0] // k * k, img.shape[1] // k * k
    if not h or not w:
        return img
    a = img[:h, :w]
    return a.reshape((h // k, k, w // k, k) + a.shape[2:]).mean(axis=(1, 3))


def zooms(c, win):
    import numpy as np
    x, y, w, h = win
    one = c[y:y + h, x:x + w]
    hx, hy = w // 4, h // 4
    two = np.repeat(np.repeat(one[hy:hy + h // 2, hx:hx + w // 2], 2, axis=0), 2, axis=1)
    return one, two


# ---- the analysis --------------------------------------------------------------------------------------------------------

def analyse(prefix, eye=0):
    import numpy as np
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    manifest = load_manifest(prefix)
    mask_path = prefix + ('_Bench_Mask_e1.bin' if eye else '_Bench_Mask.bin')
    if not os.path.isfile(mask_path):
        raise BadInput('no %s (the region source)' % os.path.basename(mask_path))
    _, mimg = read_tex(mask_path)
    if mimg.ndim != 3 or mimg.shape[2] < 4:
        raise BadInput('%s has no alpha' % os.path.basename(mask_path))
    alpha = np.nan_to_num(mimg[..., 3].astype(np.float32))
    shape = alpha.shape
    reg = regions(alpha)
    if not reg['interior'].any() or not reg['sil'].any():
        raise BadInput('the mask shows no hull')

    entries = {}
    for v in (manifest or {}).get('variants') or []:
        if isinstance(v, dict) and v.get('id'):
            entries[v['id']] = v
    order = list((manifest or {}).get('order') or ORDER)
    order += [v for v in entries if v not in order]

    rows, stats = [], {}
    # orig_a first (dluma/drgb, the diff images and the zoom windows need it), then the bench's order.
    todo = sorted(order, key=lambda v: v != 'orig_a')
    windows, base = None, None
    for vid in todo:
        e = entries.get(vid)
        row = {'id': vid, 'status': (e or {}).get('status') or ('ok' if manifest is None else 'not captured'),
               'reason': (e or {}).get('reason') or ''}
        g, mb = (e or {}).get('gpu_ms') or {}, (e or {}).get('mb') or {}
        row['layer_ms'] = num(g.get('layer_stage'))
        ls = g.get('layer_stage')
        row['stages'] = [num(x) for x in ls] if isinstance(ls, (list, tuple)) and len(ls) == len(STAGES) else None
        row['hull_dlss'] = num(g.get('hull_dlss'))
        row['main_dlss'] = num(g.get('main_dlss'))
        row['frame_ms'] = num(g.get('frame_span'))
        parts = [x for x in (row['layer_ms'], row['hull_dlss']) if x is not None]
        row['var_ms'] = sum(parts) if parts else None
        row['mb_own'], row['mb_ngx'] = num(mb.get('own')), num(mb.get('ngx'))
        rots = [num(f.get('head_rot_deg')) for f in (e or {}).get('frames') or [] if isinstance(f, dict)]
        rots = [r for r in rots if r is not None]
        row['still'] = max(rots) if rots else None
        rows.append(row)
        paths = capture_files(prefix, vid, e, eye)
        if row['status'] == 'refused':
            continue
        if not paths:
            if row['status'] == 'ok':
                row['status'] = 'not captured'
            continue
        if eye == 0 and frame_gap(e):
            row['status'], row['reason'] = 'rejected', 'the capture frames are not consecutive (layer_frame gap)'
            continue
        try:
            st = measure(paths, shape, reg)
        except (OSError, ValueError) as ex:
            if isinstance(ex, BadInput):
                raise
            row['status'], row['reason'] = 'unreadable', str(ex)
            continue
        if windows is None:
            base = vid
            Lm = st['Lm']
            lap = np.zeros_like(Lm)
            lap[1:-1, 1:-1] = np.abs(4 * Lm[1:-1, 1:-1] - Lm[:-2, 1:-1] - Lm[2:, 1:-1] - Lm[1:-1, :-2] - Lm[1:-1, 2:])
            windows = {'edge': best_window(reg['sil'], ZOOM), 'detail': best_window(np.where(reg['interior'], lap, 0), ZOOM)}
        stats[vid] = st
        c = st['frame0']
        k = max(2, int(math.ceil(shape[1] / float(OVERVIEW_MAX))))
        row['img'] = {'overview': png_uri(downscale(c, k))}
        for name, win in windows.items():
            one, two = zooms(c, win)
            row['img'][name + ' 1:1'] = png_uri(one)
            row['img'][name + ' 2x'] = png_uri(two)
        if 'orig_a' in stats:
            row['img']['|dluma| x4 vs orig_a'] = png_uri(downscale(np.abs(st['Lm'] - stats['orig_a']['Lm']) * 4.0, k))
        fp = final_file(prefix, vid, e, eye)
        if fp:
            try:
                fc = rgb_of(read_tex(fp)[1])
                row['img']['final (headset)'] = png_uri(downscale(fc, max(2, int(math.ceil(fc.shape[1] / float(OVERVIEW_MAX))))))
            except (OSError, ValueError) as ex:
                row['reason'] = (row['reason'] + '; ' if row['reason'] else '') + 'final unreadable: %s' % ex
        st.pop('frame0')
        row['frames'] = st['n']
        # The per-variant image metrics.
        T = st['n']
        row['shim_int'] = region_mean(st['std'], reg['interior']) * 255 if T > 1 else None
        row['shim_sil'] = region_mean(st['std'], reg['sil']) * 255 if T > 1 else None
        contrast = region_mean(_run(_run(st['Lm'], 2, 0, np.maximum), 2, 1, np.maximum)
                               - _run(_run(st['Lm'], 2, 0, np.minimum), 2, 1, np.minimum), reg['sil'])
        contrast = max(contrast or 0.0, 1e-6)
        row['stair'] = region_mean(st['rabs'], reg['sil']) / contrast
        row['crawl'] = region_mean(st['rstd'], reg['sil']) / contrast if T > 1 else None
        row['hf'] = hf_energy(st['Lm'], reg['interior'])
        if 'orig_a' in stats:
            a = stats['orig_a']
            la = float(np.mean(a['Lm'][reg['interior']]))
            row['dluma'] = float(np.mean(st['Lm'][reg['interior']])) / la if la > 1e-9 else None
            row['drgb'] = float(np.mean(np.abs(st['rgb_int'] - a['rgb_int']))) * 255
        del st['rabs'], st['rstd']
        if vid != 'orig_a':
            del st['rgb_int']
    if not stats:
        raise BadInput('no variant of %s was captured' % os.path.basename(prefix))
    rows.sort(key=lambda r: order.index(r['id']))

    # Across variants: the sharpness against sep_taa's, the smear against the hull-free world.
    ref_hf = next((r['hf'] for r in rows if r['id'] == 'sep_taa' and r.get('hf')), None)
    refs = [stats[v]['Lm'] for v in HULL_FREE if v in stats] or ([stats['orig_a']['Lm']] if 'orig_a' in stats else [])
    R = np.median(np.stack(refs), axis=0) if refs else None
    wing = reg['wing']
    for r in rows:
        if r['id'] not in stats:
            continue
        if ref_hf:
            r['sharp'] = r['hf'] / ref_hf
        if R is not None and wing.any():
            Lm = stats[r['id']]['Lm']
            spread = float(np.mean(np.abs(R[wing] - np.mean(R[wing]))))
            r['smear'] = float(np.mean(np.abs(Lm[wing] - R[wing]))) / max(spread, 1e-6)
            rh = hf_energy(R, wing)
            r['band_hf'] = hf_energy(Lm, wing) / rh if rh > 0 else None

    still = {'max_rot': max([r['still'] for r in rows if r.get('still') is not None] or [None], key=lambda x: -1 if x is None else x),
             'diff': None, 'noise': None, 'moved': False}
    if 'orig_a' in stats and 'orig_b' in stats:
        a, b = stats['orig_a'], stats['orig_b']
        m = reg['interior'] | reg['sil'] | wing
        T = min(a['n'], b['n'])
        still['diff'] = float(np.mean(np.abs(a['Lm'][m] - b['Lm'][m]))) * 255
        still['noise'] = float(np.mean((a['std'][m] + b['std'][m]) * 0.5)) * 255 / math.sqrt(max(T, 1))
        still['moved'] = still['diff'] > max(MOVED_MIN, MOVED_SIGMAS * still['noise'])
    return {'prefix': prefix, 'eye': eye, 'manifest': manifest, 'rows': rows, 'still': still, 'shape': shape, 'base': base,
            'windows': windows, 'region_px': {k: int(v.sum()) for k, v in reg.items()}, 'ref': [v for v in HULL_FREE if v in stats] or ['orig_a']}


# ---- output --------------------------------------------------------------------------------------------------------------

def fmt(v, digits):
    return '-' if v is None else '%.*f' % (digits, v)


def best_of(rows):
    """Per column the best measured non-original row ids (the originals win ms, MB and difference trivially)."""
    best = {}
    for key, _, better, digits in COLUMNS:
        vals = [(r[key], r['id']) for r in rows if r.get(key) is not None and not r['id'].startswith('orig')]
        if not vals:
            continue
        score = {'low': lambda x: x, 'high': lambda x: -x, 'one': lambda x: abs(x - 1.0)}[better]
        top = min(score(v) for v, _ in vals)
        best[key] = {i for v, i in vals if abs(score(v) - top) <= 10 ** -digits / 2}
    return best


def still_line(s):
    if s['diff'] is None:
        tail = 'orig_a/orig_b not both captured: stillness unknown'
    else:
        tail = 'orig_a vs orig_b %.2f (noise %.2f, 8-bit): %s' % (s['diff'], s['noise'], 'PILOT MOVED, the press is unreliable' if s['moved'] else 'still')
    rot = 'max head rotation %s deg/frame' % fmt(s['max_rot'], 3)
    return 'stillness: %s; %s' % (rot, tail)


def stage_lines(res):
    """The stage split of every row that has one, then each A/B variant against sep_taa (differences, '-' when unmeasured)."""
    out = []
    rows = {r['id']: r for r in res['rows']}
    split = [r for r in res['rows'] if any(x is not None for x in r.get('stages') or []) and not r['id'].startswith('orig')]
    if split:
        out.append('stages ms (%s):' % '/'.join(STAGES))
        out += ['  %s %s' % (r['id'], '/'.join(fmt(x, 3) for x in r['stages'])) for r in split]
    base = rows.get('sep_taa')
    for vid in AB:
        r = rows.get(vid)
        if not base or not r:
            continue

        def diff(a, b, digits):
            return '-' if a is None or b is None else '%+.*f' % (digits, a - b)
        st = ', '.join('%s %s' % (n, diff(a, b, 3)) for n, a, b in zip(STAGES, r.get('stages') or [None] * 6, base.get('stages') or [None] * 6))
        out.append('A/B %s - sep_taa: dluma %s layer_ms %s (%s)%s' % (vid, diff(r.get('dluma'), base.get('dluma'), 3),
                                                                       diff(r.get('layer_ms'), base.get('layer_ms'), 3), st,
                                                                       '; ' + r['reason'] if r.get('reason') else ''))
    return out


def table_text(res):
    head = ['variant', 'status'] + [next(c[1] for c in COLUMNS if c[0] == k) for k in STDOUT_COLS]
    lines = [[r['id'], r['status']] + [fmt(r.get(k), next(c[3] for c in COLUMNS if c[0] == k)) for k in STDOUT_COLS] for r in res['rows']]
    widths = [max(len(x[i]) for x in [head] + lines) for i in range(len(head))]
    out = ['  '.join(h.ljust(w) for h, w in zip(head, widths))]
    out += ['  '.join(x.ljust(w) for x, w in zip(line, widths)) for line in lines]
    out += ['  %s: %s' % (r['id'], r['reason']) for r in res['rows'] if r.get('reason')]
    out += stage_lines(res)
    out.append(still_line(res['still']))
    return '\n'.join(out)


def esc(s):
    return str(s).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')


def html(res):
    m = res['manifest'] or {}
    best = best_of(res['rows'])
    name = os.path.basename(res['prefix'])
    p = ['<!doctype html><html><head><meta charset="utf-8"><title>Hull bench %s</title><style>' % esc(name),
         ':root{--bg:#fff;--fg:#111;--mute:#666;--line:#ccc;--warn:#b00}'
         '@media (prefers-color-scheme:dark){:root{--bg:#161616;--fg:#eee;--mute:#999;--line:#444;--warn:#f66}}'
         'body{background:var(--bg);color:var(--fg);font:14px/1.4 system-ui,sans-serif;margin:16px}'
         'table{border-collapse:collapse;font-variant-numeric:tabular-nums}td,th{border:1px solid var(--line);padding:3px 6px;text-align:right}'
         'td:first-child,td:nth-child(2),th{text-align:left}.mute{color:var(--mute)}.warn{color:var(--warn);font-weight:bold}'
         '.row{display:flex;gap:8px;overflow-x:auto;padding-bottom:8px}figure{margin:0}figcaption{font-size:12px;color:var(--mute)}'
         'img.px{image-rendering:pixelated}.nc{width:256px;height:64px;border:1px dashed var(--line);display:flex;align-items:center;'
         'justify-content:center;color:var(--mute)}</style></head><body>',
         '<h1>Hull bench %s, eye %d</h1>' % (esc(name), res['eye'])]
    info = ['build %s' % m.get('build', '?'), 'render %s' % m.get('render', '?'), 'output %s' % m.get('output', '?'),
            'rect_output %s' % m.get('rect_output', '?'), 'dlss %s' % m.get('dlss', '?'), 'evicted %s' % m.get('evicted', '?'),
            'overlay_in_final %s' % m.get('overlay_in_final', '?'), 'crop %dx%d' % (res['shape'][1], res['shape'][0]),
            'regions (px) %s' % res['region_px'], 'smear reference %s' % '/'.join(res['ref'])]
    if res['manifest'] is None:
        info.insert(0, 'no Bench.json manifest: variants found by file name, no ms/MB')
    p.append('<p class="mute">%s</p>' % esc('; '.join(info)))
    if m.get('aborted'):
        p.append('<p class="warn">The bench aborted: %s</p>' % esc(m['aborted']))
    p.append('<p class="%s">%s</p>' % ('warn' if res['still']['moved'] else 'mute', esc(still_line(res['still']))))
    p.append('<table><tr><th>variant</th><th>status</th>%s</tr>' % ''.join('<th>%s</th>' % esc(c[1]) for c in COLUMNS))
    for r in res['rows']:
        cells = []
        for key, _, _, digits in COLUMNS:
            v = fmt(r.get(key), digits)
            cells.append('<td>%s</td>' % ('<b>%s</b>' % v if r['id'] in best.get(key, ()) else v))
        status = r['status'] + (': ' + r['reason'] if r.get('reason') else '')
        p.append('<tr><td>%s</td><td>%s</td>%s</tr>' % (esc(r['id']), esc(status), ''.join(cells)))
    p.append('</table><p class="mute">Bold: the best measured variant per column, the originals left out. shimmer and drgb in 8-bit units; '
             'sharp / sep_taa; stair and crawl / local contrast; smear / the reference\'s spread; band hf and dluma best at 1; still in '
             'deg/frame. Definitions in tools\\hull_bench_report.py.</p>')
    sl = stage_lines(res)
    if sl:
        p.append('<pre>%s</pre>' % esc('\n'.join(sl)))
    views = []
    for r in res['rows']:
        for v in r.get('img', {}):
            if v not in views:
                views.append(v)
    for v in views:
        win = ''
        for wname, w in (res['windows'] or {}).items():
            if v.startswith(wname + ' '):
                win = ' (window x %d y %d, %dx%d%s)' % (w[0], w[1], w[2], w[3], ', centre half at 2x' if v.endswith('2x') else '')
        p.append('<h2>%s%s</h2><div class="row">' % (esc(v), esc(win)))
        for r in res['rows']:
            uri = r.get('img', {}).get(v)
            body = ('<img class="%s" src="%s" alt="%s %s">' % ('px' if ('1:1' in v or '2x' in v) else '', uri, esc(r['id']), esc(v)) if uri
                    else '<div class="nc">%s</div>' % esc(r['status'] if r['status'] != 'ok' else 'none'))
            p.append('<figure>%s<figcaption>%s</figcaption></figure>' % (body, esc(r['id'])))
        p.append('</div>')
    p.append('</body></html>')
    return '\n'.join(p)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('prefix', nargs='?', help='the bench prefix: <edvr_logs>\\eyes\\eye_<stamp>')
    ap.add_argument('--out', help='the HTML to write (default <prefix>_Bench.html, or _Bench_e1.html for --eye 1)')
    ap.add_argument('--eye', type=int, choices=(0, 1), default=0, help='eye 0 (8 frames, default) or eye 1 (frame 0 only: no temporal metrics)')
    ap.add_argument('--expect-build', help='exit 2 unless the manifest\'s build is this git ref or version')
    ap.add_argument('--dry-run', action='store_true', help='print the table and write nothing')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.prefix:
        ap.error('give the bench prefix, or --self-test')
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import numpy  # noqa: F401
    except ImportError:
        print('hull bench report: NumPy is not installed')
        return 2
    prefix = a.prefix[:-len('_Bench.json')] if a.prefix.endswith('_Bench.json') else a.prefix
    try:
        res = analyse(prefix, a.eye)
    except BadInput as e:
        print('hull bench report: %s' % e)
        return 2
    if a.expect_build:
        import edvr_log
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        want = edvr_log.expected_version(a.expect_build, root)
        got = (res['manifest'] or {}).get('build')
        if not edvr_log.version_matches(got, want):
            print('hull bench report: BUILD MISMATCH: the bench is %s, expected %s' % (got, want))
            return 2
    print(table_text(res))
    out = a.out or prefix + ('_Bench_e1.html' if a.eye else '_Bench.html')
    if a.dry_run:
        print('would write %s (dry run: not written)' % out)
    else:
        with open(out, 'w', encoding='utf-8') as f:
            f.write(html(res))
        print('wrote %s' % out)
    return 3 if res['still']['moved'] else 0


# ---- self-test -----------------------------------------------------------------------------------------------------------

def _write_tex(path, arr, fmt=28, eye=0, frame=0):
    import numpy as np
    h, w = arr.shape[:2]
    if fmt == 28:
        data = np.round(np.clip(arr, 0, 1) * 255).astype(np.uint8)
        px = 4
    else:  # 10: R16G16B16A16_FLOAT
        data = arr.astype(np.float16)
        px = 8
    with open(path, 'wb') as f:
        f.write(b'EDVRTEX1' + struct.pack('<9I', 1, w, h, fmt, w * px, frame, eye, 0, 0) + np.ascontiguousarray(data).tobytes())


def synthetic(prefix, moved=False, drop=()):
    """A sharp disk hull with panel stripes over a still textured terrain, 8 frames a variant, eye 1 frame 0, a mask, a manifest.
    orig_a/orig_b/sep_taa: supersampled (anti-aliased); sep_noaa: one jittered point sample a pixel (aliased, crawling);
    dlss_full: per-frame noise in the interior (shimmer); dlss_sub: the hull blurred; native: the terrain smeared;
    native_fxaa: refused. moved: orig_b's whole image 2 px aside. drop: variants the manifest calls ok but with no files."""
    import numpy as np
    H = W = 72
    cx, cy, R, T = 36.0, 36.0, 20.0, 8
    rng = np.random.default_rng(7)
    terrain = gauss(rng.random((H, W, 3)).astype(np.float32), 1.0)
    terrain = 0.15 + 0.5 * (terrain - terrain.min()) / (terrain.max() - terrain.min())
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)

    def scene(jx=0.0, jy=0.0, ss=4, cxo=cx):
        offs = (np.arange(ss) + 0.5) / ss if ss > 1 else np.array([0.5])
        a, hc = np.zeros((H, W)), np.zeros((H, W, 3))
        for oy in offs:
            for ox in offs:
                x, y = xx + ox + jx, yy + oy + jy
                inside = (x - cxo) ** 2 + (y - cy) ** 2 < R * R
                t = 0.55 + 0.2 * np.sin(0.3 * x + 0.2 * y) + 0.1 * np.sin(0.5 * y)
                a += inside
                hc += inside[..., None] * np.stack([t, 0.9 * t, 0.8 * t], -1)
        return a / len(offs) ** 2, hc / len(offs) ** 2

    def halton(i, b):
        f, r = 1.0, 0.0
        while i:
            f /= b
            r += f * (i % b)
            i //= b
        return r
    a, hc = scene()
    aa = hc + (1 - a)[..., None] * terrain
    deep = (xx + 0.5 - cx) ** 2 + (yy + 0.5 - cy) ** 2 < (R - 6) ** 2
    frames = {'orig_a': [aa] * T, 'sep_taa': [aa] * T, 'orig_b': [np.roll(aa, 2, axis=(0, 1)) if moved else aa] * T,
              'sep_noaa': [], 'dlss_full': [], 'native': [hc + (1 - a)[..., None] * gauss(terrain, 2.5)] * T}
    for k in range(T):
        aj, hj = scene(halton(k + 1, 2) - 0.5, halton(k + 1, 3) - 0.5, ss=1)
        frames['sep_noaa'].append(hj + (1 - aj)[..., None] * terrain)
        frames['dlss_full'].append(aa + np.where(deep, rng.normal(0, 0.08, (H, W)), 0)[..., None])
    frames['dlss_sub'] = [gauss(hc, 2.0) + (1 - gauss(a, 2.0))[..., None] * terrain] * T
    # The A/B variants: sep_norefl and sep_nopre as sep_taa; sep_norect's hull 10% darker (a dluma the A/B line must show).
    frames['sep_norefl'] = frames['sep_nopre'] = [aa] * T
    frames['sep_norect'] = [0.9 * hc + (1 - a)[..., None] * terrain] * T
    mask = np.concatenate([hc, a[..., None]], axis=-1)
    _write_tex(prefix + '_Bench_Mask.bin', mask, fmt=10)
    _write_tex(prefix + '_Bench_Mask_e1.bin', mask, fmt=10, eye=1)
    variants = []
    for i, vid in enumerate(ORDER):
        if vid not in frames:
            variants.append({'id': vid, 'status': 'refused', 'reason': 'not built'})
            continue
        fl = []
        for k, img in enumerate(frames[vid]):
            name = os.path.basename(prefix) + '_Bench_%s_f%02d.bin' % (vid, k)
            if vid not in drop:
                rgba = np.concatenate([img, np.ones((H, W, 1))], axis=-1)
                _write_tex(os.path.join(os.path.dirname(prefix), name), rgba, frame=1000 + k)
                if k == 0:
                    _write_tex(prefix + '_Bench_%s_f00_e1.bin' % vid, rgba, eye=1, frame=1000)
                    if vid == 'sep_taa':
                        _write_tex(prefix + '_Bench_%s_final.bin' % vid, rgba, frame=1000)
            fl.append({'index': k, 'layer_frame': 500 + 100 * i + k, 'file': name, 'jitter': [0, 0], 'head_rot_deg': 0.01 * (i + 1),
                       'tanNow': [-1, 1, 1, -1]})
        sep = not vid.startswith('orig')
        reason = {'sep_norefl': 'feature off 32 times in the capture',
                  'sep_nopre': 'NOT SWITCHED OFF: the feature never ran in the capture (this row is sep_taa)'}.get(vid, '')
        gbuf = 0.3 if vid == 'sep_norect' else 0.1   # sep_norect: the whole eye shaded, a dearer G-buffer stage
        variants.append({'id': vid, 'status': 'ok', 'reason': reason, 'warm_frames': 60, 'fallback_frames': 0, 'frames': fl,
                         'final_files': [os.path.basename(prefix) + '_Bench_%s_final.bin' % vid, ''],
                         'gpu_ms': {'layer_stage': [gbuf * sep, 0.05 * sep, 0.2 * sep, 0.1 * sep, 0.02 * sep, 0.3 * sep],
                                    'hull_dlss': 0.9 if vid.startswith('dlss') else 0.0, 'main_dlss': 1.5, 'frame_span': 9.0 + i, 'cpu_ms': 0.1},
                         'mb': {'own': 40.0 * sep + i, 'ngx': 120.0 if vid.startswith('dlss') else 0.0, 'dxgi': 9000.0}})
    js = {'schema': 1, 'stamp': os.path.basename(prefix)[4:], 'build': 'v0.30.0-5-gabcdef1', 'render': [54, 54], 'output': [72, 72],
          'rect_render': [0, 0, 54, 54], 'rect_output': [0, 0, 72, 72], 'dlss': {'preset': 'K', 'quality': 'Quality', 'version': '310.9.1'},
          'fg': False, 'order': list(ORDER), 'aborted': None, 'evicted': False, 'overlay_in_final': False, 'variants': variants}
    with open(prefix + '_Bench.json', 'w', encoding='utf-8') as f:
        json.dump(js, f)


def self_test():
    import contextlib
    import io
    import tempfile
    try:
        import numpy  # noqa: F401
    except ImportError:
        print('hull bench report self-test skipped: NumPy is not installed')
        return 0
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    failures = []

    def expect(name, cond, what=''):
        if not cond:
            failures.append('%s: %s' % (name, what))

    def run(argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = main(argv)
        return code, out.getvalue()

    def worst(rows, key, low=True):
        vals = [(r[key], r['id']) for r in rows if r.get(key) is not None]
        return (max(vals) if low else min(vals))[1] if vals else None

    with tempfile.TemporaryDirectory() as temp:
        prefix = os.path.join(temp, 'eye_202611090001')
        synthetic(prefix)
        res = analyse(prefix)
        rows = {r['id']: r for r in res['rows']}
        expect('order', [r['id'] for r in res['rows']] == list(ORDER), [r['id'] for r in res['rows']])
        expect('shimmer', worst(res['rows'], 'shim_int') == 'dlss_full', 'the noisy variant is not the worst interior shimmer: %r'
               % {k: r.get('shim_int') for k, r in rows.items()})
        expect('stair', worst(res['rows'], 'stair') == 'sep_noaa', 'the aliased variant is not the worst stair: %r'
               % {k: r.get('stair') for k, r in rows.items()})
        expect('crawl', worst(res['rows'], 'crawl') == 'sep_noaa', 'the jittered aliased edge is not the worst crawl: %r'
               % {k: r.get('crawl') for k, r in rows.items()})
        expect('sharp', worst(res['rows'], 'sharp', low=False) == 'dlss_sub' and abs(rows['sep_taa']['sharp'] - 1) < 1e-9,
               'the blurred variant is not the least sharp: %r' % {k: r.get('sharp') for k, r in rows.items()})
        expect('smear', worst(res['rows'], 'smear') == 'native' and rows['native']['band_hf'] < 0.8,
               'the smeared terrain is not the worst smear: %r' % {k: (r.get('smear'), r.get('band_hf')) for k, r in rows.items()})
        expect('drgb', rows['orig_b']['drgb'] < 0.5 and rows['dlss_sub']['drgb'] > 1.0 and abs(rows['orig_b']['dluma'] - 1) < 1e-3,
               'drgb orig_b %r dlss_sub %r' % (rows['orig_b'].get('drgb'), rows['dlss_sub'].get('drgb')))
        expect('still', not res['still']['moved'] and abs(res['still']['max_rot'] - 0.01 * len(ORDER)) < 1e-9, res['still'])
        # The A/B variants against sep_taa: sep_norect's darker hull and dearer G-buffer, sep_nopre's not-switched-off reason.
        ab = '\n'.join(stage_lines(res))
        expect('a/b', rows['sep_norect']['dluma'] < 0.95 and abs(rows['sep_norefl']['dluma'] - 1) < 1e-3
               and re.search(r'^A/B sep_norect - sep_taa: dluma -0\.\d+ layer_ms \+0\.200 \(gbuffer \+0\.200, mask_filter \+0\.000', ab, re.M)
               and re.search(r'^A/B sep_nopre - sep_taa: .*NOT SWITCHED OFF', ab, re.M) and re.search(r'^  sep_taa 0\.100/0\.050/', ab, re.M), ab)
        expect('refused', rows['native_fxaa']['status'] == 'refused' and 'img' not in rows['native_fxaa'], rows['native_fxaa'])
        # The manifest round trip: ms and MB as written.
        expect('manifest', abs(rows['sep_taa']['layer_ms'] - 0.77) < 1e-9 and abs(rows['dlss_full']['var_ms'] - 1.67) < 1e-9
               and rows['dlss_full']['mb_ngx'] == 120.0 and rows['sep_taa']['frame_ms'] == 10.0, rows['dlss_full'])
        before = sorted(os.listdir(temp))
        code, said = run([prefix, '--dry-run'])
        expect('dry-run', code == 0 and sorted(os.listdir(temp)) == before and 'not written' in said, '%r %r' % (code, said))
        code, said = run([prefix])
        out = prefix + '_Bench.html'
        page = open(out, encoding='utf-8').read() if os.path.isfile(out) else ''
        expect('html', code == 0 and '<table>' in page and 'data:image/png;base64,' in page and 'v0.30.0-5-gabcdef1' in page
               and 'final (headset)' in page and 'edge 2x' in page, '%r %d bytes' % (code, len(page)))
        expect('stdout', 'shimmer int' in said and re.search(r'^native_fxaa +refused', said, re.M) and 'stillness:' in said, said)
        code, said = run([prefix, '--eye', '1', '--dry-run'])
        expect('eye 1', code == 0 and 'sep_taa' in said, '%r %r' % (code, said))
        code, said = run([prefix, '--dry-run', '--expect-build', 'v0.29.0-1-g1234567'])
        expect('expect-build', code == 2 and 'BUILD MISMATCH' in said, '%r %r' % (code, said))
        code, said = run([prefix, '--dry-run', '--expect-build', 'v0.30.0-5-gabcdef1'])
        expect('expect-build match', code == 0, '%r %r' % (code, said))
    with tempfile.TemporaryDirectory() as temp:
        # The pilot moved between orig_a and orig_b; a variant the manifest calls ok has no files; no manifest at all.
        prefix = os.path.join(temp, 'eye_202611090002')
        synthetic(prefix, moved=True, drop=('dlss_full',))
        code, said = run([prefix, '--out', os.path.join(temp, 'r.html')])
        expect('moved', code == 3 and 'PILOT MOVED' in said, '%r %r' % (code, said))
        expect('missing', re.search(r'^dlss_full +not captured', said, re.M) and os.path.isfile(os.path.join(temp, 'r.html')), said)
        os.remove(prefix + '_Bench.json')
        res = analyse(prefix)
        expect('no manifest', [r['status'] for r in res['rows'] if r['id'] in ('dlss_full', 'native_fxaa', 'sep_taa')]
               == ['ok', 'not captured', 'not captured'] and res['rows'][1]['id'] == 'sep_taa',
               [(r['id'], r['status']) for r in res['rows']])
        os.remove(prefix + '_Bench_Mask.bin')
        code, said = run([prefix, '--dry-run'])
        expect('no mask', code == 2 and 'Bench_Mask' in said, '%r %r' % (code, said))
    for f in failures:
        print('hull bench report self-test FAILED: ' + f)
    if not failures:
        print('hull bench report self-test passed (21 checks)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    sys.exit(main())
