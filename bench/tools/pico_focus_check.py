"""Conservative log-only focus gate. Passing never verifies captured pixels."""
import re

def focus_status(text):
    state = None
    rendered = []
    for line in text.splitlines():
        match = re.search(r'Session state changed.*=> (XR_SESSION_STATE_[A-Z_]+)', line)
        if match:
            state = match[1]
        if re.search(r'render: \d+ iterations in ', line):
            rendered.append(state)
    focused = sum(s == 'XR_SESSION_STATE_FOCUSED' for s in rendered)
    unknown = sum(s is None for s in rendered)
    return {
        'last_session_state': state,
        'observed_render_windows': len(rendered),
        'focused_render_windows': focused,
        'unknown_render_windows': unknown,
        'unfocused_render_windows': len(rendered) - focused - unknown,
        'focus_gate_passed': bool(rendered) and focused == len(rendered) and state == 'XR_SESSION_STATE_FOCUSED',
        'visual_verified': False,
        'visual_verification_note': 'Focus logs cannot verify pixels, tracking quality, or physical display cadence; inspect an actual capture.',
    }

if __name__ == '__main__':
    import json, sys
    from pathlib import Path
    print(json.dumps(focus_status(Path(sys.argv[1]).read_text(errors='replace')), indent=2))
