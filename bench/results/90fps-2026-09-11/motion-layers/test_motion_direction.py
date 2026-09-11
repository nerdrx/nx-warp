"""Regression: a current->previous inverse map is reusable for constant motion.

For a constant frame transform T, frame k coordinates are T^k x.  The map from
current (k=1) to previous (k=0) is A=T^-1.  The next frame at coordinate p samples
the current frame at A p, so the same A predicts one more equal interval.
"""
import numpy as np


def affine(tx=0.0, ty=0.0, angle=0.0):
    c, s = np.cos(angle), np.sin(angle)
    return np.array([[c, -s, tx], [s, c, ty], [0.0, 0.0, 1.0]])


def apply(m, pts):
    h = np.c_[pts, np.ones(len(pts))]
    q = (m @ h.T).T
    return q[:, :2] / q[:, 2:3]


def check(name, step):
    # Points avoid the rotation centre so sign errors cannot hide.
    x = np.array([[-.7,-.4], [.2,-.6], [.8,.1], [-.3,.75], [.65,.7]])
    previous = apply(np.eye(3), x)
    current = apply(step, x)
    future = apply(step @ step, x)
    current_to_previous = np.linalg.inv(step)
    # Extrapolate future at destination p by sampling current at A p.
    predicted = apply(current_to_previous, future)
    error = float(np.max(np.linalg.norm(predicted - current, axis=1)))
    assert error < 1e-12, (name, error)
    # The displacement convention used by the shader is p - A p.
    probe = future[2]
    d = probe - apply(current_to_previous, probe[None, :])[0]
    print(f"{name}: max_error={error:.3e}, probe_displacement={d}")


def main():
    check("translation", affine(.08, -.035))
    check("rotation", affine(angle=np.deg2rad(7.0)))
    # A changing projective transform is not generally reusable.  A depth-like
    # projective term makes the second step differ from the first.
    h = np.array([[1.0, .0, .04], [.0, 1.0, -.02], [.001, .0007, 1.0]])
    x = np.array([[-.7,-.4], [.2,-.6], [.8,.1], [-.3,.75], [.65,.7]])
    h2 = np.array([[1.0, .0, .08], [.0, 1.0, -.04], [.0015, .0002, 1.0]])
    same = apply(np.linalg.inv(h), apply(h2 @ h, x))
    one_more = apply(h, x)
    residual = float(np.max(np.linalg.norm(same - one_more, axis=1)))
    print(f"changing_projective_transform: residual={residual:.3e} (expected nonzero)")
    assert residual > 1e-5
    print("PASS: constant rigid step reuses current->previous inverse; varying projective step does not")


if __name__ == "__main__":
    main()
