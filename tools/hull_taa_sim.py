#!/usr/bin/env python3
"""Reproduce the hull layer's TAA offline from a dump's TAA sequence (diag.hull_layer = separate, TEMPORARY; docs/research-own-hull-layer-2026-11-08.md "Dump v2").

Usage: python tools/hull_taa_sim.py <edvr_logs\\eyes\\eye_<stamp>> [--current 0.1] [--interior-scale 0.5] [--kernel 3.5]
           [--clip-interior 3.0] [--clip-edge 1.25] [--sharpen 0.3] [--shader 1|2] [--last 8] [--compare]
       python tools/hull_taa_sim.py --self-test

Reads eye_<stamp>_HullSeq.json and, per frame nn, eye_<stamp>_HullSeq_<nn>_Cover.bin (the layer's cover at render size: the
tonemapped hull where shown, coverage in alpha, R8G8B8A8), _Depth.bin (the cover's depth, R32_FLOAT) and _Comp.bin (the composite
the GPU wrote, output size), each cropped to the hull (outside a crop the cover, depth and history are zero), and
_HullSeq_00_Hist.bin, the history frame 0's TAA read (R16G16B16A16_FLOAT, absent when frame 0 had none). The frames are eye 0's,
one after another from the dump's capture frame; a gap ends the sequence.

The TAA is src\\d3d11\\hull_layer.cpp's kTaaHlsl, transcribed: this frame's cover over the 3x3 around the output pixel's
unjittered render position, weighted exp(-kernel d^2); the history reprojected by the head's delta (and translation at the 3x3's
nearest depth), read through the 9-tap Catmull-Rom; clamped to mean +- clip_interior sigma where the whole 3x3 is hull (and the
current weight times interior_scale), else to the min/max box inside mean +- clip_edge sigma; then kSharpHlsl's cross unsharp
mask where the whole cross is hull. The history is kept as float16, as the GPU keeps it. Every parameter is a flag; the defaults
are this build's (the self-test checks them against hull_layer.cpp).

--shader picks the TAA's version, so --compare fits the build that made the dump:
  2 (default): builds after b85d728c (2026-11-12). At the silhouette a history whose coverage lies within the 3x3's alpha range
     +- 0.05 is taken unclamped (saturated) at the interior's current weight; sharpen 0.3.
  1: builds up to b85d728c. The silhouette always clipped to mean +- clip_edge sigma; sharpen 0.6.

Reports, on the hull interior (coverage 1):
  * shimmer: the per-pixel standard deviation of luma over the last --last frames, in 8-bit units (mean and p95), for the
    simulation and for the GPU's composites;
  * sharpness: the high-frequency (Laplacian) energy of the last frame against the bicubic upscale of that frame's cover alone;
  * with --compare, the difference between the simulation and the GPU's composite per frame (mean, p99, max, 8-bit units).

Exit: 0 (with --compare: MATCH, mean at most 1 and p99 at most 4 on every frame), 3 MISMATCH, 1 INCONCLUSIVE (frame 0 read a
history the dump does not hold, so the simulation cannot start where the GPU did), 2 bad input.
"""
import argparse
import json
import os
import re
import struct
import sys

SHADER = 2             # the kTaaHlsl version this build has; --shader picks the one a dump was made with
DEFAULTS = {'current': 0.1, 'interior_scale': 0.5, 'kernel': 3.5, 'clip_interior': 3.0, 'clip_edge': 1.25, 'sharpen': 0.3,
            'coverage_tol': 0.05, 'shader': SHADER}
SHADER_SHARPEN = {1: 0.6, 2: 0.3}   # kSharpAmount with each version: the default of --sharpen
LUMA = (0.2126, 0.7152, 0.0722)
BAND = 128             # output rows a band: the arrays stay small at headset sizes
MATCH_MEAN = 1.0       # 8-bit units
MATCH_P99 = 4.0
SEQ_MARGIN = 4         # hull_layer.cpp kSeqMargin: render pixels about the cover's extent
COMP_MARGIN = 8        # ... the composite's crop about the scaled cover crop
HIST_MARGIN = 64       # ... kSeqHistMargin: the history's crop about frame 0's composite crop


# ---- the transcription ---------------------------------------------------------------------------------------------------

def bilinear(tex, u, v):
    """tex (H, W, C) sampled at normalized (u, v) with linear filtering and clamp addressing (the TAA's sampler)."""
    import numpy as np
    H, W = tex.shape[:2]
    x = u * W - 0.5
    y = v * H - 0.5
    x0 = np.floor(x)
    y0 = np.floor(y)
    fx = (x - x0)[..., None].astype(np.float32)
    fy = (y - y0)[..., None].astype(np.float32)
    x0 = x0.astype(np.int64)
    y0 = y0.astype(np.int64)
    xa, xb = np.clip(x0, 0, W - 1), np.clip(x0 + 1, 0, W - 1)
    ya, yb = np.clip(y0, 0, H - 1), np.clip(y0 + 1, 0, H - 1)
    top = tex[ya, xa] * (1 - fx) + tex[ya, xb] * fx
    bottom = tex[yb, xa] * (1 - fx) + tex[yb, xb] * fx
    return top * (1 - fy) + bottom * fy


