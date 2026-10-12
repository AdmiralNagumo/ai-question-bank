/* ── 主应用：路由 + 页面切换 ── */
const $ = (id) => document.getElementById(id);

/* 简易 hash 路由 */
function navigate(hash) {
  window.location.hash = hash;
  route();
}

function route() {
  const hash = window.location.hash || '#login';

  // 检查是否已登录
  const token = API.getToken();

  if (hash === '#login' || hash === '' || hash === '#') {
    AuthPage.show();
    return;
  }

  // 以下页面需要登录
  if (!token) {
    navigate('#login');
    return;
  }

  // 更新导航栏用户信息
  const user = API.getUser();
  $('userInfo').textContent = user ? (user.name || user.username) : '';

  if (hash === '#questions') {
    QuestionsPage.show();
  } else if (hash.startsWith('#chat/')) {
    const id = hash.split('/')[1];
    ChatPage.show(id);
  } else {
    navigate('#questions');
  }
}

/* ── 全局函数（HTML onclick 调用） ── */
window.app = {
  login(e) {
    return AuthPage.handleLogin(e);
  },

  logout() {
    // 尝试调用后端退出 API，然后清除本地状态
    API.logout().catch(() => {});
    API.clearAuth();
    navigate('#login');
  },

  sendChat() {
    ChatPage.sendMessage();
  },

  navigate
};

/* ── 支持回车发送 ── */
document.addEventListener('DOMContentLoaded', () => {
  $('chatInput').addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      ChatPage.sendMessage();
    }
  });

  // 初始路由
  route();
});

/* hash 变化时重新路由 */
window.addEventListener('hashchange', route);