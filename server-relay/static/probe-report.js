"use strict";
// Shared presentation contract; usable in the browser and dependency-free Node tests.
(function (root) {
  function report(result) {
    if (!result || !["validate", "tls", "end_to_end"].includes(result.mode) ||
        !result.providers || typeof result.providers !== "object") {
      return {text:"无法读取测试结果，请刷新后检查服务状态。", success:false, uncertain:false};
    }
    const lines = [];
    let success = true, uncertain = false;
    const names = {qwen:"千问", siyuan:"思源"};
    for (const [name, label] of Object.entries(names)) {
      const state = result.providers[name];
      if (!state) continue;
      const details = [];
      if (state.validation === "missing_credential") details.push("尚未配置凭据");
      if (state.tls === "pass") details.push("TLS 连接通过");
      else if (state.tls === "fail") details.push("TLS 连接失败");
      else details.push("TLS 未检查");
      if (state.auth === "pass") details.push("鉴权通过");
      else if (state.auth === "fail") details.push("鉴权失败");
      else details.push("鉴权未检查");
      if (state.delivery === "uncertain_do_not_repeat_automatically") {
        details.push("投递结果不确定，请先到思源核对，勿重复执行测试");
        uncertain = true;
      } else if (state.delivery === "pass_test_entry_created") {
        details.push("测试条目已创建，请在思源查看");
      }
      if (state.end_to_end === "pass_synthetic_silence") {
        details.push("合成静音请求已通过；识别效果仍需实际录音验证");
      } else if (state.end_to_end === "fail") {
        details.push("实际调用未通过，请核对凭据与服务状态");
      }
      if (result.mode === "tls") success = success && state.tls === "pass";
      else if (result.mode === "validate") success = success && state.validation === "pass";
      else success = success && ["pass", "pass_synthetic_silence"].includes(state.end_to_end);
      lines.push(label + "：" + details.join("；") + "。");
    }
    if (!lines.length) return {text:"测试结果缺少服务信息，请检查部署状态。",success:false,uncertain:false};
    return {text:lines.join("\n"),success:success && !uncertain,uncertain};
  }
  if (typeof module !== "undefined" && module.exports) module.exports = report;
  else root.voiceMemoProbeReport = report;
})(globalThis);
