"""Check transport of a motion field sampled at the current destination.

The estimator's d(p) obeys current(p) = previous(p - d(p)).  For a future
sample p, using the field at the unknown source q requires q = p - d(q).
Sampling d(p) directly is only exact for translations.
"""
import numpy as np


def exact_source(p, a, b):
    return np.linalg.solve(np.eye(2) + a, (p - b).T).T


def fixed_point(p, a, b, steps, damping=0.5):
    # Start from the production q=p-d(p) estimate, then transport d at q.
    q = p - (p @ a.T + b)
    for _ in range(steps):
        target = p - (q @ a.T + b)
        q = (1.0 - damping) * q + damping * target
    return q


def check(name, a, b):
    points = np.array([[-.7, -.4], [.2, -.6], [.8, .1], [-.3, .75], [.65, .7]])
    truth = exact_source(points, a, b)
    direct = points - (points @ a.T + b)
    six = fixed_point(points, a, b, 6)
    many = fixed_point(points, a, b, 60)
    direct_err = np.max(np.linalg.norm(direct - truth, axis=1))
    six_err = np.max(np.linalg.norm(six - truth, axis=1))
    many_err = np.max(np.linalg.norm(many - truth, axis=1))
    print(f"{name}: direct={direct_err:.3e} six_damped={six_err:.3e} 60_steps={many_err:.3e}")
    assert six_err <= direct_err + 1e-12
    assert many_err < 1e-10


def main():
    check("translation", np.zeros((2, 2)), np.array([.08, -.035]))
    a = np.array([[.12, .04], [-.03, .10]])
    check("contractive_affine", a, np.array([.08, -.035]))
    # The fixed point is not guaranteed to converge for a large/non-invertible
    # flow; this test intentionally stays in the ||A|| < 1 regime.
    print("PASS: d(p) direct sampling has affine anchoring error; damped transport converges")


if __name__ == "__main__":
    main()
