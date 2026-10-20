/* ── 通用工具函数 ── */
/** HTML 转义：来自后端的不可信内容拼入 innerHTML 前必须先转义，
 *  防止 <script> / <img onerror=...> 等被浏览器执行（XSS）。
 *  这也是 CONTRIBUTING.md 审查清单中「注入」一项的要求。 */
function escapeHtml(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

// 供其他页面脚本复用
window.escapeHtml = escapeHtml;
