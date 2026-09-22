#!/bin/bash
# CoreDevice 触摸/截图探针启动器
set -e
cd "$(dirname "$0")"

# 向上查找 venv，避免依赖固定层级
VENV=""
d="$(pwd)"
while [ "$d" != "/" ]; do
    if [ -x "$d/.probe-venv/bin/python" ]; then VENV="$d/.probe-venv/bin/python"; break; fi
    d="$(dirname "$d")"
done
[ -n "$VENV" ] || { echo "找不到 .probe-venv，先执行: python3 -m venv .probe-venv && .probe-venv/bin/pip install pymobiledevice3 pillow numpy"; exit 1; }

exec "$VENV" "${1:-probe.py}" "$(pwd)"
