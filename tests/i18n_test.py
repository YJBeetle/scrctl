"""以独立进程验证语言选择，避免 gettext 缓存掩盖初始化问题。"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
base = {k: v for k, v in os.environ.items() if k not in
        ('LANG', 'LC_ALL', 'LC_MESSAGES', 'LANGUAGE', 'SCRCTL_LOCALEDIR')}

def run(args, variables=None):
    return subprocess.run([binary, *args], env={**base, **(variables or {})},
                          capture_output=True, text=True, encoding="utf-8", timeout=10)

def help_is(chinese, variables=None, args=None):
    result = run(args or ['--help'], variables)
    assert result.returncode == 0, result.stderr
    expected = 'iOS 屏幕镜像与控制' if chinese else 'iOS screen mirroring and control'
    assert expected in result.stdout, result.stdout
    assert '--lang' in result.stdout
    if not chinese:
        assert not any('\u4e00' <= c <= '\u9fff' for c in result.stdout), result.stdout

help_is(False)
help_is(False, {'LANG': 'C'})
help_is(False, {'LANG': 'POSIX'})
help_is(False, {'LANG': 'fr_FR.UTF-8'})
help_is(True, {'LANG': 'zh_CN.UTF-8'})
help_is(True, {'LANG': 'zh-CN'})
help_is(False, {'LANG': 'zh_CN.UTF-8', 'LC_MESSAGES': 'en_US.UTF-8'})
help_is(True, {'LANG': 'en_US.UTF-8', 'LC_MESSAGES': 'zh_CN.UTF-8'})
help_is(False, {'LANG': 'zh_CN.UTF-8', 'LC_ALL': 'C'})
help_is(True, {'LANG': 'en_US.UTF-8', 'LC_MESSAGES': 'en', 'LC_ALL': 'zh_CN.UTF-8'})
help_is(True, {'LANG': 'zh_CN.UTF-8', 'LC_ALL': '', 'LC_MESSAGES': ''})
help_is(False, {'LANG': 'zh_CN.UTF-8'}, ['--lang=en', '--help'])
help_is(True, {'LC_ALL': 'C'}, ['--help', '--lang', 'zh-CN'])
help_is(False, {'LANGUAGE': 'zh_CN'}, ['--lang', 'en', '--help'])
help_is(False, {'LANG': 'en_US.UTF-8', 'LANGUAGE': 'zh_CN'})
help_is(True, {'LANGUAGE': 'en'}, ['--lang', 'zh-CN', '--help'])
help_is(True, {'LANG': 'zh_CN.UTF-8'}, ['--lang', 'auto', '--help'])
with tempfile.TemporaryDirectory() as missing_catalog:
    help_is(False, {'SCRCTL_LOCALEDIR': missing_catalog}, ['--lang', 'zh-CN', '--help'])

for language, expected in [('en', 'finite positive'), ('zh-CN', '有限正数')]:
    result = run(['--lang', language, '--scale', '0', '--wifi', '127.0.0.1'])
    assert result.returncode == 2 and expected in result.stderr, result.stderr
    # 语言选择不能把数字解析切成 locale 相关的小数逗号。
    result = run(['--lang', language, '--scale=0.5', '--version'], {'LC_ALL': 'fr_FR.UTF-8'})
    assert result.returncode == 0 and 'scrctl' in result.stdout, result.stderr
for args in [['--lang=invalid', '--help'], ['--lang', 'invalid'], ['--lang']]:
    assert run(args).returncode == 2
# 用户字符串不会被用于选择语言或翻译。
result = run(['--title', 'zh-CN', '--lang', 'en', '--version'])
assert result.returncode == 0
print('Auto default, locale priority, overrides, English fallback, help ordering and numeric parsing passed')
