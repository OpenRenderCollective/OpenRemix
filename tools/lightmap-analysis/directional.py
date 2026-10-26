"""Offline directional-lightmap controls, not a runtime format or encoder.

Inputs are OpenRemix's cosine-convolved RGB SH coefficients in the stored order
0, y, z, x, xy, yz, 3z*z-1, xz, x*x-y*y. No quantization or denoising is modeled.
"""

from dataclasses import dataclass

import numpy as np


def unit_vectors(values):
    values = np.asarray(values, dtype=np.float64)
    if values.shape[-1:] != (3,) or not np.isfinite(values).all():
        raise ValueError("Expected finite three-component directions")
    lengths = np.linalg.norm(values, axis=-1, keepdims=True)
    if np.any(lengths < 1e-12):
        raise ValueError("Direction has zero length")
    return values / lengths


@dataclass(frozen=True)
class Polynomial:
    constant: np.ndarray  # (..., RGB)
    linear: np.ndarray  # (..., RGB, xyz)
    quadratic: np.ndarray  # (..., RGB, xyz, xyz), symmetric and trace-free

    def evaluate(self, directions):
        """Evaluate matching/broadcastable leading dimensions, before clamping."""
        n = unit_vectors(directions)
        return (self.constant + np.einsum("...ci,...i->...c", self.linear, n)
                + np.einsum("...i,...cij,...j->...c", n, self.quadratic, n))


def from_sh(coefficients):
    sh = np.asarray(coefficients, dtype=np.float64)
    if sh.shape[-2:] != (9, 3) or not np.isfinite(sh).all():
        raise ValueError("Expected finite (..., 9, 3) RGB SH coefficients")
    q = np.zeros(sh.shape[:-2] + (3, 3, 3))
    q[..., :, 0, 0] = -.3153915653 * sh[..., 6, :] + .5462742153 * sh[..., 8, :]
    q[..., :, 1, 1] = -.3153915653 * sh[..., 6, :] - .5462742153 * sh[..., 8, :]
    q[..., :, 2, 2] = 2 * .3153915653 * sh[..., 6, :]
    for row, column, band in [(0, 1, 4), (1, 2, 5), (0, 2, 7)]:
        q[..., :, row, column] = q[..., :, column, row] = .5462742155 * sh[..., band, :]
    return Polynomial(sh[..., 0, :] * .2820947918,
                      np.swapaxes(sh[..., [3, 1, 2], :], -1, -2) * .4886025119, q)


@dataclass(frozen=True)
class SurfaceFit:
    """Fifteen scalar fields; geometric normal is an external decode input.

    Interpolation of these fields is intentionally exposed for qualification.
    Exact per-texel value/gradient preservation does not prove filtered accuracy.
    """

    constant: np.ndarray
    linear: np.ndarray
    zonal: np.ndarray

    def polynomial(self, geometric_normal):
        n = unit_vectors(geometric_normal)
        shape = 3 * np.einsum("...i,...j->...ij", n, n) - np.eye(3)
        q = self.zonal[..., :, None, None] * shape[..., None, :, :]
        return Polynomial(self.constant, self.linear, q)


def fit_surface(source, geometric_normal):
    """Preserve value and angular gradient at one unit geometric normal.

    The unclamped residual at direction v is (v-n)^T R (v-n), where R is the
    discarded quadratic matrix. This is a local approximation, not an SH fit
    valid for arbitrary normals or a reproduction of Valve's packed encoding.
    """
    n = unit_vectors(geometric_normal)
    zonal = .5 * np.einsum("...i,...cij,...j->...c", n, source.quadratic, n)
    shape = 3 * np.einsum("...i,...j->...ij", n, n) - np.eye(3)
    residual = source.quadratic - zonal[..., :, None, None] * shape[..., None, :, :]
    constant = source.constant - np.einsum("...i,...cij,...j->...c", n, residual, n)
    linear = source.linear + 2 * np.einsum("...cij,...j->...ci", residual, n)
    return SurfaceFit(constant, linear, zonal)


def interpolate(a, b, fraction):
    """Linear texture-filter control; does not interpolate external normals."""
    t = np.asarray(fraction, dtype=np.float64)
    if t.ndim or not np.isfinite(t) or not 0 <= t <= 1:
        raise ValueError("Expected an interpolation fraction in [0, 1]")
    return SurfaceFit(*((1 - t) * x + t * y for x, y in
                        [(a.constant, b.constant), (a.linear, b.linear), (a.zonal, b.zonal)]))
