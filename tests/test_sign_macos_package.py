"""Exercise signing orchestration without accessing a real keychain or secrets."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'tools/sign_macos_package.sh'

class SigningTest(unittest.TestCase):
    def run_fixture(self, matching):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            commands = root / 'commands'
            commands.mkdir()
            package = root / 'package/bin'
            package.mkdir(parents=True)
            (package / 'scrctl').write_text('fixture')
            log = root / 'calls'
            fake = '''#!/bin/bash
case "${0##*/}" in
security)
  echo "security $1" >> "$CALLS"
  if [[ "$1" == list-keychains && "$*" != *' -s '* ]]; then echo '    "/fixture/login.keychain-db"'; fi
  if [[ "$1" == find-identity && "$MATCHING" == yes ]]; then echo '  1) ABCDEF "Developer ID Application: Fixture (TEAM)"'; fi
  ;;
file) echo 'Mach-O 64-bit executable';;
codesign)
  echo "codesign $*" >> "$CALLS"
  if [[ "$1" == -dvv ]]; then
    echo 'TeamIdentifier=TEAM flags=0x10000(runtime)' >&2
    echo 'Authority=Developer ID Application: Fixture (TEAM)' >&2
    echo 'Timestamp=fixture' >&2
    for ((i=0; i<2000; ++i)); do echo 'verbose certificate diagnostics' >&2; done
  fi
  ;;
esac
'''
            for name in ('security', 'file', 'codesign'):
                path = commands / name
                path.write_text(fake)
                path.chmod(0o755)
            env = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'],
                       CALLS=str(log), MATCHING='yes' if matching else 'no',
                       MACOS_CERTIFICATE_P12_BASE64='Zml4dHVyZQ==',
                       MACOS_CERTIFICATE_PASSWORD='fixture',
                       SIGNING_IDENTITY='Developer ID Application: Fixture (TEAM)', SIGNING_TEAM_ID='TEAM')
            result = subprocess.run(['/bin/bash', str(SCRIPT), str(package.parent)],
                                    env=env, text=True, capture_output=True)
            return result, log.read_text()

    def test_search_identity_and_cleanup(self):
        result, calls = self.run_fixture(True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('codesign --force --sign ABCDEF', calls)
        self.assertIn('--options runtime', calls)
        self.assertTrue(calls.rstrip().endswith('security delete-keychain'))
        self.assertGreaterEqual(calls.count('security list-keychains'), 3)

    def test_missing_identity_fails_before_signing(self):
        result, calls = self.run_fixture(False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('No valid identity matching', result.stderr)
        self.assertNotIn('codesign --force', calls)
        self.assertTrue(calls.rstrip().endswith('security delete-keychain'))

if __name__ == '__main__':
    unittest.main()
