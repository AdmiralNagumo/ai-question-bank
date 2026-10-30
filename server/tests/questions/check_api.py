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
    for method in ("PATCH", "OPTIONS"):
        status, body, headers = request("/api/v1/questions", token, method=method)
        assert status == 405 and set(body) == {"error"}
        assert headers["Allow"] == "GET, POST, PUT, DELETE"
    status, _, _ = request("/api/v1/questionsXYZ", token)
    assert status == 404
    status, _, _ = request("/api/v1/auth/logout", token, method="POST")
    assert status == 200
    status, body, _ = request("/api/v1/questions", token)
    assert status == 401 and set(body) == {"error"}
    print("PASS: 缺失/无效/登出 token 返回 401；不支持的方法返回 405 + Allow")
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
elif mode == "create":
    token = login("admin")
    created = []

    def make_question(payload):
        status, body, _ = request("/api/v1/questions", token, method="POST", data=payload)
        assert status == 201, (status, payload)
        question = body["question"]
        check_question(question)
        created.append(question["id"])
        return question

    before = list_questions(token)
    single = make_question({
        "content": "新增单选题：2 + 2 = ?",
        "type": "single_choice",
        "options": [{"label": "A", "text": "3"}, {"label": "B", "text": "4"},
                    {"label": "C", "text": "5"}, {"label": "D", "text": "6"}],
        "answer": "B",
        "analysis": "2 + 2 = 4。",
        "knowledge_points": ["加法"],
    })
    assert single["content"] == "新增单选题：2 + 2 = ?"
    assert single["type"] == "single_choice" and single["answer"] == "B"
    assert single["id"] not in {q["id"] for q in before}
    assert len(list_questions(token)) == len(before) + 1

    short = make_question({
        "content": "新增简答题：简述牛顿第一定律。",
        "type": "short_answer",
        "options": [],
        "answer": "惯性定律",
        "analysis": "物体在不受外力时保持静止或匀速直线运动。",
        "knowledge_points": ["力学"],
    })
    assert short["type"] == "short_answer"
    assert short["options"] == [] and short["answer"] == "惯性定律"

    # 非法负载 → 400
    bad_payloads = [
        {"type": "single_choice", "options": [{"label": "A", "text": "1"}], "answer": "A"},      # 缺 content
        {"content": "x", "type": "essay", "answer": "y"},                                         # type 非法
        {"content": "x", "type": "single_choice", "answer": "A"},                                 # 缺 options
        {"content": "x", "type": "single_choice", "options": [], "answer": "A"},                  # 空选项
        {"content": "x", "type": "single_choice",
         "options": [{"label": "A", "text": "1"}, {"label": "B", "text": "2"}], "answer": "C"},   # answer 不在选项
        {"content": "x", "type": "short_answer",
         "options": [{"label": "A", "text": "1"}], "answer": "y"},                                # 简答带选项
        {"content": "x", "type": "single_choice",
         "options": [{"label": "", "text": "1"}, {"label": "B", "text": "2"}], "answer": "B"},    # 空 label
        {"content": "x", "type": "single_choice", "options": "not-array", "answer": "A"},         # options 非数组
    ]
    for payload in bad_payloads:
        status, body, _ = request("/api/v1/questions", token, method="POST", data=payload)
        assert status == 400, (status, payload)
        assert set(body) == {"error"}
    status, body, _ = request("/api/v1/questions", token, method="POST", data={})
    assert status == 400

    # 清理创建的题目，保持共享测试库干净
    for qid in created:
        status, _, _ = request("/api/v1/questions/" + str(qid), token, method="DELETE")
        assert status == 200
    assert len(list_questions(token)) == len(before)
    print("PASS: 创建 single_choice/short_answer 成功；非法负载 400；用例自清理")