def catmull_rom(tex, u, v):
    """kTaaHlsl's histCatmullRom: Catmull-Rom through nine bilinear taps."""
    import numpy as np
    H, W = tex.shape[:2]
    out = 0.0
    taps = []
    for pos, size in ((u * W, W), (v * H, H)):
        c1 = np.floor(pos - 0.5) + 0.5
        f = pos - c1
        w0 = f * (-0.5 + f * (1.0 - 0.5 * f))
        w1 = 1.0 + f * f * (-2.5 + 1.5 * f)
        w2 = f * (0.5 + f * (2.0 - 1.5 * f))
        w3 = f * f * (-0.5 + 0.5 * f)
        w12 = w1 + w2
        o12 = w2 / w12
        taps.append(((c1 - 1.0) / size, w0, (c1 + o12) / size, w12, (c1 + 2.0) / size, w3))
    (tx0, wx0, tx12, wx12, tx3, wx3), (ty0, wy0, ty12, wy12, ty3, wy3) = taps
    for ty, wy in ((ty0, wy0), (ty12, wy12), (ty3, wy3)):
        for tx, wx in ((tx0, wx0), (tx12, wx12), (tx3, wx3)):
            out = out + bilinear(tex, tx, ty) * (wx * wy)[..., None]
    return out


def reproject(fr, prx, pry, zr):
    """The history position of render position (prx, pry) at depth zr: (pp.x, pp.y, valid), as kTaaHlsl computes it."""
    import numpy as np
    rw, rh = fr['render']
    tn, tp = fr['tan_now'], fr['tan_prev']
    dx = tn[0] + (prx + 0.5) / rw * (tn[1] - tn[0])
    dy = tn[3] - (pry + 0.5) / rh * (tn[3] - tn[2])
    dz = -1.0
    R = fr['head_delta']
    px = R[0] * dx + R[1] * dy + R[2] * dz
    py = R[3] * dx + R[4] * dy + R[5] * dz
    pz = R[6] * dx + R[7] * dy + R[8] * dz + np.zeros_like(dx)
    if fr.get('use_trans'):
        A, B = fr['depth_ab']
        T = fr['head_trans']
        den = zr - A
        m = (zr > 0) & (den > 0)
        s = np.where(m, B / np.where(m, den, 1.0), 1.0)
        px = np.where(m, px * s + T[0], px)
        py = np.where(m, py * s + T[1], py)
        pz = np.where(m, pz * s + T[2], pz)
    valid = pz < -1e-6
    inv = 1.0 / np.where(valid, -pz, 1.0)
    ppx = (px * inv - tp[0]) / (tp[1] - tp[0]) * rw - 0.5
    ppy = (tp[3] - py * inv) / (tp[3] - tp[2]) * rh - 0.5
    return ppx, ppy, valid


def taa_band(cover, cdepth, hist, fr, P, ys, xs):
    """kTaaHlsl for the output rows ys and columns xs: the next history (premultiplied colour, coverage in alpha)."""
    import numpy as np
    rw, rh = fr['render']
    ow, oh = fr['output']
    iy, ix = np.meshgrid(ys, xs, indexing='ij')
    prx = ((ix + 0.5) * rw / ow - 0.5).astype(np.float32)
    pry = ((iy + 0.5) * rh / oh - 0.5).astype(np.float32)
    jx, jy = np.float32(fr['jitter'][0]), np.float32(fr['jitter'][1])
    cx = np.rint(prx + jx).astype(np.int64)
    cy = np.rint(pry + jy).astype(np.int64)
    shape = prx.shape + (4,)
    cur = np.zeros(shape, np.float32)
    wsum = np.zeros(prx.shape + (1,), np.float32)
    lo = np.full(shape, 1e9, np.float32)
    hi = np.full(shape, -1e9, np.float32)
    m1 = np.zeros(shape, np.float32)
    m2 = np.zeros(shape, np.float32)
    zr = np.zeros(prx.shape, np.float32)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            qx = np.clip(cx + dx, 0, rw - 1)
            qy = np.clip(cy + dy, 0, rh - 1)
            s = cover[qy, qx]
            ddx = qx - jx - prx
            ddy = qy - jy - pry
            wt = np.exp(-np.float32(P['kernel']) * (ddx * ddx + ddy * ddy)).astype(np.float32)[..., None]
            cur += s * wt
            wsum += wt
            lo = np.minimum(lo, s)
            hi = np.maximum(hi, s)
            m1 += s
            m2 += s * s
            zr = np.maximum(zr, cdepth[qy, qx])
    cur /= np.maximum(wsum, 1e-6)
    m1 /= 9.0
    sd = np.sqrt(np.maximum(m2 / 9.0 - m1 * m1, 0.0))
    interior = lo[..., 3] > 0.999
    a_lo, a_hi = lo[..., 3].copy(), hi[..., 3].copy()   # the 3x3's coverage range, before the clip tightens lo and hi
    weight = np.where(interior, P['current'] * P['interior_scale'], P['current']).astype(np.float32)[..., None]
    lo_i, hi_i = m1 - P['clip_interior'] * sd, m1 + P['clip_interior'] * sd
    lo_i[..., 3] = 1.0
    hi_i[..., 3] = 1.0
    lo = np.where(interior[..., None], lo_i, np.maximum(lo, m1 - P['clip_edge'] * sd))
    hi = np.where(interior[..., None], hi_i, np.minimum(hi, m1 + P['clip_edge'] * sd))
    res = cur
    if fr['history'] and hist is not None:
        ppx, ppy, valid = reproject(fr, prx, pry, zr)
        u = (ppx + 0.5) / rw
        v = (ppy + 0.5) / rh
        ok = valid & (u >= 0) & (u <= 1) & (v >= 0) & (v <= 1)
        hraw = catmull_rom(hist, u, v)
        h = np.minimum(np.maximum(hraw, lo), hi)
        if P.get('shader', SHADER) >= 2:
            # Shader 2: at the silhouette, a history whose coverage agrees with the 3x3's is taken unclamped in colour
            # (saturated), at the interior's current weight; any other keeps the clip above.
            tol = P['coverage_tol']
            agree = ~interior & (hraw[..., 3] >= a_lo - tol) & (hraw[..., 3] <= a_hi + tol)
            h = np.where(agree[..., None], np.clip(hraw, 0.0, 1.0), h)
            weight = np.where(agree[..., None], np.float32(P['current'] * P['interior_scale']), weight)
        res = np.where(ok[..., None], h + (cur - h) * weight, cur)
    return res.astype(np.float32)


