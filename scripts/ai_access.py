#!/usr/bin/env python3
"""Trusted operator CLI. No HTTP write endpoint and no model credentials."""
import argparse
import datetime as dt
import getpass
import json
from pathlib import Path
import sqlite3
import sys
import time


def arguments():
    parser = argparse.ArgumentParser(description='管理独立AI授权；先启动C后端初始化数据库。')
    parser.add_argument('--db', required=True, type=Path, help='实际C数据库路径，不自动创建数据库')
    commands = parser.add_subparsers(dest='action', required=True)
    for action in ['grant', 'revoke', 'status']:
        command = commands.add_parser(action)
        command.add_argument('--username', required=True)
        if action != 'status': command.add_argument('--reason', required=True)
        if action == 'grant': command.add_argument('--expires-at', help='带时区的ISO8601时间，不填为永久授权')
    return parser.parse_args()


def expiry(value):
    if value is None: return None
    parsed = dt.datetime.fromisoformat(value.replace('Z', '+00:00'))
    if parsed.tzinfo is None or parsed.microsecond:
        raise ValueError('expires-at须带时区且精确到秒')
    seconds = int(parsed.timestamp())
    if not int(time.time()) < seconds <= 253402300799:
        raise ValueError('expires-at须为未来的有效时间')
    return seconds


def identity(connection, username):
    rows = connection.execute("SELECT value FROM users WHERE key_name=? AND value_name='id'", (username,)).fetchall()
    if len(rows) != 1 or not isinstance(rows[0][0], str) or not rows[0][0].isascii() or not rows[0][0].isdecimal():
        raise ValueError('账号不存在或ID无效')
    user_id = int(rows[0][0])
    if not 1 <= user_id <= 2147483647 or str(user_id) != rows[0][0]: raise ValueError('ID无效')
    count = connection.execute("SELECT count(*) FROM users WHERE value_name='id' AND value=?", (str(user_id),)).fetchone()[0]
    if count != 1: raise ValueError('用户ID重复，须先修复用户数据')
    return user_id


def operate(args):
    if not args.username or '\0' in args.username: raise ValueError('用户名无效')
    if args.action != 'status' and (not args.reason.strip() or len(args.reason) > 200 or
                                   any(ord(char) < 32 for char in args.reason)):
        raise ValueError('reason须为1-200个可显示字符，不能含控制字符')
    expires = expiry(args.expires_at) if args.action == 'grant' else None
    # mode=rw避免路径拼错时创建空库；URI自动编码文件路径。
    connection = sqlite3.connect(args.db.resolve().as_uri() + ('?mode=ro' if args.action == 'status' else '?mode=rw'), uri=True, timeout=2)
    try:
        if args.action != 'status': connection.execute('BEGIN IMMEDIATE')
        user_id = identity(connection, args.username)
        current = connection.execute('SELECT enabled,expires_at,user_key FROM ai_access_grants WHERE user_id=?', (user_id,)).fetchone()
        if args.action != 'status':
            if args.action == 'revoke' and current and current[2] == args.username: expires = current[1]
            connection.execute('INSERT INTO ai_access_grants(user_id,user_key,enabled,expires_at,updated_at) VALUES(?,?,?,?,?) '
                               'ON CONFLICT(user_id) DO UPDATE SET user_key=excluded.user_key,enabled=excluded.enabled,'
                               'expires_at=excluded.expires_at,updated_at=excluded.updated_at',
                               (user_id,args.username,int(args.action == 'grant'),expires,int(time.time())))
            connection.execute('INSERT INTO ai_access_audit(user_id,user_key,action,expires_at,actor,reason,created_at) VALUES(?,?,?,?,?,?,?)',
                               (user_id,args.username,args.action,expires,getpass.getuser(),args.reason,int(time.time())))
            connection.commit()
            current = (int(args.action == 'grant'), expires, args.username)
        status = 'not_granted' if not current or current[2] != args.username else 'revoked' if not current[0] else 'expired' if current[1] is not None and current[1] <= int(time.time()) else 'active'
        timestamp = current[1] if current and current[2] == args.username else None
        return {'user_id':user_id,'username':args.username,'ai_access':{'allowed':status == 'active','status':status,
                'expires_at':dt.datetime.fromtimestamp(timestamp,dt.timezone.utc).isoformat().replace('+00:00','Z') if timestamp is not None else None}}
    finally:
        connection.close()  # 发生异常时未提交的授权与审计同时回滚。


if __name__ == '__main__':
    try: print(json.dumps(operate(arguments()),ensure_ascii=False))
    except (sqlite3.Error,ValueError,OSError,OverflowError):
        print('授权操作失败：检查数据库路径、初始化状态、账号、唯一ID或时间格式。未完成的变更已回滚。',file=sys.stderr)
        sys.exit(1)
