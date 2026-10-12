/* ── API 客户端 ──
 *
 * 两种运行模式，自动切换，调用方无需关心：
 *
 *   1. 在线模式：页面由 C 后端托管（http://host:8000），正常请求 /api/v1/*
 *   2. 离线演示模式：直接双击 index.html（file:// 协议）或后端未启动时，
 *      浏览器会禁止 fetch 并抛 TypeError: Failed to fetch。此时自动回退到
 *      内置演示数据，保证登录 → 题目列表 → 对话页整条链路可完整演示。
 *
 * 离线数据严格对齐 docs/auth.md（三个 demo 账号）与 docs/questions.md（5 道固定题）。
 */
const API = (() => {
  const BASE = '/api/v1';

  /* file:// 协议下 fetch 必然失败，直接判定为离线，省去一次无谓请求 */
  const OFFLINE_PROTOCOL =
    (typeof window !== 'undefined') && window.location.protocol === 'file:';

  /* ── 存储：file:// 下 localStorage 可能受限，降级到内存 ── */
  const memoryStore = {};

  function storeGet(key) {
    try {
      return localStorage.getItem(key);
    } catch (e) {
      return Object.prototype.hasOwnProperty.call(memoryStore, key) ? memoryStore[key] : null;
    }
  }

  function storeSet(key, value) {
    try {
      localStorage.setItem(key, value);
    } catch (e) {
      memoryStore[key] = value;
    }
  }

  function storeDel(key) {
    try {
      localStorage.removeItem(key);
    } catch (e) {
      delete memoryStore[key];
    }
  }

  /** 获取存储的 token */
  function getToken() {
    return storeGet('token');
  }

  /** 获取当前用户信息 */
  function getUser() {
    const raw = storeGet('user');
    if (!raw) return null;
    try {
      return JSON.parse(raw);
    } catch (e) {
      return null;
    }
  }

  /** 保存登录状态 */
  function setAuth(token, user) {
    storeSet('token', token);
    storeSet('user', JSON.stringify(user));
  }

  /** 清除登录状态 */
  function clearAuth() {
    storeDel('token');
    storeDel('user');
  }

  /** 判断异常是否属于「后端不可达」——这类错误应回退到演示数据 */
  function isUnreachable(err) {
    if (!err) return false;
    if (err.networkError) return true;
    const msg = String(err.message || '');
    return msg.indexOf('Failed to fetch') !== -1 ||
           msg.indexOf('NetworkError') !== -1 ||
           msg.indexOf('Load failed') !== -1;   // Safari 的措辞
  }

  /** 判断异常是否属于「接口尚未实现」——例如 chat 接口还没上线时的 404 */
  function isNotImplemented(err) {
    if (!err) return false;
    /* 优先按 HTTP 状态码判断，比匹配错误文案可靠：
     *   404 接口不存在 / 405 方法不允许 / 501 服务端未实现该方法
     * 注意不含 500：那是后端真实故障，必须暴露出来，不能被演示数据掩盖。 */
    if (err.status === 404 || err.status === 405 || err.status === 501) {
      return true;
    }
    const msg = String(err.message || '');
    return msg.indexOf('api not found') !== -1 ||
           msg.indexOf('404') !== -1 ||
           msg.indexOf('405') !== -1 ||
           msg.indexOf('501') !== -1 ||
           msg.indexOf('Not Found') !== -1 ||
           msg.indexOf('method not allowed') !== -1 ||
           msg.indexOf('Unsupported method') !== -1;
  }

  /** 通用 fetch 封装 */
  async function request(method, path, body) {
    const url = BASE + path;
    const headers = { 'Content-Type': 'application/json' };
    const token = getToken();
    if (token) {
      headers['Authorization'] = 'Bearer ' + token;
    }
    const opts = { method, headers };
    if (body !== undefined) {
      opts.body = JSON.stringify(body);
    }

    let res;
    try {
      res = await fetch(url, opts);
    } catch (netErr) {
      /* 网络层失败：file:// 协议、后端没启动、连接被拒。
       * 打上标记，让上层决定是回退演示数据还是直接报错。 */
      const wrapped = new Error(netErr && netErr.message ? netErr.message : '网络请求失败');
      wrapped.networkError = true;
      throw wrapped;
    }

    /* 响应体可能不是 JSON（例如反向代理返回 HTML 错误页），单独兜住 */
    let data;
    try {
      data = await res.json();
    } catch (e) {
      data = {};
    }

    if (!res.ok) {
      /* 把状态码挂到异常上，供 isNotImplemented 精确判断 */
      const err = new Error(data.error || ('请求失败 (' + res.status + ')'));
      err.status = res.status;
      throw err;
    }
    return data;
  }

  /** 对话接口是否真的回退到了演示数据（http:// 下也会发生，因为后端
   *  /api/v1/chat 尚未实现会返回 404）。页面据此提示，避免把内置回复
   *  当成真实 AI 输出展示。 */
  let chatMocked = false;

  /** 统一回退包装：真实请求失败且属于可回退错误时，改用演示数据 */
  async function withFallback(realCall, mockCall, onFallback) {
    if (OFFLINE_PROTOCOL) {
      if (onFallback) onFallback();
      return mockCall();
    }
    try {
      return await realCall();
    } catch (e) {
      if (isUnreachable(e) || isNotImplemented(e)) {
        if (onFallback) onFallback();
        return mockCall();
      }
      throw e;
    }
  }

  /* ══════════════════════════════════════════════════════════
   *  离线演示数据
   *  账号来源：docs/auth.md 第 4 节
   *  题目来源：docs/questions.md 第 1 节（5 道固定题）
   * ══════════════════════════════════════════════════════════ */

  const MOCK_USERS = [
    { username: 'admin',   password: 'admin123',   id: 1, name: '系统管理员', role: 'admin' },
    { username: 'teacher', password: 'teacher123', id: 2, name: '张老师',     role: 'teacher' },
    { username: 'student', password: 'student123', id: 3, name: '李同学',     role: 'student' }
  ];

  const MOCK_QUESTIONS = [
    {
      id: 1,
      content: '解方程：2x + 3 = 11，x 的值是多少？',
      options: [
        { label: 'A', text: '2' },
        { label: 'B', text: '3' },
        { label: 'C', text: '4' },
        { label: 'D', text: '5' }
      ],
      answer: 'C',
      analysis: '两边减去 3 得到 2x = 8，再除以 2，得到 x = 4。',
      knowledge_points: ['一元一次方程', '等式性质'],
      type: 'single_choice'
    },
    {
      id: 2,
      content: '求函数 f(x) = x² 的导数 f\'(x)。',
      options: [
        { label: 'A', text: '2x' },
        { label: 'B', text: 'x' },
        { label: 'C', text: 'x²' },
        { label: 'D', text: '2x²' }
      ],
      answer: 'A',
      analysis: '由幂函数求导公式 (xⁿ)\' = n·xⁿ⁻¹，得 f\'(x) = 2x。',
      knowledge_points: ['导数', '幂函数求导'],
      type: 'single_choice'
    },
    {
      id: 3,
      content: '求极限 lim(x→0) sin(x)/x 的值。',
      options: [
        { label: 'A', text: '0' },
        { label: 'B', text: '1' },
        { label: 'C', text: '∞' },
        { label: 'D', text: '不存在' }
      ],
      answer: 'B',
      analysis: '这是第一个重要极限，弧度制下 x→0 时 sin(x)/x → 1。',
      knowledge_points: ['极限', '重要极限'],
      type: 'single_choice'
    },
    {
      id: 4,
      content: '质量为 2 kg 的物体产生 3 m/s² 的加速度，所受合力是多少？',
      options: [
        { label: 'A', text: '5 N' },
        { label: 'B', text: '1.5 N' },
        { label: 'C', text: '6 N' },
        { label: 'D', text: '9 N' }
      ],
      answer: 'C',
      analysis: '由牛顿第二定律 F = ma = 2 × 3 = 6 N。',
      knowledge_points: ['牛顿第二定律', '力学'],
      type: 'single_choice'
    },
    {
      id: 5,
      content: 'C 语言中 sizeof(char) 的值是多少？',
      options: [
        { label: 'A', text: '1' },
        { label: 'B', text: '2' },
        { label: 'C', text: '4' },
        { label: 'D', text: '8' }
      ],
      answer: 'A',
      analysis: 'C 标准规定 sizeof(char) 恒为 1，单位是字节而非位。',
      knowledge_points: ['C语言', 'sizeof'],
      type: 'single_choice'
    }
  ];

  const MOCK_REPLIES = {
    1: '根据一元一次方程的解法：\n\n步骤 1：将常数项移到等号右边\n2x = 11 - 3\n2x = 8\n\n步骤 2：两边同时除以系数\nx = 8 ÷ 2\nx = 4\n\n所以 x = 4，选择 C 选项。',
    2: '根据幂函数求导公式 (x^n)\' = n·x^(n-1)：\n\nf(x) = x^2\nf\'(x) = 2·x^(2-1) = 2x\n\n所以答案为 f\'(x) = 2x，选择 A 选项。',
    3: '这是微积分中非常重要的第一个重要极限。\n\n在弧度制下，当 x → 0 时，sin(x)/x → 1。\n\n直观理解：当角度很小时，正弦值约等于弧度值，比值趋近于 1。\n\n所以选择 B 选项。',
    4: '根据牛顿第二定律：\n\n公式：F = ma\n其中 m = 2 kg，a = 3 m/s²\n\nF = 2 × 3 = 6 N\n\n所以所受合力为 6 N，选择 C 选项。',
    5: '在 C 语言中：\n\n· C 标准明确规定 sizeof(char) 的值为 1\n· 这个 "1" 是指 1 个字节（byte）\n· 一个字节有多少位由 CHAR_BIT 宏定义（通常为 8）\n· 但 sizeof 返回的是字节数，不是位数\n\n所以答案为 A 选项。'
  };

  /** 演示数据也模拟网络延迟，让加载态可见，避免界面闪跳 */
  function delay(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }

  function mockLogin(username, password) {
    return delay(400).then(() => {
      const hit = MOCK_USERS.filter((u) => u.username === username)[0];
      /* 对齐 docs/auth.md：用户名不存在与密码错误统一返回 401，防枚举 */
      if (!hit || hit.password !== password) {
        throw new Error('用户名或密码错误');
      }
      return {
        token: 'demo-offline-token-' + Date.now(),
        user: { id: hit.id, username: hit.username, name: hit.name, role: hit.role }
      };
    });
  }

  function mockQuestions() {
    return delay(300).then(() => ({ questions: MOCK_QUESTIONS.slice() }));
  }

  function mockQuestionDetail(id) {
    return delay(300).then(() => {
      const num = parseInt(id, 10);
      const hit = MOCK_QUESTIONS.filter((q) => q.id === num)[0];
      if (!hit) {
        /* 对齐 docs/questions.md 的 404 契约 */
        throw new Error('question not found');
      }
      return { question: hit };
    });
  }

  function mockChat(questionId, message) {
    return delay(800).then(() => {
      const reply = MOCK_REPLIES[questionId] ||
        '这是题目 ' + questionId + ' 的解答（演示模式）。\n\n' +
        '你的问题：' + message + '\n\n' +
        '后端对话接口尚未就绪，当前为前端内置的模拟回复。';
      return { reply: reply };
    });
  }

  function mockMe() {
    return delay(150).then(() => {
      const user = getUser();
      if (!user) throw new Error('未登录或 token 已过期');
      return { user: user };
    });
  }

  /* ── 对外接口（签名与在线模式完全一致） ── */
  return {
    getToken,
    getUser,
    setAuth,
    clearAuth,

    /** 当前是否处于离线演示模式，供页面提示使用 */
    isOfflineDemo() {
      return OFFLINE_PROTOCOL;
    },

    /** 对话回复是否来自内置演示数据（离线或后端 chat 接口未就绪时为 true） */
    isChatMocked() {
      return chatMocked;
    },

    /** 登录 POST /auth/login */
    login(username, password) {
      return withFallback(
        () => request('POST', '/auth/login', { username: username, password: password }),
        () => mockLogin(username, password)
      );
    },

    /** 退出 POST /auth/logout（离线模式直接成功，本地状态由调用方清除） */
    logout() {
      return withFallback(
        () => request('POST', '/auth/logout'),
        () => delay(100).then(() => ({ status: 'ok' }))
      );
    },

    /** 获取当前用户 GET /auth/me */
    me() {
      return withFallback(() => request('GET', '/auth/me'), mockMe);
    },

    /** 获取题目列表 GET /questions */
    questions() {
      return withFallback(() => request('GET', '/questions'), mockQuestions);
    },

    /** 获取题目详情 GET /questions/{id} */
    questionDetail(id) {
      return withFallback(
        () => request('GET', '/questions/' + id),
        () => mockQuestionDetail(id)
      );
    },

    /** 发送对话 POST /chat（后端尚未实现，自动回退演示数据） */
    chat(questionId, message) {
      return withFallback(
        () => request('POST', '/chat', { question_id: questionId, message: message }),
        () => mockChat(questionId, message),
        () => { chatMocked = true; }
      );
    }
  };
})();