def sharpen_band(h, amount, ys, xs):
    """kSharpHlsl on the history h (full output canvas) for rows ys and columns xs: c + amount (c - mean of the cross), clamped to
    the cross's range, where the whole cross is hull."""
    import numpy as np
    oh, ow = h.shape[:2]
    iy, ix = np.meshgrid(ys, xs, indexing='ij')
    c = h[iy, ix]
    l = h[iy, np.clip(ix - 1, 0, ow - 1)]
    r = h[iy, np.clip(ix + 1, 0, ow - 1)]
    u = h[np.clip(iy - 1, 0, oh - 1), ix]
    d = h[np.clip(iy + 1, 0, oh - 1), ix]
    interior = np.minimum(np.minimum(c[..., 3], l[..., 3]), np.minimum(np.minimum(r[..., 3], u[..., 3]), d[..., 3])) > 0.999
    m = (l[..., :3] + r[..., :3] + u[..., :3] + d[..., :3]) * 0.25
    lo = np.minimum(c[..., :3], np.minimum(np.minimum(l[..., :3], r[..., :3]), np.minimum(u[..., :3], d[..., :3])))
    hi = np.maximum(c[..., :3], np.maximum(np.maximum(l[..., :3], r[..., :3]), np.maximum(u[..., :3], d[..., :3])))
    out = c.copy()
    out[..., :3] = np.where(interior[..., None], np.clip(c[..., :3] + np.float32(amount) * (c[..., :3] - m), lo, hi), c[..., :3])
    return out


def bicubic_cover(cover, fr, ys, xs):
    """The frame's cover alone, upscaled: Catmull-Rom 4x4 at each output pixel's position in the jittered cover."""
    import numpy as np
    rw, rh = fr['render']
    ow, oh = fr['output']
    iy, ix = np.meshgrid(ys, xs, indexing='ij')
    x = (ix + 0.5) * rw / ow - 0.5 + fr['jitter'][0]
    y = (iy + 0.5) * rh / oh - 0.5 + fr['jitter'][1]
    x0, y0 = np.floor(x), np.floor(y)
    fx, fy = x - x0, y - y0

    def weights(f):
        return (f * (-0.5 + f * (1.0 - 0.5 * f)), 1.0 + f * f * (-2.5 + 1.5 * f), f * (0.5 + f * (2.0 - 1.5 * f)), f * f * (-0.5 + 0.5 * f))
    wx, wy = weights(fx), weights(fy)
    out = np.zeros(x.shape + (cover.shape[2],), np.float32)
    for j in range(4):
        yy = np.clip(y0.astype(np.int64) + j - 1, 0, rh - 1)
        for i in range(4):
            xx = np.clip(x0.astype(np.int64) + i - 1, 0, rw - 1)
            out += cover[yy, xx] * (wx[i] * wy[j])[..., None]
    return out


# ---- the sequence -----------------------------------------------------------------------------------------------------------

def load_sequence(prefix):
    """The JSON and the frames' images (cropped), as a dict; ValueError on missing or inconsistent files."""
    import numpy as np
    import eye_inputs
    path = prefix + '_HullSeq.json'
    if not os.path.exists(path):
        raise ValueError('missing %s' % path)
    with open(path, encoding='utf-8') as f:
        seq = json.load(f)
    frames = []
    for i, fr in enumerate(seq.get('frames', [])):
        if fr.get('status') != 'written' or fr.get('index') != i:
            break   # the TAA's frames follow one another: the sequence ends at the first frame not on disk
        fr = dict(fr)
        for key in ('Cover', 'Depth', 'Comp'):
            p = '%s_HullSeq_%02d_%s.bin' % (prefix, i, key)
            if not os.path.exists(p):
                raise ValueError('missing %s' % p)
            _, img = eye_inputs.read(p)
            fr[key.lower()] = np.asarray(img, np.float32)
        cw, ch = fr['crop'][2], fr['crop'][3]
        if fr['cover'].shape[:2] != (ch, cw) or fr['depth'].shape[:2] != (ch, cw):
            raise ValueError('frame %d: the cover or depth is not its %dx%d crop' % (i, cw, ch))
        if fr['comp'].shape[:2] != (fr['comp_crop'][3], fr['comp_crop'][2]):
            raise ValueError('frame %d: the composite is not its crop' % i)
        if frames and (fr['render'] != frames[0]['render'] or fr['output'] != frames[0]['output']):
            raise ValueError('frame %d changes the render or output size' % i)
        frames.append(fr)
    if not frames:
        raise ValueError('the sequence holds no written frame (end: %s)' % seq.get('end'))
    seq['loaded'] = frames
    seq['hist_image'] = None
    hp = '%s_HullSeq_00_Hist.bin' % prefix
    if seq.get('history', {}).get('status') == 'written' and os.path.exists(hp):
        _, img = eye_inputs.read(hp)
        seq['hist_image'] = np.asarray(img, np.float32)
    return seq


