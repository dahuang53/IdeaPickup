"use strict";
(() => {
  const $ = (selector) => document.querySelector(selector);
  const labels = {overview:"概览",config:"配置",devices:"设备",jobs:"任务"};
  const states = {received:"已接收",receiving:"接收中",transcribing:"转写中",queued:"等待处理",working:"处理中",transcribed:"待投递",sending:"投递中",sent:"已同步",uncertain:"投递结果待确认",delivery_failed:"投递失败",transcription_failed:"转写失败",transcription_rejected:"转写未通过",audio_expired:"音频暂存已过期",reupload_required:"需要设备重传",created:"配对中",activated:"配对中",finalized:"已配对",revoked:"已撤销",expired:"已过期"};
  const errors = {login_denied:"口令不正确，请重试。",login_required:"登录已失效，请重新登录。",authentication_rate_limited:"尝试过于频繁，请 5 分钟后再试。",initialization_denied:"初始化码无效或已过期，请在部署机重新生成。",password_length_12_to_128:"口令需要 12—128 个字符。",revision_conflict:"配置已被另一个页面修改，请刷新后重试。",snapshot_limit:"旧任务仍在使用配置版本，暂时不能新增版本。",configuration_broker_unavailable:"配置服务暂时不可用，请检查部署状态。",maintenance:"服务正在维护，请稍后重试。",not_configured:"请先填写并保存千问和思源配置。",origin_denied:"页面地址与服务地址不匹配，请从正确的 HTTPS 地址打开。",csrf_denied:"页面会话已更新，请刷新重试。",invalid_url:"地址格式或接口不受支持，请核对完整 HTTPS 地址。",invalid_config:"配置内容不符合要求，请检查字段。",enrollment_pending:"设备有尚未完成的配对，请先在设备上恢复配对。",unsafe_target:"服务地址不在支持范围内。"};
  let csrf="", initialized=false, page="overview", config=null, noticeTimer, pairTimer;
  const offsets={devices:0,jobs:0}, counts={devices:0,jobs:0}, pageSize=20;
  function tell(message, bad=false) {
    const box=$("#notice");box.textContent=message;box.classList.toggle("error",bad);box.hidden=false;
    // Resolve the hidden state before applying the approved entry transition.
    void box.offsetWidth;box.classList.add("show");clearTimeout(noticeTimer);
    noticeTimer=setTimeout(()=>{
      box.classList.remove("show");
      const duration=Math.max(...getComputedStyle(box).transitionDuration.split(",").map(value=>parseFloat(value)*(value.trim().endsWith("ms")?1:1000)));
      noticeTimer=setTimeout(()=>{box.hidden=true;},duration);
    },bad?12000:6000);
  }
  async function api(path, options={}) {
    const headers={Accept:"application/json",...options.headers};
    if(options.body!==undefined){headers["Content-Type"]="application/json";headers["X-CSRF-Token"]=csrf;}
    let response;try{response=await fetch(new URL("api/"+path,document.baseURI),{...options,headers,credentials:"same-origin",cache:"no-store",body:options.body===undefined?undefined:JSON.stringify(options.body)});}catch{throw new Error("无法连接服务，请稍后重试。");}
    let data;try{data=await response.json();}catch{throw new Error("服务返回了无法读取的响应。");}
    if(!response.ok){if(response.status===401&&path!=="session/login"){csrf="";showGate();}const error=new Error(errors[data.error]||"操作未完成，请检查服务状态后重试。");error.code=data.error;throw error;}
    return data;
  }
  async function external(path){const url=new URL("../"+path,document.baseURI);const response=await fetch(url,{credentials:"same-origin",cache:"no-store"});let data;try{data=await response.json();}catch{throw new Error("无法读取服务状态。");}if(!response.ok&&response.status!==503)throw new Error("无法读取服务状态。");return data;}
  async function action(button, task){const old=button.disabled;button.disabled=true;try{await task();}catch(error){tell(error.message,true);}finally{button.disabled=old;if(button.id.endsWith("-next")){const kind=button.id.split("-")[0];button.disabled=counts[kind]<pageSize;}if(button.id.endsWith("-prev")){const kind=button.id.split("-")[0];button.disabled=offsets[kind]===0;}requestAnimationFrame(()=>{const focused=document.activeElement,closedDialog=focused.closest("#confirm-dialog");if(!button.disabled&&(focused===document.body||(closedDialog&&!closedDialog.open))&&button.getClientRects().length)button.focus({preventScroll:true});});}}
  function showGate(){ $("#gate").hidden=false;$("#workspace").hidden=true;$("#auth-form").hidden=false;$("#gate-retry").hidden=true;$("#gate-title").textContent=initialized?"欢迎回来。":"设置你的管理口令。";$("#gate-help").textContent=initialized?"登录后管理服务配置和设备。":"在部署机运行初始化命令取得一次性码，完成首次设置。";$("#bootstrap-label").hidden=initialized;$("#bootstrap-code").required=!initialized;$("#password").autocomplete=initialized?"current-password":"new-password";$("#auth-submit").textContent=initialized?"登录管理页":"完成初始化";}
  async function session(){try{const value=await api("session");initialized=value.initialized;if(!value.authenticated){showGate();return;}csrf=value.csrf;$("#gate").hidden=true;$("#workspace").hidden=false;try{await showPage(page);}catch(error){tell(error.message,true);}}catch(error){$("#gate-title").textContent="暂时无法连接服务。";$("#gate-help").textContent=error.message;$("#gate-retry").hidden=false;}}
  async function showPage(name){page=name;for(const key of Object.keys(labels)){$("#page-"+key).hidden=key!==page;const button=$('[data-page="'+key+'"]');if(key===page)button.setAttribute("aria-current","page");else button.removeAttribute("aria-current");}$("#breadcrumb").textContent=labels[page];await refresh();}
  async function refresh(){if(page==="overview")await overview();if(page==="config")await loadConfig();if(page==="devices")await devices();if(page==="jobs")await jobs();}
  function short(value){return value?value.slice(0,8):"—";}
  function date(value){return value?new Date(value).toLocaleString("zh-CN",{hour12:false}):"尚无活动";}
  function status(value){return states[value]||value||"未知";}
  function bytes(value){return Number.isFinite(value)?(value/1024/1024).toFixed(0)+" MiB":"尚未确认";}
  async function overview(){
    const [state,view]=await Promise.all([api("status"),api("config")]);
    $("#service-badge").textContent=state.ready?"可以接收录音":"需要处理";
    $("#service-badge").className="badge "+(state.ready?"good":"attention");
    $("#ready-title").textContent=state.ready?"服务已就绪":"尚未就绪";
    $("#ready-detail").textContent=state.ready?"新录音可以按当前配置转写与投递。":errors[state.reason]||"检查配置与部署状态后，再连接设备。";
    $("#instance").textContent=state.instance_uuid;
    $("#config-summary").textContent=view.active_revision?"已启用 · "+short(view.active_revision):"未启用";
    $("#cleanup-summary").textContent=state.cleanup_ok===true?"运行正常":state.cleanup_ok===false?"需要检查":"尚未确认";
    $("#version-summary").textContent=state.code_version;
    $("#worker-summary").textContent=state.worker_alive===true?"运行中":state.maintenance?"维护中，暂停处理":"尚未启动";
    $("#decoder-summary").textContent=state.opus_available===true?"可用":state.opus_available===false?"不可用":"尚未确认";
    $("#storage-summary").textContent=bytes(state.free_work_bytes);
  }
  function renderConfig(view){
    config=view;const form=$("#config-form");form.elements.public_base_url.value=view.public_url||"";
    for(const field of ["qwen_url","qwen_model","siyuan_url"])form.elements[field].value=view.config[field]||"";
    for(const field of ["qwen_api_key","siyuan_token"])form.elements[field].value="";
    form.elements.clear_qwen.checked=false;form.elements.clear_siyuan.checked=false;
    $("#qwen-state").classList.toggle("unconfigured",!view.config.qwen_api_key_configured);
    $("#qwen-state").textContent=view.config.qwen_api_key_configured?"已配置 · 留空保留":"未配置";
    $("#siyuan-state").classList.toggle("unconfigured",!view.config.siyuan_token_configured);
    $("#siyuan-state").textContent=view.config.siyuan_token_configured?"已配置 · 留空保留":"未配置";
    $("#revision-badge").textContent=view.active_revision?"已生效 · "+short(view.active_revision):"尚未配置";
  }
  async function loadConfig(){renderConfig(await api("config"));}
  function patch(){const form=$("#config-form"),result={};for(const [field,clear] of [["qwen_api_key","clear_qwen"],["siyuan_token","clear_siyuan"]]){if(form.elements[clear].checked&&form.elements[field].value)throw new Error("替换和清除同一凭据不能同时选择。");if(form.elements[clear].checked)result[field]={op:"clear"};else if(form.elements[field].value)result[field]={op:"replace",value:form.elements[field].value};}return result;}
  function cell(row,text,secondary){const td=document.createElement("td");td.textContent=text;if(secondary){const small=document.createElement("span");small.className="secondary-line";small.textContent=secondary;td.append(small);}row.append(td);return td;}
  function operation(td,label,callback){const button=document.createElement("button");button.textContent=label;button.type="button";button.addEventListener("click",()=>action(button,callback));td.append(button);}
  async function confirm(title,copy){const dialog=$("#confirm-dialog");$("#confirm-title").textContent=title;$("#confirm-copy").textContent=copy;dialog.returnValue="cancel";dialog.showModal();return new Promise(resolve=>dialog.addEventListener("close",()=>resolve(dialog.returnValue==="confirm"),{once:true}));}
  function pagination(kind,count){counts[kind]=count;$("#"+kind+"-prev").disabled=offsets[kind]===0;$("#"+kind+"-next").disabled=count<pageSize;$("#"+kind+"-page").textContent="第 "+(offsets[kind]/pageSize+1)+" 页";}
  async function devices(){
    const values=await api("devices?limit="+pageSize+"&offset="+offsets.devices),target=$("#devices-body");
    target.replaceChildren();$("#devices-empty").hidden=values.length!==0;
    for(const device of values){
      const row=document.createElement("tr");cell(row,device.device_id);
      cell(row,device.revoked_at?"已撤销":status(device.enrollment_phase||"finalized"));
      cell(row,date(device.last_seen_at));const operations=cell(row,"");
      if(!device.revoked_at){
        operation(operations,"撤销",async()=>{if(await confirm("撤销这台设备？","撤销后设备无法继续上传；已有录音与处理记录会保留。")){await api("devices/"+encodeURIComponent(device.device_id)+"/revoke",{method:"POST",body:{}});tell("设备授权已撤销。");await devices();}});
      }
      target.append(row);
    }
    pagination("devices",values.length);
  }
  function duration(ms){return Math.floor((ms||0)/60000)+":"+String(Math.floor((ms||0)/1000)%60).padStart(2,"0");}
  async function retryJob(job,path){
    const view=await api("config");
    if(!view.active_revision)throw new Error("请先保存可用的服务配置。");
    if(!await confirm("使用当前配置重试？","本次重试使用当前保存的千问和思源配置，可能产生调用费用。"))return;
    await api(path+"/retry",{method:"POST",body:{expected_revision:view.active_revision}});
    tell("重试已受理。");await jobs();
  }
  async function jobs(){
    const result=await api("jobs?limit="+pageSize+"&offset="+offsets.jobs),values=Array.isArray(result)?result:result.items;
    const target=$("#jobs-body");target.replaceChildren();$("#jobs-empty").hidden=values.length!==0;
    for(const job of values){
      const row=document.createElement("tr");cell(row,"#"+job.recording_id,job.device_id);
      cell(row,duration(job.duration_ms),job.source==="stream"?"无卡暂存":job.source==="wav"?"存储卡录音":"历史记录");
      cell(row,status(job.status),job.reason||job.error_code);
      const operations=cell(row,""),path="jobs/"+encodeURIComponent(job.device_id)+"/"+job.recording_id;
      if(job.can_retry)operation(operations,"重试",()=>retryJob(job,path));
      if(job.status==="uncertain"){
        operation(operations,"思源中已存在",async()=>{if(await confirm("确认思源中已经收到？","请在当次投递使用的思源收集箱核对这条记录。确认后将标记完成，不再次投递。")){await api(path+"/reconcile",{method:"POST",body:{present:true}});await jobs();}});
        operation(operations,"思源中不存在",async()=>{if(await confirm("确认思源中没有这条记录？","请确认这条记录尚未收到；错误确认可能造成重复投递。")){await api(path+"/reconcile",{method:"POST",body:{present:false}});await jobs();}});
      }
      target.append(row);
    }
    pagination("jobs",values.length);
  }
  async function probe(service,endToEnd=false){
    if(endToEnd&&!await confirm("执行一次实际调用测试？","将使用固定合成静音调用千问，并在当前思源收集箱创建一条测试记录，可能产生服务费用。两项结果会分别显示。"))return;
    const result=await api("config/probe",{method:"POST",body:{service,end_to_end:endToEnd}});
    const report=window.voiceMemoProbeReport(result);
    $("#probe-result").textContent=report.text;
    $("#probe-result").classList.toggle("probe-failed",!report.success);
  }
  $("#auth-form").addEventListener("submit",event=>{event.preventDefault();action($("#auth-submit"),async()=>{if(!initialized){await api("session/initialize",{method:"POST",body:{code:$("#bootstrap-code").value,password:$("#password").value}});initialized=true;$("#bootstrap-code").value="";}const value=await api("session/login",{method:"POST",body:{password:$("#password").value}});csrf=value.csrf;$("#password").value="";await session();});});
  $("#gate-retry").addEventListener("click",session);
  document.querySelectorAll("[data-page],[data-go]").forEach(button=>button.addEventListener("click",()=>action(button,()=>showPage(button.dataset.page||button.dataset.go))));
  $("#refresh").addEventListener("click",()=>action($("#refresh"),refresh));
  $("#logout").addEventListener("click",()=>action($("#logout"),async()=>{await api("session/logout",{method:"POST",body:{}});csrf="";config=null;clearInterval(pairTimer);$("#pairing-code").textContent="";$("#pairing-panel").hidden=true;$("#config-form").reset();showGate();}));
  $("#config-form").addEventListener("submit",event=>{event.preventDefault();action(event.submitter,async()=>{
    if(!config)throw new Error("请先读取当前配置。");const draft=patch();
    try{const view=await api("config",{method:"POST",body:{expected_revision:config.active_revision,patch:draft}});renderConfig(view);tell("配置已保存并生效。");}
    catch(error){
      try{renderConfig(await api("config"));const form=$("#config-form");for(const [field,value] of Object.entries(draft)){if(value.op==="replace")form.elements[field].value=value.value;else form.elements[field==="qwen_api_key"?"clear_qwen":"clear_siyuan"].checked=true;}}
      catch{ /* Preserve the original failure if reading the service also fails. */ }
      throw error;
    }
  });});
  $("#validate-config").addEventListener("click",()=>action($("#validate-config"),async()=>{await api("config/validate",{method:"POST",body:{patch:patch()}});tell("填写内容符合要求；尚未保存，也未验证供应商鉴权。");}));
  $("#probe-qwen").addEventListener("click",()=>action($("#probe-qwen"),()=>probe("qwen")));$("#probe-siyuan").addEventListener("click",()=>action($("#probe-siyuan"),()=>probe("siyuan")));$("#test-delivery").addEventListener("click",()=>action($("#test-delivery"),()=>probe("all",true)));
  $("#issue-code").addEventListener("click",()=>action($("#issue-code"),async()=>{const result=await api("devices/pairing-code",{method:"POST",body:{ttl_seconds:900}});clearInterval(pairTimer);$("#pairing-panel").hidden=false;$("#pairing-code").textContent=result.pairing_code;const update=()=>{const seconds=Math.max(0,Math.ceil((result.expires_at-Date.now())/1000));$("#pairing-expiry").textContent=seconds?Math.floor(seconds/60)+" 分 "+seconds%60+" 秒后过期，仅可使用一次。":"已过期，请重新生成。";if(!seconds){clearInterval(pairTimer);$("#pairing-code").textContent="已过期";$("#copy-code").disabled=true;}};$("#copy-code").disabled=false;update();pairTimer=setInterval(update,1000);}));
  $("#copy-code").addEventListener("click",()=>action($("#copy-code"),async()=>{await navigator.clipboard.writeText($("#pairing-code").textContent);tell("配对码已复制。");}));
  for(const kind of ["devices","jobs"]){$("#"+kind+"-prev").addEventListener("click",()=>action($("#"+kind+"-prev"),async()=>{offsets[kind]=Math.max(0,offsets[kind]-pageSize);await refresh();}));$("#"+kind+"-next").addEventListener("click",()=>action($("#"+kind+"-next"),async()=>{offsets[kind]+=pageSize;await refresh();}));}
  window.addEventListener("pagehide",()=>{csrf="";clearInterval(pairTimer);for(const input of document.querySelectorAll('input[type="password"]'))input.value="";$("#pairing-code").textContent="";});
  session();
})();
