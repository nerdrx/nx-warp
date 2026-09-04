"""Synthetic test-material generation."""

from __future__ import annotations

import numpy as np
import pytest

from capture import font5x7, synth


class TestFont:
    def test_glyph_shape(self):
        assert font5x7.glyph("A").shape == (7, 5)

    def test_space_is_blank_and_letters_are_not(self):
        assert not font5x7.glyph(" ").any()
        assert font5x7.glyph("A").any()

    def test_case_insensitive(self):
        assert np.array_equal(font5x7.glyph("a"), font5x7.glyph("A"))

    def test_unknown_character_gets_a_box(self):
        assert font5x7.glyph("§").any()

    def test_text_mask_size_matches_prediction(self):
        for s in (1, 3):
            m = font5x7.text_mask("NX WARP", scale=s)
            assert m.shape == font5x7.text_size("NX WARP", scale=s)

    def test_scale_repeats_pixels(self):
        m1 = font5x7.text_mask("I", 1)
        m2 = font5x7.text_mask("I", 2)
        assert m2.shape == (m1.shape[0] * 2, m1.shape[1] * 2)
        assert m2.sum() == m1.sum() * 4

    def test_draw_text_writes_pixels(self):
        img = np.zeros((32, 128, 3), np.uint8)
        font5x7.draw_text(img, "NX", 4, 4, (255, 255, 255), scale=2)
        assert img.any()

    def test_draw_text_clips_instead_of_raising(self):
        img = np.zeros((10, 10, 3), np.uint8)
        font5x7.draw_text(img, "LONG TEXT", 8, 8)
        font5x7.draw_text(img, "OFFSCREEN", -50, -50)
        font5x7.draw_text(img, "OFFSCREEN", 500, 500)

    def test_empty_text(self):
        assert font5x7.text_mask("").size > 0


@pytest.fixture(scope="module")
def pano():
    """One panorama shared by the whole module: building it is the slow part."""
    return synth.make_panorama(512, 256, seed=1)


class TestPanorama:
    def test_shape_and_dtype(self, pano):
        assert pano.shape == (256, 512, 3)
        assert pano.dtype == np.uint8

    def test_deterministic(self):
        a = synth.make_panorama(256, 128, seed=5)
        b = synth.make_panorama(256, 128, seed=5)
        assert np.array_equal(a, b)

    def test_seed_changes_content(self):
        a = synth.make_panorama(256, 128, seed=5)
        b = synth.make_panorama(256, 128, seed=6)
        assert not np.array_equal(a, b)

    def test_contains_dark_and_bright_regions(self, pano):
        luma = pano.mean(axis=2)
        assert luma.min() < 20, "needs a near-black region for the banding test"
        assert luma.max() > 230, "needs highlights"

    def test_has_high_frequency_content(self, pano):
        """Adjacent-pixel differences must be large somewhere: checkers/zone plate."""
        d = np.abs(np.diff(pano.mean(axis=2), axis=1))
        assert d.max() > 150

    def test_full_dynamic_range_in_all_channels(self, pano):
        for c in range(3):
            assert np.ptp(pano[..., c]) > 180


class TestPoses:
    def test_length_and_keys(self):
        p = synth.make_poses(5, "pan")
        assert len(p) == 5
        for k in ("frame", "time_s", "position_xyz", "orientation_xyzw",
                  "yaw_deg", "angular_velocity_deg_s"):
            assert k in p[0]

    def test_quaternions_are_unit(self):
        for pose in synth.make_poses(10, "turn"):
            q = np.asarray(pose["orientation_xyzw"])
            assert np.linalg.norm(q) == pytest.approx(1.0, abs=1e-9)

    def test_first_frame_has_zero_velocity(self):
        assert synth.make_poses(4, "turn")[0]["angular_velocity_deg_s"] == 0.0

    def test_unknown_motion(self):
        with pytest.raises(ValueError, match="unknown motion"):
            synth.make_poses(3, "spin")

    @pytest.mark.parametrize("motion", synth.MOTIONS)
    def test_rates_are_clip_length_independent(self, motion):
        """The whole point of rate-based profiles: 10 and 300 frames agree."""
        short = max(p["angular_velocity_deg_s"] for p in synth.make_poses(10, motion))
        long = max(p["angular_velocity_deg_s"] for p in synth.make_poses(300, motion))
        ceiling = synth.PEAK_YAW_RATE[motion] + 40.0  # drift and pitch add a little
        assert short <= ceiling
        assert long <= ceiling

    def test_static_is_nearly_still(self):
        av = [p["angular_velocity_deg_s"] for p in synth.make_poses(60, "static")]
        assert max(av) < 25.0

    def test_turn_actually_turns(self):
        av = [p["angular_velocity_deg_s"] for p in synth.make_poses(90, "turn")]
        assert max(av) > 100.0

    def test_mixed_has_both_rest_and_motion(self):
        av = np.array([p["angular_velocity_deg_s"] for p in synth.make_poses(90, "mixed")])
        assert av.min() < 20.0
        assert av.max() > 80.0
        # the split the Phase 2 kill test relies on must be meaningful
        assert np.percentile(av, 80) > np.percentile(av, 20) + 20.0

    def test_deterministic(self):
        assert synth.make_poses(6, "mixed") == synth.make_poses(6, "mixed")


