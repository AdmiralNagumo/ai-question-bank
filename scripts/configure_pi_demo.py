#!/usr/bin/env python3
"""只为本地演示补充缺失配置；密钥不打印、不进入 Git。"""
import os
import re
import secrets
from pathlib import Path

root = Path(__file__).resolve().parents[1]
def read(path):
    if not path.exists(): return {}
    return dict(line.split('=', 1) for line in path.read_text(encoding='utf-8').splitlines()
                if '=' in line and not line.lstrip().startswith('#'))

server = root / 'server/.env'
pi = root / 'ai-service/.env'
s = read(server)
p = read(pi)
tokens = {value for value in (s.get('AI_SERVICE_TOKEN'), p.get('AI_SERVICE_TOKEN')) if value}
if len(tokens) > 1: raise SystemExit('两个服务的 AI_SERVICE_TOKEN 不一致，请手动核对，未修改配置。')
token = next(iter(tokens)) if tokens else secrets.token_hex(32)
if not re.fullmatch(r'[A-Za-z0-9_-]{32,128}', token): raise SystemExit('已有服务凭证格式不正确，未修改配置。')

def append_missing(path, current, defaults):
    missing = {key: value for key, value in defaults.items() if not current.get(key)}
    if not missing: return
    previous = path.read_text(encoding='utf-8') if path.exists() else ''
    # 删除空占位行，防止 Node .env 解析或 C overwrite=0 行为优先采用空值。
    lines = [line for line in previous.splitlines() if not any(line == key + '=' for key in missing)]
    with open(path, 'w', encoding='utf-8', newline='\n') as stream:
        stream.write('\n'.join(lines) + '\n')
        stream.writelines(key + '=' + value + '\n' for key, value in missing.items())
    os.chmod(path, 0o600)

append_missing(server, s, {'AI_SERVICE_URL': 'http://127.0.0.1:8001', 'AI_SERVICE_TOKEN': token})
append_missing(pi, p, {'AI_SERVICE_TOKEN': token, 'AI_SERVICE_HOST': '127.0.0.1',
                      'AI_SERVICE_PORT': '8001', 'AI_MOCK': '1'})
print('已补充本地 Pi Mock 配置；已有配置已保留，内部凭证不显示。')
