import unittest
from pico_focus_check import focus_status

def state(name): return 'Session state changed at timestamp 0: XR_SESSION_STATE_IDLE => XR_SESSION_STATE_' + name
render = 'render: 180 iterations in 2.0 s (90.0/s)'
class FocusCheck(unittest.TestCase):
    def test_focused_is_not_visual_proof(self):
        s=focus_status(state('FOCUSED')+'\n'+render)
        self.assertTrue(s['focus_gate_passed']); self.assertFalse(s['visual_verified'])
    def test_visible_overlay_is_not_focused(self):
        self.assertFalse(focus_status(state('VISIBLE')+'\n'+render)['focus_gate_passed'])
    def test_missing_logs(self):
        self.assertFalse(focus_status(render)['focus_gate_passed'])
        self.assertFalse(focus_status('')['focus_gate_passed'])
    def test_lost_focus_without_new_render_window(self):
        self.assertFalse(focus_status(state('FOCUSED')+'\n'+render+'\n'+state('VISIBLE'))['focus_gate_passed'])
    def test_recovered_focus_does_not_erase_bad_window(self):
        self.assertFalse(focus_status(state('VISIBLE')+'\n'+render+'\n'+state('FOCUSED')+'\n'+render)['focus_gate_passed'])
if __name__ == '__main__': unittest.main()
