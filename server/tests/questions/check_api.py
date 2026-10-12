#!/usr/bin/env python3
"""题目接口回归：真实 HTTP + JSON 结构验证，不依赖 grep 匹配。"""
import json
import os
import sys
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

# 强制直连 localhost，绕过系统代理对 127.0.0.1 的重定向
import urllib.request
proxy_handler = urllib.request.ProxyHandler({})
urllib.request.install_opener(urllib.request.build_opener(proxy_handler))

base = "http://127.0.0.1:" + os.getenv("TEST_PORT", "8099")

def request(path, token=None, method="GET", data=None):
    headers = {}
    if token is not None:
        headers["Authorization"] = "Bearer " + token
    if data is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(data).encode()
    req = Request(base + path, data=data, headers=headers, method=method)
    try:
        response = urlopen(req, timeout=5)
    except HTTPError as error:
        response = error
    with response:
        assert response.headers.get_content_type() == "application/json"
        return response.status, json.loads(response.read()), response.headers

def login(username):
    password = os.getenv("DEMO_" + username.upper() + "_PW", username + "123")
    status, body, _ = request("/api/v1/auth/login", method="POST",
                              data={"username": username, "password": password})
    assert status == 200, "登录失败"
    assert isinstance(body["token"], str) and body["token"]
    return body["token"]

def check_question(question):
    assert set(question) == {"id", "content", "options", "answer", "analysis",
                             "knowledge_points", "type", "status"}
    assert isinstance(question["id"], int) and question["id"] > 0
    for key in ("content", "answer", "analysis"):
        assert isinstance(question[key], str) and question[key]
    assert isinstance(question["knowledge_points"], list)
    assert question["knowledge_points"]
    assert all(isinstance(point, str) and point for point in question["knowledge_points"])
    assert isinstance(question["options"], list)
    assert question["type"] in ("single_choice", "short_answer")
    if question["type"] == "single_choice":
        assert len(question["options"]) == 4
        assert all(set(option) == {"label", "text"} for option in question["options"])
        assert all(isinstance(option["text"], str) and option["text"]
                   for option in question["options"])
        assert question["answer"] in {option["label"] for option in question["options"]}
    else:
        assert question["options"] == []

def list_questions(token):
    status, body, _ = request("/api/v1/questions", token)
    assert status == 200
    assert set(body) == {"questions"}
    return body["questions"]

mode = sys.argv[1]
if mode == "list":
    admin_questions = list_questions(login("admin"))
    student_token = login("student")
    assert list_questions(student_token) == admin_questions
    assert len(admin_questions) == 5
    ids = [question["id"] for question in admin_questions]
    assert ids == sorted(set(ids))
    for question in admin_questions:
        check_question(question)
    status, body, _ = request("/api/v1/questions?page=1&search=ignored", student_token)
    assert status == 200 and body["questions"] == admin_questions
    print("PASS: admin/student 全量列表、字段结构、中文、题型与无分页行为")
elif mode == "detail":
    token = login("student")
    questions = list_questions(token)
    for question in questions:
        status, body, _ = request("/api/v1/questions/" + str(question["id"]), token)
        assert status == 200 and body == {"question": question}
    missing = str(max(question["id"] for question in questions) + 10000)
    for invalid in (missing, "0", "-1", "abc", "1.5", "%2B1", "1/extra", "",
                    "9223372036854775808", "1%20OR%201=1"):
        status, body, _ = request("/api/v1/questions/" + invalid, token)
        assert status == 404, (invalid, status)
        assert set(body) == {"error"} and isinstance(body["error"], str)
    print("PASS: 全部详情与列表一致；不存在、非法、溢出 ID 返回 JSON 404")
elif mode == "access":
    for path in ("/api/v1/questions", "/api/v1/questions/1"):
        for token in (None, "invalid-token"):
            status, body, _ = request(path, token)
            assert status == 401 and set(body) == {"error"}
    token = login("admin")
    for method in ("POST", "PUT", "DELETE"):
        status, body, headers = request("/api/v1/questions", token, method=method)
        assert status == 405 and set(body) == {"error"} and headers["Allow"] == "GET"
    status, _, _ = request("/api/v1/questionsXYZ", token)
    assert status == 404
    status, _, _ = request("/api/v1/auth/logout", token, method="POST")
    assert status == 200
    status, body, _ = request("/api/v1/questions", token)
    assert status == 401 and set(body) == {"error"}
    print("PASS: 缺失/无效/登出 token 返回 401；只读接口返回 405 + Allow")
elif mode == "storage_error":
    token = login("student")
    database = Path(os.environ["DATABASE_PATH"])
    backup = database.with_name(database.name + ".questions-backup")
    assert database.is_file() and not backup.exists()
    database.rename(backup)
    try:
        for path in ("/api/v1/questions", "/api/v1/questions/1"):
            status, body, _ = request(path, token)
            assert status == 500 and set(body) == {"error"}
        assert not database.exists(), "读接口不应偷偷创建空数据库"
    finally:
        backup.rename(database)
    assert len(list_questions(token)) == 5
    print("PASS: 数据库不可读返回 JSON 500；恢复后接口正常")
else:
    raise ValueError(mode)