class TestRotation:
    def test_identity(self):
        assert synth.rot_matrix(0, 0, 0) == pytest.approx(np.eye(3))

    def test_orthonormal(self):
        r = synth.rot_matrix(0.3, -0.2, 0.1)
        assert r @ r.T == pytest.approx(np.eye(3), abs=1e-12)
        assert np.linalg.det(r) == pytest.approx(1.0)

    def test_yaw_turns_forward_vector_sideways(self):
        r = synth.rot_matrix(np.pi / 2, 0, 0)
        fwd = r @ np.array([0.0, 0.0, -1.0])
        assert fwd == pytest.approx(np.array([-1.0, 0.0, 0.0]), abs=1e-12)


class TestRendering:
    def test_equirect_sampling_shape(self, pano):
        cam = synth.Camera(32, 32)
        d = synth._ray_grid(cam)
        out = synth.sample_equirect(pano, d)
        assert out.shape == (32, 32, 3)

    def test_ray_grid_is_unit_length(self):
        d = synth._ray_grid(synth.Camera(16, 16))
        assert np.linalg.norm(d, axis=-1) == pytest.approx(np.ones((16, 16)), abs=1e-6)

    def test_ray_grid_centre_looks_forward(self):
        d = synth._ray_grid(synth.Camera(65, 65))
        assert d[32, 32] == pytest.approx(np.array([0, 0, -1]), abs=1e-6)

    def test_render_view_shape_and_dtype(self, pano):
        cam = synth.Camera(32, 32)
        pose = synth.make_poses(1, "static")[0]
        img = synth.render_view(pano, cam, pose, None, 0)
        assert img.shape == (32, 32, 3)
        assert img.dtype == np.uint8

    def test_stereo_sbs_is_double_width(self, pano):
        cam = synth.Camera(32, 24)
        pose = synth.make_poses(1, "static")[0]
        assert synth.render_stereo(pano, cam, pose, None, "sbs").shape == (24, 64, 3)
        assert synth.render_stereo(pano, cam, pose, None, "mono").shape == (24, 32, 3)

    def test_eyes_differ_only_where_near_objects_are(self, pano):
        """A panorama is at infinity, so rotation-only views must match exactly."""
        cam = synth.Camera(32, 32)
        pose = synth.make_poses(1, "static")[0]
        left = synth.render_view(pano, cam, pose, None, 0, hud=False)
        right = synth.render_view(pano, cam, pose, None, 1, hud=False)
        assert np.array_equal(left, right)

    def test_near_objects_create_stereo_disparity(self, pano):
        cam = synth.Camera(64, 64)
        pose = synth.make_poses(1, "static")[0]
        objs = synth.Objects(count=7, seed=11)
        left = synth.render_view(pano, cam, pose, objs, 0, hud=False)
        right = synth.render_view(pano, cam, pose, objs, 1, hud=False)
        assert not np.array_equal(left, right)

    def test_head_rotation_changes_the_view(self, pano):
        cam = synth.Camera(32, 32)
        poses = synth.make_poses(30, "turn")
        a = synth.render_view(pano, cam, poses[0], None, 0, hud=False)
        b = synth.render_view(pano, cam, poses[-1], None, 0, hud=False)
        assert not np.array_equal(a, b)

    def test_rendering_is_deterministic(self, pano):
        cam = synth.Camera(32, 32)
        pose = synth.make_poses(1, "mixed")[0]
        objs = synth.Objects(count=4, seed=11)
        a = synth.render_stereo(pano, cam, pose, objs)
        b = synth.render_stereo(pano, cam, pose, objs)
        assert np.array_equal(a, b)


class TestObjects:
    def test_positions_stay_in_the_box(self):
        objs = synth.Objects(count=9, seed=3)
        for t in np.linspace(0, 30, 40):
            p = objs.positions(float(t))
            assert np.all(p[:, 0] >= -1.5001) and np.all(p[:, 0] <= 1.5001)
            assert np.all(p[:, 1] >= -0.9001) and np.all(p[:, 1] <= 0.9001)
            assert np.all(p[:, 2] >= -3.2001) and np.all(p[:, 2] <= -0.3499)

    def test_objects_move(self):
        objs = synth.Objects(count=5, seed=3)
        assert not np.allclose(objs.positions(0.0), objs.positions(1.0))
