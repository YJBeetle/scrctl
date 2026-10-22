"""Run the actual scrctl frontend through the public XDB1 protocol."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'xdb/tests/support'))
from fake_server import FakeServer

exe = str(Path(sys.argv[1]).resolve())
with FakeServer() as server:
    env = {**os.environ, **server.environment, 'SDL_VIDEODRIVER':'dummy', 'SDL_AUDIODRIVER':'dummy', 'SDL_RENDER_DRIVER':'software'}
    def run(*args):
        result = subprocess.run([exe, '--lang', 'en', *args], env=env, capture_output=True, text=True, timeout=12)
        assert result.returncode == 0, (args, result.stdout, result.stderr)
        return result.stdout
    run('--list-devices', '--discovery-timeout', '0')
    # Actual background BGRA receiver, ASCII input and finalized media ownership.
    run('-s', 'mock-iphone', '--no-window', '--no-audio', '--exit-after', '2', '--test-type', 'abc123')
    assert 'video.frame' in server.methods() and 'input.text' in server.methods()
    # PNG decoding occurs in the server; the UI receives upright BGRA geometry.
    run('-s', 'mock-iphone', '--video-source', 'screenshot', '--no-window', '--no-audio', '--exit-after', '2')
    assert any(name == 'screenshot' and p.get('format') == 'bgra' for name,p in server.calls)
    run('-s', 'mock-iphone', '--copy', '中文✓🙂')
    assert run('-s', 'mock-iphone', '--paste').strip() == '中文✓🙂'
    # The audio callback reads only the local queue; a separate attached client polls PCM.
    run('-s', 'mock-iphone', '--no-window', '--no-video', '--time-limit', '1', '--audio-dup', '--audio-buffer', '200')
    assert 'audio.read' in server.methods()
    assert any(name == 'audio.start' and p['duplicate'] and p['buffer_ms'] == 200 for name,p in server.calls)
    # Server-side encoded recording remains usable without local decode or playback.
    run('-s', 'mock-iphone', '--no-window', '--no-playback', '--record', str(root/'build-xdb/mock-recording.mkv'), '--time-limit', '1')
    assert 'recording.configure' in server.methods()
    assert any(name == 'video.start' and p['decode'] is False for name,p in server.calls)
    assert 'media.finish' in server.methods()
    assert not {'pair', 'pairing.import', 'ddi.prepare'} & set(server.methods())
print('PASS: scrctl BGRA/PNG, ASCII, explicit clipboard, PCM attachment and encoded recording routing')
