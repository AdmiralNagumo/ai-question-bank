#!/usr/bin/env python3
"""只补充本地教师Key加密主密钥；不打印、不覆盖、不迁移题库数据。"""
from pathlib import Path
import os
import re
import secrets

path=Path(__file__).resolve().parents[1]/'server/.env'
text=path.read_text(encoding='utf-8') if path.exists() else ''
values=[line.split('=',1)[1] for line in text.splitlines() if line.startswith('AI_KEY_ENCRYPTION_KEY=')]
if len(values)>1:
    raise SystemExit('发现重复主密钥配置，请先人工核对；未修改。')
existing=os.environ.get('AI_KEY_ENCRYPTION_KEY') or (values[0] if values else '')
if existing:
    if not re.fullmatch(r'[0-9a-fA-F]{64}',existing): raise SystemExit('已有主密钥格式无效，未覆盖。')
    print('已有主密钥已保留；未显示密钥。')
else:
    lines=[line for line in text.splitlines() if line!='AI_KEY_ENCRYPTION_KEY=']
    path.write_text('\n'.join(lines)+'\nAI_KEY_ENCRYPTION_KEY='+secrets.token_hex(32)+'\n',encoding='utf-8')
    path.chmod(0o600)
    print('已生成独立本地主密钥，请与数据库分开备份；未显示密钥。')