def place(canvas, crop, img):
    x, y, w, h = crop
    canvas[y:y + h, x:x + w] = img
    return canvas


def window_of(frames, hist_crop=None):
    """The output rectangle (x0, y0, x1, y1) the simulation runs over: every composite crop and the history's. Outside it the
    TAA's result is exactly zero (no hull in any 3x3), so nothing is lost."""
    boxes = [fr['comp_crop'] for fr in frames] + ([hist_crop] if hist_crop and hist_crop[2] and hist_crop[3] else [])
    x0 = min(b[0] for b in boxes)
    y0 = min(b[1] for b in boxes)
    x1 = max(b[0] + b[2] for b in boxes)
    y1 = max(b[1] + b[3] for b in boxes)
    return x0, y0, x1, y1


def run(frames, hist0, P, window, keep_last):
    """The TAA and sharpen over the frames; yields (frame, cover canvas, sharpened window (h, w, 4))."""
    import numpy as np
    rw, rh = frames[0]['render']
    ow, oh = frames[0]['output']
    hist = hist0
    x0, y0, x1, y1 = window
    xs = np.arange(x0, x1)
    for fr in frames:
        cover = place(np.zeros((rh, rw, 4), np.float32), fr['crop'], fr['cover'])
        depth = place(np.zeros((rh, rw), np.float32), fr['crop'], fr['depth'])
        new = np.zeros((oh, ow, 4), np.float32)
        for b in range(y0, y1, BAND):
            ys = np.arange(b, min(b + BAND, y1))
            new[ys[0]:ys[-1] + 1, x0:x1] = taa_band(cover, depth, hist, fr, P, ys, xs).astype(np.float16).astype(np.float32)
        out = np.zeros((y1 - y0, x1 - x0, 4), np.float32)
        for b in range(y0, y1, BAND):
            ys = np.arange(b, min(b + BAND, y1))
            out[ys[0] - y0:ys[-1] - y0 + 1] = sharpen_band(new, P['sharpen'], ys, xs)
        hist = new
        yield fr, cover, out


def luma(rgb):
    return rgb[..., 0] * LUMA[0] + rgb[..., 1] * LUMA[1] + rgb[..., 2] * LUMA[2]


def hf_energy(l, mask):
    """The mean squared 4-neighbour Laplacian of l over mask (eroded by one pixel)."""
    import numpy as np
    lap = 4 * l[1:-1, 1:-1] - l[:-2, 1:-1] - l[2:, 1:-1] - l[1:-1, :-2] - l[1:-1, 2:]
    m = mask[1:-1, 1:-1] & mask[:-2, 1:-1] & mask[2:, 1:-1] & mask[1:-1, :-2] & mask[1:-1, 2:] & np.isfinite(lap)
    return float(np.mean(lap[m] ** 2)) if m.any() else 0.0


def quantize8(x):
    import numpy as np
    return np.round(np.clip(x, 0.0, 1.0) * 255.0) / 255.0


