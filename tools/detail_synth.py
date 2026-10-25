"""Add the fine grain a 2x upscale of a gritty texture is missing (grass, gravel, tarmac), without a neural network.

A plain 2x resize has no energy above the original's Nyquist frequency, so noisy surfaces look soft. The texture's own
radial power spectrum is a power law in its upper band; this extends that law one octave and fills the new band with
noise of exactly that spectrum, on the luminance only. Textures tile, so everything is done on a wrap-padded copy and
the noise itself is periodic. The result is fresh grain of the same character, not a new pattern.
"""
import numpy as np
from PIL import Image


def _radial_power(F, n):
    h, w = F.shape
    fy = np.fft.fftfreq(h)[:, None] * h
    fx = np.fft.fftfreq(w)[None, :] * w
    r = np.hypot(fx, fy)
    p = np.abs(F) ** 2
    bins = np.minimum(r.astype(int), n - 1)
    sums = np.bincount(bins.ravel(), p.ravel(), n)
    cnt = np.maximum(np.bincount(bins.ravel(), minlength=n), 1)
    return sums / cnt


def enhance(img, scale=2, strength=1.0, seed=1):
    """img: RGB PIL image (a tiling texture) -> RGB PIL image, `scale` times larger, with synthesized fine grain."""
    w, h = img.size
    up = img.resize((w * scale, h * scale), Image.LANCZOS)
    a = np.asarray(img.convert("RGB")).astype(np.float64)
    lum = a @ np.array([0.299, 0.587, 0.114])
    F = np.fft.fft2(lum - lum.mean())
    half = min(w, h) // 2
    ps = _radial_power(F, half)
    # fit P(f) ~ C * f^s over the upper-middle band of the original (f in [half/4, half*0.9])
    f = np.arange(half)
    lo, hi = max(2, half // 4), max(4, int(half * 0.9))
    s, logc = np.polyfit(np.log(f[lo:hi]), np.log(ps[lo:hi] + 1e-12), 1)
    s = float(np.clip(s, -4.0, -0.2))
    # noise on the upscaled grid, shaped to the extended law only above the original's Nyquist (index `half`)
    H, W = h * scale, w * scale
    rng = np.random.default_rng(seed)
    N = np.fft.fft2(rng.standard_normal((H, W)))
    fy = np.fft.fftfreq(H)[:, None] * H
    fx = np.fft.fftfreq(W)[None, :] * W
    r = np.hypot(fx, fy)
    fn = half
    band = (r > fn) & (r <= fn * scale * 1.0)
    shape = np.zeros_like(r)
    shape[band] = (r[band] / fn) ** (s / 2)          # amplitude = sqrt(power law), continuous at the old Nyquist
    D = np.real(np.fft.ifft2(N * shape))
    # amplitude: the original's top octave sets the level, the power law says how much the next octave carries
    orig_top = (np.hypot(np.fft.fftfreq(w)[None, :] * w, np.fft.fftfreq(h)[:, None] * h) > fn / 2)
    top_std = float(np.sqrt((np.abs(F[orig_top]) ** 2).sum()) / (w * h))
    lo_pw = float(np.sum((np.arange(int(fn / 2), fn) + 0.5) ** s * (np.arange(int(fn / 2), fn) + 0.5)))
    hi_pw = float(np.sum((np.arange(fn, int(fn * scale)) + 0.5) ** s * (np.arange(fn, int(fn * scale)) + 0.5)))
    target = top_std * np.sqrt(hi_pw / max(lo_pw, 1e-12)) * strength
    D *= target / max(float(D.std()), 1e-9)
    out = np.asarray(up).astype(np.float64) + D[..., None]
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))
