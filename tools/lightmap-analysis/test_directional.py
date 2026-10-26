"""Mathematical controls for the offline representation study."""

import unittest

import numpy as np

from directional import Polynomial, fit_surface, from_sh, interpolate, unit_vectors


class SurfaceFitTests(unittest.TestCase):
    def setUp(self):
        rng = np.random.default_rng(26719)
        self.source = from_sh(rng.normal(size=(64, 9, 3)))
        self.normals = unit_vectors(rng.normal(size=(64, 3)))

    def test_value_and_angular_derivative(self):
        fit = fit_surface(self.source, self.normals).polynomial(self.normals)
        np.testing.assert_allclose(fit.evaluate(self.normals), self.source.evaluate(self.normals), atol=1e-14)
        # A field's tangential gradient is the projection of l + 2Qn.
        def gradient(p):
            g = p.linear + 2 * np.einsum("ncij,nj->nci", p.quadratic, self.normals)
            return g - np.einsum("nci,ni->nc", g, self.normals)[..., None] * self.normals[:, None, :]
        np.testing.assert_allclose(gradient(fit), gradient(self.source), atol=1e-14)

    def test_residual_identity(self):
        fit = fit_surface(self.source, self.normals).polynomial(self.normals)
        directions = np.roll(self.normals, 1, axis=1)
        delta = directions - self.normals
        expected = np.einsum("ni,ncij,nj->nc", delta, self.source.quadratic - fit.quadratic, delta)
        np.testing.assert_allclose(self.source.evaluate(directions) - fit.evaluate(directions), expected, atol=1e-14)

    def test_constant_field(self):
        sh = np.zeros((9, 3))
        sh[0] = [1, 2, 3]
        source = from_sh(sh)
        fitted = fit_surface(source, [0, 0, 1])
        np.testing.assert_array_equal(fitted.zonal, np.zeros(3))
        np.testing.assert_array_equal(fitted.polynomial([0, 0, 1]).evaluate([1, 0, 0]), source.constant)

    def test_fixed_normal_filtering_commutes(self):
        a = Polynomial(self.source.constant[0], self.source.linear[0], self.source.quadratic[0])
        b = Polynomial(self.source.constant[1], self.source.linear[1], self.source.quadratic[1])
        n = self.normals[0]
        blended = Polynomial(*(x * .25 + y * .75 for x, y in
                              [(a.constant, b.constant), (a.linear, b.linear), (a.quadratic, b.quadratic)]))
        mixed = interpolate(fit_surface(a, n), fit_surface(b, n), .75)
        expected = fit_surface(blended, n)
        np.testing.assert_allclose(mixed.constant, expected.constant, atol=1e-14)
        np.testing.assert_allclose(mixed.linear, expected.linear, atol=1e-14)
        np.testing.assert_allclose(mixed.zonal, expected.zonal, atol=1e-14)

    def test_invalid_inputs(self):
        for normal in [[0, 0, 0], [1, float("nan"), 0]]:
            with self.assertRaises(ValueError):
                fit_surface(self.source, normal)
        with self.assertRaises(ValueError):
            from_sh(np.zeros((8, 3)))

    def test_varying_normal_filter_is_not_exact(self):
        # Identical world-space lighting at both texels does not suffice when
        # their local encode normals differ. Preserve this counterexample.
        sh = np.zeros((9, 3))
        sh[0] = 4
        sh[8] = [1, .5, .25]
        source = from_sh(sh)
        a, b = unit_vectors([0, 0, 1]), unit_vectors([1, 0, 1])
        n = unit_vectors(a + b)
        filtered = interpolate(fit_surface(source, a), fit_surface(source, b), .5)
        error = filtered.polynomial(n).evaluate(n) - source.evaluate(n)
        self.assertGreater(np.max(np.abs(error)), .001)

    def test_rotation_equivariance(self):
        angle = .73
        rotation = np.array([[np.cos(angle), -np.sin(angle), 0],
                             [np.sin(angle), np.cos(angle), 0], [0, 0, 1]])
        rotated = Polynomial(self.source.constant,
                             self.source.linear @ rotation.T,
                             np.einsum("ij,ncjk,lk->ncil", rotation, self.source.quadratic, rotation))
        expected = fit_surface(self.source, self.normals)
        actual = fit_surface(rotated, self.normals @ rotation.T)
        np.testing.assert_allclose(actual.constant, expected.constant, atol=1e-14)
        np.testing.assert_allclose(actual.zonal, expected.zonal, atol=1e-14)
        np.testing.assert_allclose(actual.linear, expected.linear @ rotation.T, atol=1e-14)


if __name__ == "__main__":
    unittest.main()