def simulate(seq, P, last=8, compare=False):
    """The measurement as a dict."""
    import numpy as np
    frames = seq['loaded']
    f0 = frames[0]
    ow, oh = f0['output']
    hist_crop = seq.get('history', {}).get('crop')
    exact = True
    hist0 = np.zeros((oh, ow, 4), np.float32)
    if f0['history']:
        if seq['hist_image'] is not None:
            place(hist0, hist_crop, seq['hist_image'])
        else:
            exact = False   # frame 0 read a history the dump does not hold: the simulation starts from nothing
    window = window_of(frames, hist_crop if seq['hist_image'] is not None else None)
    x0, y0, x1, y1 = window
    wh, ww = y1 - y0, x1 - x0
    lumas, masks, gpu_lumas = [], [], []
    diffs = []
    last_cover = last_out = None
    for fr, cover, out in run(frames, hist0, P, window, last):
        interior = out[..., 3] > 0.999
        sim_rgb = quantize8(out[..., :3])
        gpu = np.full((wh, ww, 4), np.nan, np.float32)
        cx, cy, cw, ch = fr['comp_crop']
        gpu[cy - y0:cy - y0 + ch, cx - x0:cx - x0 + cw] = fr['comp']
        lumas.append(luma(sim_rgb) * 255.0)
        masks.append(interior)
        gpu_lumas.append(luma(gpu[..., :3]) * 255.0)
        lumas, masks, gpu_lumas = lumas[-last:], masks[-last:], gpu_lumas[-last:]
        if compare:
            m = interior & np.isfinite(gpu[..., 0])
            d = np.abs(sim_rgb - gpu[..., :3])[m] * 255.0
            diffs.append({'index': fr['index'], 'pixels': int(m.sum()), 'mean': float(d.mean()) if d.size else 0.0,
                          'p99': float(np.percentile(d, 99)) if d.size else 0.0, 'max': float(d.max()) if d.size else 0.0})
        last_cover, last_out, last_fr, last_gpu = cover, out, fr, gpu
    r = {'frames': len(frames), 'render': f0['render'], 'output': f0['output'], 'window': window, 'exact_start': exact, 'params': dict(P)}
    stack_mask = np.logical_and.reduce(masks)
    r['interior_px'] = int(stack_mask.sum())
    if len(lumas) >= 2 and stack_mask.any():
        sd = np.std(np.stack(lumas), axis=0)[stack_mask]
        r['shimmer_sim'] = (float(sd.mean()), float(np.percentile(sd, 95)))
        g = np.stack(gpu_lumas)
        gm = stack_mask & np.all(np.isfinite(g), axis=0)
        if gm.any():
            gsd = np.std(g, axis=0)[gm]
            r['shimmer_gpu'] = (float(gsd.mean()), float(np.percentile(gsd, 95)))
    # Sharpness: the last frame against its own cover, bicubically upscaled (the single-frame reference of taa_blur.py).
    ys, xs = np.arange(y0, y1), np.arange(x0, x1)
    bic = luma(bicubic_cover(last_cover, last_fr, ys, xs)[..., :3])
    mask = last_out[..., 3] > 0.999
    ref = hf_energy(bic, mask)
    r['sharpness_sim'] = hf_energy(luma(last_out[..., :3]), mask) / ref if ref > 0 else 0.0
    r['sharpness_gpu'] = hf_energy(luma(last_gpu[..., :3]), mask) / ref if ref > 0 else 0.0
    if compare:
        r['compare'] = diffs
        if not exact:
            r['verdict'] = 'INCONCLUSIVE'
            r['why'] = 'frame 0 read a history the dump does not hold (no HullSeq_00_Hist): the simulation starts from nothing'
        elif all(d['mean'] <= MATCH_MEAN and d['p99'] <= MATCH_P99 for d in diffs):
            r['verdict'] = 'MATCH'
            r['why'] = 'the simulation reproduces the GPU composite on the hull interior (mean <= %.1f, p99 <= %.1f, 8-bit units)' % (MATCH_MEAN, MATCH_P99)
        else:
            r['verdict'] = 'MISMATCH'
            r['why'] = 'the simulation and the GPU composite differ: the transcription, the dump or the parameters are not the shader\'s'
    return r


def report(prefix, r):
    P = r['params']
    lines = ['hull taa sim: %s: %d frame(s), render %dx%d, output %dx%d, window %s; shader %d current %g interior_scale %g kernel %g clip %g/%g sharpen %g'
             % (os.path.basename(prefix), r['frames'], r['render'][0], r['render'][1], r['output'][0], r['output'][1], r['window'],
                P.get('shader', SHADER), P['current'], P['interior_scale'], P['kernel'], P['clip_interior'], P['clip_edge'], P['sharpen'])]
    if 'shimmer_sim' in r:
        line = ('shimmer (luma std over the last frames, 8-bit units, %d interior px): sim mean %.3f p95 %.3f'
                % (r['interior_px'], r['shimmer_sim'][0], r['shimmer_sim'][1]))
        if 'shimmer_gpu' in r:
            line += '; gpu mean %.3f p95 %.3f' % r['shimmer_gpu']
        lines.append(line)
    else:
        lines.append('shimmer: not measured (fewer than 2 frames, or no pixel interior in all of them)')
    lines.append('sharpness (high-frequency energy against the last frame\'s bicubic upscale): sim %.3f, gpu %.3f' % (r['sharpness_sim'], r['sharpness_gpu']))
    for d in r.get('compare', []):
        lines.append('compare frame %02d: %d px, mean %.3f p99 %.3f max %.3f' % (d['index'], d['pixels'], d['mean'], d['p99'], d['max']))
    if 'verdict' in r:
        lines.append('%s: %s' % (r['verdict'], r['why']))
    return '\n'.join(lines)


# ---- the self-test ----------------------------------------------------------------------------------------------------------

def shader_defaults(src):
    """The TAA's and sharpen's parameters as hull_layer.cpp has them, or None for any not found."""
    def num(pattern):
        m = re.search(pattern, src)
        return float(m.group(1)) if m else None
    return {'kernel': num(r'float wt = exp\(-([\d.]+) \* dot\(dd, dd\)\)'),
            'clip_interior': num(r'lo = m1 - ([\d.]+) \* sd;'),
            'clip_edge': num(r'lo = max\(lo, m1 - ([\d.]+) \* sd\)'),
            'interior_scale': num(r'weight = jit\.w \* ([\d.]+);'),
            'current': num(r'constexpr float kTaaCurrent = ([\d.]+)f;'),
            'sharpen': num(r'constexpr float kSharpAmount = ([\d.]+)f;'),
            'coverage_tol': num(r'h\.a >= aLo - ([\d.]+) && h\.a <= aHi \+ \1'),
            'shader': 2.0 if re.search(r'res = lerp\(saturate\(h\), cur, jit\.w \* 0\.5\)', src) else 1.0}


def crop_of(alpha, margin, w, h):
    """hull_layer.cpp seqWriteSet's crop: alpha's extent grown by margin, (0, 0, 1, 1) when empty."""
    import numpy as np
    ys, xs = np.nonzero(alpha > 0)
    if not len(xs):
        return [0, 0, 1, 1]
    x0, y0 = max(int(xs.min()) - margin, 0), max(int(ys.min()) - margin, 0)
    x1, y1 = min(int(xs.max()) + 1 + margin, w), min(int(ys.max()) + 1 + margin, h)
    return [x0, y0, x1 - x0, y1 - y0]