elif mode == "update":
    token = login("admin")
    status, body, _ = request("/api/v1/questions", token, method="POST", data={
        "content": "待更新题：1+1=?",
        "type": "single_choice",
        "options": [{"label": "A", "text": "1"}, {"label": "B", "text": "2"},
                    {"label": "C", "text": "3"}, {"label": "D", "text": "4"}],
        "answer": "B",
        "knowledge_points": ["加法"],
    })
    assert status == 201
    qid = body["question"]["id"]
    try:
        # 部分更新：只改 answer 与 analysis，其余字段保留
        status, body, _ = request("/api/v1/questions/" + str(qid), token, method="PUT",
                                  data={"answer": "D", "analysis": "新解析"})
        assert status == 200
        question = body["question"]
        assert question["id"] == qid
        assert question["answer"] == "D"
        assert question["analysis"] == "新解析"
        assert question["content"] == "待更新题：1+1=?"
        assert question["type"] == "single_choice"
        assert len(question["options"]) == 4
        # 整体改题型 → short_answer（options 必须为空）
        status, body, _ = request("/api/v1/questions/" + str(qid), token, method="PUT",
                                  data={"type": "short_answer", "options": [], "answer": "2"})
        assert status == 200 and body["question"]["type"] == "short_answer"
        assert body["question"]["options"] == []
        # 错误路径
        status, _, _ = request("/api/v1/questions/999999", token, method="PUT",
                               data={"content": "x"})
        assert status == 404
        status, _, _ = request("/api/v1/questions/" + str(qid), token, method="PUT", data={})
        assert status == 400
        # 一致性：answer 不在新选项 → 400
        status, _, _ = request("/api/v1/questions/" + str(qid), token, method="PUT",
                               data={"type": "single_choice",
                                     "options": [{"label": "A", "text": "1"},
                                                 {"label": "B", "text": "2"}],
                                     "answer": "Z"})
        assert status == 400
    finally:
        status, _, _ = request("/api/v1/questions/" + str(qid), token, method="DELETE")
        assert status == 200
    print("PASS: 部分更新保留未传字段；跨字段一致性校验；404/400 错误路径")
elif mode == "delete":
    token = login("admin")
    created = []
    try:
        for content in ("待删题1", "待删题2"):
            status, body, _ = request("/api/v1/questions", token, method="POST", data={
                "content": content, "type": "short_answer", "options": [],
                "answer": "答案", "knowledge_points": ["测试"],
            })
            assert status == 201
            created.append(body["question"]["id"])
        qid = created[0]
        before = list_questions(token)
        status, body, _ = request("/api/v1/questions/" + str(qid), token, method="DELETE")
        assert status == 200 and body == {"status": "ok"}
        status, _, _ = request("/api/v1/questions/" + str(qid), token)
        assert status == 404
        after = list_questions(token)
        assert len(after) == len(before) - 1
        assert qid not in {q["id"] for q in after}
        # 重复删除 → 404；非法 id → 404
        status, _, _ = request("/api/v1/questions/" + str(qid), token, method="DELETE")
        assert status == 404
        for invalid in ("0", "-1", "abc"):
            status, _, _ = request("/api/v1/questions/" + invalid, token, method="DELETE")
            assert status == 404
    finally:
        for qid in created:
            request("/api/v1/questions/" + str(qid), token, method="DELETE")
    print("PASS: 删除后不可读、列表减一、重复删除/非法 id 404；用例自清理")
elif mode == "permission":
    student = login("student")
    teacher = login("teacher")
    # student 写 → 403（权限检查先于 id 存在性）
    for method, path in (("POST", "/api/v1/questions"),
                         ("PUT", "/api/v1/questions/1"),
                         ("DELETE", "/api/v1/questions/1")):
        status, body, _ = request(path, student, method=method,
                                  data={"content": "x", "type": "short_answer",
                                        "options": [], "answer": "y"})
        assert status == 403, (method, status)
        assert set(body) == {"error"}
    # teacher 可创建与删除
    status, body, _ = request("/api/v1/questions", teacher, method="POST", data={
        "content": "teacher 创建题", "type": "short_answer", "options": [],
        "answer": "ok", "knowledge_points": ["权限"],
    })
    assert status == 201
    qid = body["question"]["id"]
    status, _, _ = request("/api/v1/questions/" + str(qid), teacher, method="DELETE")
    assert status == 200
    # 未登录 → 401
    status, _, _ = request("/api/v1/questions", method="POST",
                           data={"content": "x", "type": "short_answer",
                                 "options": [], "answer": "y"})
    assert status == 401
    print("PASS: student 写操作 403；teacher 可创建/删除；未登录 401")
else:
    raise ValueError(mode)