def grow(c, m, w, h):
    x0, y0 = max(c[0] - m, 0), max(c[1] - m, 0)
    x1, y1 = min(c[0] + c[2] + m, w), min(c[1] + c[3] + m, h)
    return [x0, y0, max(x1 - x0, 1), max(y1 - y0, 1)]


def write_sequence(prefix, frames, hist0=None, P=None):
    """A synthetic dump in the format seqWriteSet and seqWriteJson write; the composites are this simulation's own (the GPU's
    stand-in). frames: dicts with cover (rh, rw, 4) in [0, 1], depth (rh, rw), and the JSON's constants."""
    import numpy as np
    P = P or DEFAULTS
    rw, rh = frames[0]['render']
    ow, oh = frames[0]['output']

    def tex(path, fmt, arr, px, frame):
        h, w = arr.shape[:2]
        with open(path, 'wb') as f:
            f.write(b'EDVRTEX1' + struct.pack('<9I', 1, w, h, fmt, w * px, frame, 0, 0, 0) + arr.tobytes())
    cover8 = []
    for fr in frames:
        c8 = np.round(np.clip(fr['cover'], 0, 1) * 255).astype(np.uint8)
        fr['crop'] = crop_of(c8[..., 3], SEQ_MARGIN, rw, rh)
        x, y, w, h = fr['crop']
        sx = [x * ow // rw, y * oh // rh, -(-(x + w) * ow // rw), -(-(y + h) * oh // rh)]
        fr['comp_crop'] = grow([sx[0], sx[1], sx[2] - sx[0], sx[3] - sx[1]], COMP_MARGIN, ow, oh)
        cover8.append(c8)
    hist_crop = grow(frames[0]['comp_crop'], HIST_MARGIN, ow, oh) if hist0 is not None else [0, 0, 0, 0]
    seq_frames = []
    for i, (fr, c8) in enumerate(zip(frames, cover8)):
        x, y, w, h = fr['crop']
        f = {k: fr[k] for k in ('render', 'output', 'jitter', 'tan_now', 'tan_prev', 'head_delta', 'head_trans', 'use_trans', 'depth_ab', 'history',
                                 'crop', 'comp_crop')}
        f.update(index=i, frame=100 + i, status='written', region=[0, 0], tan_prev_given=True, head_delta_given=True, applied=i > 0,
                 after_ui_resolve=True)
        f['cover'] = c8[y:y + h, x:x + w].astype(np.float32) / 255.0
        f['depth'] = fr['depth'][y:y + h, x:x + w].astype(np.float32)
        seq_frames.append(f)
    h0 = np.zeros((oh, ow, 4), np.float32)
    if hist0 is not None:
        hx, hy, hw, hh = hist_crop
        h0[hy:hy + hh, hx:hx + hw] = hist0[hy:hy + hh, hx:hx + hw].astype(np.float16).astype(np.float32)
    window = window_of(seq_frames, hist_crop if hist0 is not None else None)
    x0, y0 = window[0], window[1]
    for f, (fr, cover, out) in zip(seq_frames, run(seq_frames, h0, P, window, 1)):
        i = f['index']
        x, y, w, h = f['crop']
        tex('%s_HullSeq_%02d_Cover.bin' % (prefix, i), 28, cover8[i][y:y + h, x:x + w].copy(), 4, f['frame'])
        tex('%s_HullSeq_%02d_Depth.bin' % (prefix, i), 41, np.ascontiguousarray(f['depth'], np.float32), 4, f['frame'])
        cx, cy, cw, ch = f['comp_crop']
        comp = np.ones((ch, cw, 4), np.float32)   # world black, world alpha 1: the composite is the result's colour
        comp[..., :3] = out[cy - y0:cy - y0 + ch, cx - x0:cx - x0 + cw, :3]
        tex('%s_HullSeq_%02d_Comp.bin' % (prefix, i), 28, np.round(np.clip(comp, 0, 1) * 255).astype(np.uint8), 4, f['frame'])
    if hist0 is not None:
        hx, hy, hw, hh = hist_crop
        tex('%s_HullSeq_00_Hist.bin' % prefix, 10, np.ascontiguousarray(h0[hy:hy + hh, hx:hx + hw].astype(np.float16)), 8, 100)
    js = {'version': 1, 'stamp': os.path.basename(prefix)[4:], 'eye': 0, 'start_frame': 100, 'frames_wanted': 16, 'end': 'complete',
          'taa': {'current_weight': P['current'], 'sharpen': P['sharpen']},
          'history': {'file': 'HullSeq_00_Hist', 'status': 'written' if hist0 is not None else 'none (frame 0 had no history)', 'crop': hist_crop},
          'frames': [{k: v for k, v in f.items() if k not in ('cover', 'depth')} for f in seq_frames]}
    with open(prefix + '_HullSeq.json', 'w', encoding='utf-8') as f:
        json.dump(js, f)


def synthetic(n=10, history0=False, moving=False):
    """A small hull: a textured rectangle with a thin diagonal line, sampled at each frame's jitter (Halton 2, 3)."""
    import numpy as np
    rw, rh, ow, oh = 40, 32, 52, 42

    def halton(i, b):
        f, r = 1.0, 0.0
        while i:
            f /= b
            r += f * (i % b)
            i //= b
        return r
    frames = []
    for k in range(n):
        jx, jy = halton(k + 1, 2) - 0.5, halton(k + 1, 3) - 0.5
        shift = 0.25 * k if moving else 0.0
        qy, qx = np.mgrid[0:rh, 0:rw].astype(np.float64)
        x, y = qx - jx + shift, qy - jy   # the unjittered position each render sample sees
        inside = (x >= 6) & (x < 34) & (y >= 5) & (y < 27)
        tone = 0.45 + 0.3 * np.sin(1.7 * x) * np.cos(1.1 * y)
        tone = np.where(np.abs((x - y) - 4.0) < 0.35, 0.95, tone)   # a sub-pixel panel line
        cover = np.zeros((rh, rw, 4))
        cover[..., 0] = np.where(inside, tone, 0)
        cover[..., 1] = np.where(inside, tone * 0.9, 0)
        cover[..., 2] = np.where(inside, tone * 0.8, 0)
        cover[..., 3] = np.where(inside, 1.0, 0)
        frames.append({'render': [rw, rh], 'output': [ow, oh], 'jitter': [jx, jy], 'tan_now': [-1.0, 1.0, 1.0, -1.0], 'tan_prev': [-1.0, 1.0, 1.0, -1.0],
                       'head_delta': [1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0], 'head_trans': [0.0, 0.0, 0.0], 'use_trans': False, 'depth_ab': [0.0, 1.0],
                       'history': history0 or k > 0, 'cover': cover, 'depth': np.where(inside, 0.25, 0.0)})
    hist0 = None
    if history0:
        hist0 = np.zeros((oh, ow, 4), np.float32)
        hist0[8:34, 9:43] = (0.4, 0.36, 0.32, 1.0)
    return frames, hist0


def self_test():
    import tempfile
    try:
        import numpy as np
    except ImportError:
        print('hull taa sim self-test skipped: NumPy is not installed')
        return 0
    failures = []
    # The defaults are the shader's: a drifted kTaaHlsl or constant fails the build, not the next offline session.
    src_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src', 'd3d11', 'hull_layer.cpp')
    with open(src_path, encoding='utf-8', errors='replace') as f:
        found = shader_defaults(f.read())
    for k, v in found.items():
        if v is None or abs(v - DEFAULTS[k]) > 1e-9:
            failures.append('the default %s %s is not hull_layer.cpp\'s %s' % (k, DEFAULTS[k], v))
    # The reprojection: last frame's tangents one render pixel to the right carry each pixel one pixel left.
    fr = {'render': [40, 32], 'tan_now': [-1.0, 1.0, 1.0, -1.0], 'tan_prev': [-1.0 + 2.0 / 40, 1.0 + 2.0 / 40, 1.0, -1.0],
          'head_delta': [1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0], 'use_trans': False}
    prx, pry = np.array([10.0, 20.5]), np.array([3.0, 7.25])
    ppx, ppy, valid = reproject(fr, prx, pry, np.zeros(2))
    if not (valid.all() and np.allclose(ppx, prx - 1.0, atol=1e-6) and np.allclose(ppy, pry, atol=1e-6)):
        failures.append('a one-pixel tangent shift reprojects to %r, %r, wanted x - 1' % (ppx, ppy))
    # Catmull-Rom at a texel centre is that texel.
    t = np.random.default_rng(1).random((6, 7, 4)).astype(np.float32)
    got = catmull_rom(t, np.array([(3 + 0.5) / 7]), np.array([(2 + 0.5) / 6]))
    if not np.allclose(got[0], t[2, 3], atol=1e-5):
        failures.append('Catmull-Rom at a texel centre returned %r, wanted %r' % (got[0], t[2, 3]))
    # Shader 2 against 1: the silhouette's coverage test changes edge pixels and leaves the hull's inside as it was.
    frames, hist0 = synthetic(n=2, history0=True, moving=True)
    f1 = frames[1]
    ys, xs = np.arange(42), np.arange(52)
    band = [taa_band(f1['cover'].astype(np.float32), f1['depth'].astype(np.float32), hist0, f1, dict(DEFAULTS, shader=v), ys, xs)
            for v in (1, 2)]
    delta = np.abs(band[1] - band[0]).max(axis=-1)
    if not (delta.max() > 1e-3 and delta[12:31, 14:39].max() == 0.0):
        failures.append('shader 2 against 1: largest change %.4g, inside the hull %.4g (wanted > 0 and 0)'
                        % (delta.max(), delta[12:31, 14:39].max()))

    def main_code(argv):
        import io
        import contextlib
        with contextlib.redirect_stdout(io.StringIO()):
            return main(argv)
    with tempfile.TemporaryDirectory() as temp:
        # A sequence whose composites are the simulation's own matches itself, from a reset (frame 0 without history).
        prefix = os.path.join(temp, 'eye_123456789001')
        frames, _ = synthetic()
        write_sequence(prefix, frames)
        seq = load_sequence(prefix)
        r = simulate(seq, DEFAULTS, last=6, compare=True)
        if r.get('verdict') != 'MATCH':
            failures.append('the simulation against its own composites: %s %r' % (r.get('verdict'), r.get('compare')))
        if main_code([prefix, '--compare']) != 0:
            failures.append('main --compare on a matching sequence did not exit 0')
        # Temporal accumulation steadies the sub-pixel line: less shimmer than every frame alone (current weight 1).
        alone = simulate(seq, dict(DEFAULTS, current=1.0, interior_scale=1.0), last=6)
        if not ('shimmer_sim' in r and 'shimmer_sim' in alone and r['shimmer_sim'][0] < alone['shimmer_sim'][0]):
            failures.append('the TAA did not lower the shimmer: %r against %r' % (r.get('shimmer_sim'), alone.get('shimmer_sim')))
        # The sharpen raises the high-frequency energy.
        soft = simulate(seq, dict(DEFAULTS, sharpen=0.0), last=6)
        if not r['sharpness_sim'] > soft['sharpness_sim'] > 0:
            failures.append('the sharpen did not raise the high-frequency energy: %.4f against %.4f' % (r['sharpness_sim'], soft['sharpness_sim']))
        # A composite the GPU wrote differently is a mismatch.
        p = prefix + '_HullSeq_04_Comp.bin'
        data = bytearray(open(p, 'rb').read())
        for i in range(44, len(data), 4):
            data[i] = min(255, data[i] + 12)
        open(p, 'wb').write(bytes(data))
        if main_code([prefix, '--compare']) != 3:
            failures.append('a perturbed composite was not a MISMATCH (exit 3)')
        # Other parameters are a mismatch too: the comparison can tell the shader's parameters from others.
        write_sequence(prefix, synthetic()[0])
        if simulate(load_sequence(prefix), dict(DEFAULTS, kernel=1.0), compare=True).get('verdict') != 'MISMATCH':
            failures.append('a kernel of 1.0 against the shader\'s 3.5 matched')
    with tempfile.TemporaryDirectory() as temp:
        # Frame 0 with a history: the dump's HullSeq_00_Hist starts the simulation where the GPU did; a moving hull uses it.
        prefix = os.path.join(temp, 'eye_123456789002')
        frames, hist0 = synthetic(n=8, history0=True, moving=True)
        write_sequence(prefix, frames, hist0)
        r = simulate(load_sequence(prefix), DEFAULTS, compare=True)
        if r.get('verdict') != 'MATCH':
            failures.append('a sequence with frame 0\'s history: %s %r' % (r.get('verdict'), r.get('compare')))
        os.remove(prefix + '_HullSeq_00_Hist.bin')
        if main_code([prefix, '--compare']) != 1:
            failures.append('frame 0\'s history missing was not INCONCLUSIVE (exit 1)')
    with tempfile.TemporaryDirectory() as temp:
        if main_code([os.path.join(temp, 'eye_x')]) != 2:
            failures.append('a missing sequence was not bad input (exit 2)')
    for f in failures:
        print('hull taa sim self-test FAILED: ' + f)
    if not failures:
        print('hull taa sim self-test passed (12 checks)')
    return 1 if failures else 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('prefix', nargs='?', help='the eye run prefix: <edvr_logs>\\eyes\\eye_<stamp>')
    p.add_argument('--current', type=float, default=DEFAULTS['current'], help='this frame\'s weight at the silhouette (kTaaCurrent)')
    p.add_argument('--interior-scale', type=float, default=DEFAULTS['interior_scale'], help='the current weight\'s scale inside the hull')
    p.add_argument('--kernel', type=float, default=DEFAULTS['kernel'], help='the per-frame kernel exp(-k d^2), render pixels')
    p.add_argument('--clip-interior', type=float, default=DEFAULTS['clip_interior'], help='the interior box, sigmas about the 3x3 mean')
    p.add_argument('--clip-edge', type=float, default=DEFAULTS['clip_edge'], help='the silhouette\'s variance clip, sigmas (inside min/max)')
    p.add_argument('--sharpen', type=float, default=None,
                   help='the cross unsharp mask\'s amount (kSharpAmount; default the --shader version\'s: 0.3 for 2, 0.6 for 1)')
    p.add_argument('--shader', type=int, choices=sorted(SHADER_SHARPEN), default=SHADER,
                   help='the TAA version: 2 (default) for builds after b85d728c, the silhouette\'s coverage test; '
                        '1 for builds up to b85d728c, the silhouette always variance-clipped')
    p.add_argument('--last', type=int, default=8, help='frames the shimmer is measured over (default 8)')
    p.add_argument('--compare', action='store_true', help='compare with the composites the GPU wrote')
    p.add_argument('--self-test', action='store_true')
    a = p.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.prefix:
        p.error('give the eye run prefix, or --self-test')
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    P = {'current': a.current, 'interior_scale': a.interior_scale, 'kernel': a.kernel, 'clip_interior': a.clip_interior, 'clip_edge': a.clip_edge,
         'sharpen': SHADER_SHARPEN[a.shader] if a.sharpen is None else a.sharpen, 'coverage_tol': DEFAULTS['coverage_tol'], 'shader': a.shader}
    try:
        seq = load_sequence(a.prefix)
        r = simulate(seq, P, last=max(2, a.last), compare=a.compare)
    except ValueError as e:
        print('hull taa sim: %s' % e)
        return 2
    print(report(a.prefix, r))
    return {'MATCH': 0, 'MISMATCH': 3, 'INCONCLUSIVE': 1}.get(r.get('verdict'), 0)


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    sys.exit(main())
