"use strict";
// R02 browser scenarios expanded for the R02A DOM/style integration.
// Run only against tests/relay_admin_fixture.py, never a production URL.
const fs = require("node:fs"), path = require("node:path"), assert = require("node:assert/strict");
const {pathToFileURL} = require("node:url");
const modulePath = process.env.PLAYWRIGHT_MODULE;
if (!modulePath) throw new Error("Set PLAYWRIGHT_MODULE to the installed Playwright 1.62.1 module directory.");
assert.equal(require(path.join(modulePath,"package.json")).version,"1.62.1", "browser dependency version mismatch");
const {chromium} = require(modulePath);
const root = path.resolve(__dirname,".."), output = path.resolve(process.argv[2] || "");
assert(output.startsWith(path.join(root,"private")+path.sep),"fixture output must be under project private/");
const fixture = JSON.parse(fs.readFileSync(path.join(output,"fixture.json")));
assert.match(fixture.url,/^https:\/\/127\.0\.0\.1:\d+\/memo\/admin\/$/);
const checks = [], screenshots = [], requests = [], errors = [], layout = [];
const control = {delivery:"ok",probe:"ok"};
function mode(values) { Object.assign(control,values);fs.writeFileSync(path.join(output,"control.next"),JSON.stringify(control),{mode:0o600});fs.renameSync(path.join(output,"control.next"),path.join(output,"control.json")); }
function count(suffix) { return requests.filter(r=>r.method==="POST"&&r.path.endsWith(suffix)).length; }
async function painted(page) { await page.evaluate(()=>document.fonts.ready);await page.waitForTimeout(250); }
async function shot(page,name) { await painted(page);await page.screenshot({path:path.join(output,name+".png")});screenshots.push(name+".png"); }
async function call(page,relative,body) {
  return page.evaluate(async ({relative,body})=>{
    const session=await (await fetch(new URL("api/session",document.baseURI))).json();
    const response=await fetch(new URL(relative,document.baseURI),body===undefined?{}:{method:"POST",headers:{"Content-Type":"application/json","X-CSRF-Token":session.csrf},body:JSON.stringify(body)});
    return {status:response.status,value:await response.json()};
  },{relative,body});
}
async function navigate(page,name) { await page.locator(`[data-page="${name}"]`).click();await page.waitForFunction(name=>!document.querySelector(`[data-page="${name}"]`).disabled,name); }
async function notice(page,text) { await page.waitForFunction(text=>document.querySelector("#notice").textContent.includes(text)&&!document.querySelector("#notice").hidden,text); }
async function dialog(page,choice) { await page.locator("#confirm-dialog").waitFor({state:"visible"});await page.locator(`#confirm-dialog button[value="${choice}"]`).click();await page.locator("#confirm-dialog").waitFor({state:"hidden"}); }
async function login(page) { await page.locator("#password").fill(fixture.password);await page.locator("#auth-submit").click();await page.locator("#workspace").waitFor({state:"visible"});await page.waitForFunction(()=>!document.querySelector("#auth-submit").disabled); }
async function save(page) { await page.locator("#save-config").click();await notice(page,"配置已保存并生效");await page.waitForFunction(()=>!document.querySelector("#save-config").disabled); }
async function pair(page,device,code) {
  return page.evaluate(async ({device,code})=>{
    const post=async (relative,body,headers={})=>{const r=await fetch(new URL(relative,document.baseURI),{method:"POST",headers:{"Content-Type":"application/json",...headers},body:JSON.stringify(body)});if(!r.ok)throw new Error("fixture enrollment HTTP "+r.status);return r.json();};
    const session=await (await fetch(new URL("api/session",document.baseURI))).json();
    if(!code)code=(await post("api/devices/pairing-code",{}, {"X-CSRF-Token":session.csrf})).pairing_code;
    const body={device_id:device,pairing_code:code,enrollment_id:crypto.randomUUID(),candidate_token:Array.from(crypto.getRandomValues(new Uint8Array(32)),b=>b.toString(16).padStart(2,"0")).join("")};
    await post("../v1/enrollments",body);
    for(const action of ["activate","finalize"])await post("../v1/enrollments/"+body.enrollment_id+"/"+action,{}, {Authorization:"Bearer "+body.candidate_token});
    return body;
  },{device,code});
}
async function upload(page,device,recording) {
  const result=await page.evaluate(async ({device,recording})=>{
    const data=new Uint8Array(32044),v=new DataView(data.buffer),text=(offset,value)=>[...value].forEach((c,i)=>data[offset+i]=c.charCodeAt(0));
    text(0,"RIFF");v.setUint32(4,32036,true);text(8,"WAVE");text(12,"fmt ");v.setUint32(16,16,true);v.setUint16(20,1,true);v.setUint16(22,1,true);v.setUint32(24,16000,true);v.setUint32(28,32000,true);v.setUint16(32,2,true);v.setUint16(34,16,true);text(36,"data");v.setUint32(40,32000,true);
    const hash=Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256",data)),b=>b.toString(16).padStart(2,"0")).join("");
    const r=await fetch(new URL("../v1/memos",document.baseURI),{method:"POST",headers:{Authorization:"Bearer "+device.candidate_token,"X-Device-ID":device.device_id,"X-Recording-ID":String(recording),"X-Audio-SHA256":hash,"Content-Type":"audio/wav"},body:data});
    return r.status;
  },{device,recording});
  assert.equal(result,202);
}
async function taskState(page,device,recording,status) {
  await page.waitForFunction(async ({device,recording,status})=>{
    const r=await fetch(new URL("../v1/memos/"+device.device_id+"/"+recording,document.baseURI),{headers:{Authorization:"Bearer "+device.candidate_token}});
    return (await r.json()).status===status;
  },{device,recording,status});
}
function jobRow(page,recording) {
  return page.locator("#jobs-body tr").filter({has:page.locator("td:first-child").filter({hasText:new RegExp("^#"+recording+"\\s*browser-active-model$")})});
}
async function geometry(page,label) {
  layout.push({label,...await page.evaluate(()=>{
    const box=e=>{const r=e.getBoundingClientRect();return {x:r.x,y:r.y,width:r.width,height:r.height};};
    const node=s=>[".rail",".topbar"].includes(s)?document.querySelector(s):document.querySelector("#page-config").querySelector(s);
    const selectors=[".rail",".topbar",".page-heading","#config-title","#config-form",".two-fields",".connection-tests"];
    return {viewport:{width:innerWidth,height:innerHeight},font:getComputedStyle(document.body).fontFamily,zoom:devicePixelRatio,
      nodes:Object.fromEntries(selectors.map(s=>[s,box(node(s))])),colors:Object.fromEntries(["--bg","--text-main","--accent","--border"].map(s=>[s,getComputedStyle(document.body).getPropertyValue(s)]))};
  })});
}

(async()=>{
  const browser=await chromium.launch({executablePath:process.env.CHROME_EXECUTABLE,headless:true});
  const context=await browser.newContext({ignoreHTTPSErrors:true,viewport:{width:1280,height:720},permissions:["clipboard-read","clipboard-write"]});
  const page=await context.newPage();
  page.on("pageerror",e=>errors.push(e.message));
  page.on("request",r=>{const url=new URL(r.url());if(url.origin===new URL(fixture.url).origin)requests.push({method:r.method(),path:url.pathname});else throw new Error("Unexpected external browser request: "+url.origin);});
  try {
    // Actual application requests except explicit one-shot response/network faults.
    await page.route("**/api/session",route=>route.abort("failed"));
    await page.goto(fixture.url);await page.locator("#gate-retry").waitFor({state:"visible"});
    await shot(page,"gate-read-failure");await page.unroute("**/api/session");await page.locator("#gate-retry").click();
    await page.locator("#bootstrap-code").waitFor({state:"visible"});await shot(page,"initialization-desktop");
    checks.push("initial read failure and reconnect to uninitialized service");
    assert.equal((await call(page,"api/config")).status,401);
    await page.locator("#bootstrap-code").fill(fixture.code);await login(page);
    await page.waitForFunction(()=>document.querySelector("#ready-title").textContent==="尚未就绪");
    await shot(page,"overview-unconfigured");checks.push("unauthenticated rejection, initialization, real unconfigured overview");
    await navigate(page,"config");
    await page.locator("#notice").waitFor({state:"hidden"});
    for(const name of ["public_base_url","qwen_url","siyuan_url"])assert.notEqual(await page.locator(`[name="${name}"]`).getAttribute("readonly"),null);
    assert(await page.locator('[name="qwen_model"]').isDisabled());
    assert.equal(await page.locator("#activate-config,#migration-dialog,.demo-banner,.dummy-page").count(),0);
    const before=count("/api/config");
    await page.locator('[name="qwen_api_key"]').fill("browser-synthetic-key-A");await page.locator('[name="siyuan_token"]').fill("browser-synthetic-token-A");
    // Hold the real response to verify busy state and single-save semantics.
    let release;const hold=new Promise(resolve=>release=resolve);
    await page.route("**/api/config",async route=>{if(route.request().method()==="POST"){const response=await route.fetch();await hold;await route.fulfill({response});}else await route.continue();});
    await page.locator("#save-config").click();await page.waitForFunction(()=>document.querySelector("#save-config").disabled);
    assert.equal(count("/api/config"),before+1);release();await notice(page,"配置已保存并生效");await page.waitForFunction(()=>!document.querySelector("#save-config").disabled);await page.unroute("**/api/config");
    const initial=(await call(page,"api/config")).value;
    assert(initial.active_revision);assert(!JSON.stringify(initial).includes("browser-synthetic-key-A"));
    assert.equal(await page.locator('[name="qwen_api_key"]').inputValue(),"");
    assert.equal(await page.locator('[name="siyuan_token"]').inputValue(),"");checks.push("fixed fields, one save activates, busy feedback, secrets never echoed");
    await save(page);const retained=(await call(page,"api/config")).value;
    assert(retained.config.qwen_api_key_configured&&retained.config.siyuan_token_configured);checks.push("blank fields preserve both credentials");
    await page.locator('[name="qwen_api_key"]').fill("browser-synthetic-key-B");await page.locator('[name="clear_qwen"]').check();
    let n=count("/api/config");await page.locator("#save-config").click();await notice(page,"替换和清除");assert.equal(count("/api/config"),n);checks.push("replace/clear conflict rejected before POST");
    await page.locator('[name="qwen_api_key"]').fill("");await save(page);
    assert.equal((await call(page,"api/config")).value.config.qwen_api_key_configured,false);checks.push("explicit clear changes actual configuration");
    await page.locator('[name="qwen_api_key"]').fill("browser-synthetic-key-B");await save(page);
    await page.locator("#validate-config").click();await notice(page,"尚未保存，也未验证供应商鉴权");checks.push("validation has accurate non-auth feedback");
    let dropped=false;
    await page.route("**/api/config",async route=>{if(route.request().method()==="POST"&&!dropped){dropped=true;await route.fetch();await route.abort("failed");}else await route.continue();});
    await page.locator('[name="qwen_api_key"]').fill("browser-synthetic-key-C");await page.locator("#save-config").click();await notice(page,"无法连接服务");
    await page.waitForFunction(()=>document.querySelector('[name="qwen_api_key"]').value==="browser-synthetic-key-C"&&!document.querySelector("#save-config").disabled);
    await shot(page,"configuration-response-lost");await page.unroute("**/api/config");await save(page);
    const final=(await call(page,"api/config")).value;assert.notEqual(final.active_revision,initial.active_revision);assert.equal(final.snapshot_count,1);checks.push("lost save response preserves draft, rereads revision and can retry");
    await page.locator("#probe-qwen").click();await page.waitForFunction(()=>document.querySelector("#probe-result").textContent.includes("TLS 连接通过"));
    assert.match(await page.locator("#probe-result").innerText(),/鉴权未检查/);
    mode({probe:"fail"});await page.locator("#probe-siyuan").click();await page.waitForFunction(()=>document.querySelector("#probe-result").classList.contains("probe-failed"));
    await shot(page,"probe-failure");mode({probe:"ok"});await page.locator("#probe-siyuan").click();await page.waitForFunction(()=>!document.querySelector("#probe-result").classList.contains("probe-failed"));checks.push("actual probe parser success/failure, TLS and auth separate");
    n=count("/api/config/probe");await page.locator("#test-delivery").click();
    assert.match(await page.locator("#confirm-copy").innerText(),/费用/);assert.match(await page.locator("#confirm-copy").innerText(),/合成静音/);
    await shot(page,"confirmation-desktop");await dialog(page,"cancel");assert.equal(count("/api/config/probe"),n);
    await page.locator("#test-delivery").click();await dialog(page,"confirm");await page.waitForFunction(()=>document.querySelector("#probe-result").textContent.includes("测试条目已创建"));
    assert.equal(count("/api/config/probe"),n+1);assert.match(await page.locator("#probe-result").innerText(),/合成静音请求已通过/);checks.push("end-to-end cancel sends nothing, confirm calls real app and both synthetic transports");
    await navigate(page,"devices");assert(await page.locator("#devices-empty").isVisible());
    await page.locator("#issue-code").click();await page.locator("#pairing-panel").waitFor({state:"visible"});
    const code=await page.locator("#pairing-code").innerText();await page.locator("#copy-code").click();await notice(page,"配对码已复制");assert.equal(await page.evaluate(()=>navigator.clipboard.readText()),code);
    const device=await pair(page,"browser-future-model",code);await page.locator("#refresh").click();
    const row=page.locator("#devices-body tr").filter({hasText:device.device_id});await row.waitFor();
    await row.getByRole("button",{name:"撤销",exact:true}).click();n=count("/revoke");await dialog(page,"cancel");assert.equal(count("/revoke"),n);
    await row.getByRole("button",{name:"撤销",exact:true}).click();await dialog(page,"confirm");await notice(page,"设备授权已撤销");await page.waitForFunction(()=>document.querySelector("#devices-body").textContent.includes("已撤销"));checks.push("device empty state, actual pairing, clipboard, revoke cancel/confirm");
    const working=await pair(page,"browser-active-model");
    for(let i=0;i<20;i++)await pair(page,"browser-page-model-"+String(i).padStart(2,"0"));
    await page.locator("#refresh").click();await page.waitForFunction(()=>!document.querySelector("#devices-next").disabled);await page.locator("#devices-next").click();await page.waitForFunction(()=>document.querySelector("#devices-page").textContent==="第 2 页");await page.locator("#devices-prev").click();await page.waitForFunction(()=>document.querySelector("#devices-page").textContent==="第 1 页");await shot(page,"devices-desktop");checks.push("actual device pagination forward/back");
    await navigate(page,"jobs");assert(await page.locator("#jobs-empty").isVisible());
    mode({delivery:"fail"});await upload(page,working,1);await taskState(page,working,1,"delivery_failed");await page.locator("#refresh").click();
    const failed=jobRow(page,1);await failed.getByRole("button",{name:"重试",exact:true}).click();n=count("/retry");await dialog(page,"cancel");assert.equal(count("/retry"),n);
    mode({delivery:"ok"});await failed.getByRole("button",{name:"重试",exact:true}).click();await dialog(page,"confirm");await notice(page,"重试已受理");await taskState(page,working,1,"sent");checks.push("job empty/failure, retry cancel/confirm, actual recovered delivery");
    mode({delivery:"uncertain"});await upload(page,working,2);await taskState(page,working,2,"uncertain");await page.locator("#refresh").click();
    let uncertain=jobRow(page,2);await uncertain.getByRole("button",{name:"思源中已存在",exact:true}).waitFor();assert.equal(await uncertain.getByRole("button",{name:"重试",exact:true}).count(),0);
    await uncertain.getByRole("button",{name:"思源中已存在",exact:true}).click();n=count("/reconcile");await dialog(page,"cancel");assert.equal(count("/reconcile"),n);
    await uncertain.getByRole("button",{name:"思源中已存在",exact:true}).click();await dialog(page,"confirm");await taskState(page,working,2,"sent");
    await upload(page,working,3);await taskState(page,working,3,"uncertain");await page.locator("#refresh").click();
    uncertain=jobRow(page,3);await uncertain.getByRole("button",{name:"思源中不存在",exact:true}).click();await dialog(page,"confirm");await taskState(page,working,3,"delivery_failed");checks.push("uncertain has no direct retry, reconcile cancel and both present/absent paths");
    mode({delivery:"ok"});for(let i=4;i<=22;i++){await upload(page,working,i);await taskState(page,working,i,"sent");}
    await page.locator("#refresh").click();await page.waitForFunction(()=>!document.querySelector("#jobs-next").disabled);await page.locator("#jobs-next").click();await page.waitForFunction(()=>document.querySelector("#jobs-page").textContent==="第 2 页");await page.locator("#jobs-prev").click();await page.waitForFunction(()=>document.querySelector("#jobs-page").textContent==="第 1 页");await shot(page,"jobs-desktop");checks.push("actual job pagination and source/status information");
    await navigate(page,"overview");await page.waitForFunction(()=>document.querySelector("#ready-title").textContent==="服务已就绪");await shot(page,"overview-ready");
    for(const id of ["instance","config-summary","cleanup-summary","version-summary","worker-summary","decoder-summary","storage-summary"])assert.notEqual(await page.locator("#"+id).innerText(),"—");checks.push("real configured overview and retained diagnostics");
    await navigate(page,"config");
    await page.locator("#notice").waitFor({state:"hidden"});
    // Visual reference: same browser/font/viewport, remove ONLY prototype banner,
    // normalize fictional text/state/values. Never inject substitute CSS.
    const reference=await context.newPage();await reference.goto(pathToFileURL(path.join(root,"docs/design/admin-approved/index.html")).href);
    await reference.evaluate(()=>document.querySelector(".demo-banner").remove());
    const copy=await page.evaluate(()=>({values:Object.fromEntries(["public_base_url","qwen_url","qwen_model","siyuan_url"].map(name=>[name,document.querySelector(`[name="${name}"]`).value])),texts:Object.fromEntries(["revision-badge","qwen-state","siyuan-state"].map(id=>[id,document.getElementById(id).textContent]))}));
    await reference.evaluate(copy=>{for(const [name,value]of Object.entries(copy.values))document.querySelector(`[name="${name}"]`).value=value;for(const[id,value]of Object.entries(copy.texts)){const node=document.getElementById(id);node.textContent=value;if(id==="siyuan-state")node.style.color="";}},copy);
    for(const [size,label]of [[{width:1280,height:720},"desktop"],[{width:390,height:844},"mobile"]]){
      await page.locator("#notice").waitFor({state:"hidden"});
      await page.setViewportSize(size);await reference.setViewportSize(size);
      if(label==="mobile"){
        const savesBefore=count("/api/config");
        await save(page);assert((await call(page,"api/config")).value.config.siyuan_token_configured);
        await page.locator('[name="clear_siyuan"]').check();await save(page);
        const cleared=(await call(page,"api/config")).value;
        assert.equal(cleared.config.siyuan_token_configured,false);assert.equal(cleared.config.qwen_api_key_configured,true);
        assert.equal(await page.locator("#siyuan-state").innerText(),"未配置");
        await shot(page,"config-mobile-token-cleared");
        await page.locator('[name="siyuan_token"]').fill("browser-synthetic-token-D");await save(page);
        assert.equal(count("/api/config"),savesBefore+3);
        assert((await call(page,"api/config")).value.config.siyuan_token_configured);
        assert.equal(await page.locator('[name="siyuan_token"]').inputValue(),"");
        const revision=await page.locator("#revision-badge").innerText();
        await reference.evaluate(revision=>document.querySelector("#revision-badge").textContent=revision,revision);
        await page.locator("#notice").waitFor({state:"hidden"});
        checks.push("mobile viewport actual blank save, explicit token clear and restore, three POSTs and true configuration state");
      }
      for(const p of [page,reference])await p.evaluate(()=>document.querySelector("main").scrollTop=0);
      await geometry(page,"actual-"+label);await geometry(reference,"reference-"+label);
      await shot(page,"config-"+label+"-top");await shot(reference,"reference-"+label+"-top");
      for(const p of [page,reference])await p.locator('[name="qwen_api_key"]').focus();
      await shot(page,"config-"+label+"-focus");await shot(reference,"reference-"+label+"-focus");
      for(const p of [page,reference])await p.evaluate(()=>document.querySelector('[name="siyuan_token"]').closest("fieldset").scrollIntoView({block:"start"}));
      await shot(page,"config-"+label+"-siyuan");await shot(reference,"reference-"+label+"-siyuan");
      for(const p of [page,reference])await p.evaluate(()=>document.querySelector("main").scrollTop=100000);
      await shot(page,"config-"+label+"-bottom");await shot(reference,"reference-"+label+"-bottom");
      await page.locator("#test-delivery").click();await shot(page,"confirmation-"+label);assert(await page.locator('#confirm-dialog button[value="cancel"]').isVisible());await dialog(page,"cancel");
      await page.waitForFunction(()=>!document.querySelector("#test-delivery").disabled);
      await page.waitForFunction(()=>document.activeElement.id==="test-delivery");
      await reference.locator("#test-delivery").click();
      await reference.evaluate(()=>{const dialog=document.querySelector("#confirm-dialog");dialog.querySelector(".eyebrow").textContent="请确认";dialog.querySelector("h2").textContent="执行一次实际调用测试？";dialog.querySelector("p:not(.eyebrow)").textContent="将使用固定合成静音调用千问，并在当前思源收集箱创建一条测试记录，可能产生服务费用。两项结果会分别显示。";});
      await shot(reference,"reference-confirmation-"+label);await dialog(reference,"cancel");
      await page.locator("#validate-config").click();await notice(page,"填写内容符合要求");await shot(page,"notice-"+label);
      await reference.locator("#validate-config").click();await reference.evaluate(()=>document.querySelector("#notice").textContent="填写内容符合要求；尚未保存，也未验证供应商鉴权。");await shot(reference,"reference-notice-"+label);
      if(label==="mobile"){
        await page.locator("#notice").waitFor({state:"hidden"});
        for(const name of ["overview","devices","jobs"]){await navigate(page,name);await page.evaluate(()=>document.querySelector("main").scrollTop=0);await shot(page,name+"-mobile");await page.evaluate(()=>document.querySelector("main").scrollTop=100000);await shot(page,name+"-mobile-bottom");}
      }
    }
    await reference.close();checks.push("desktop/mobile full configuration, focus, navigation, notification, modal and other pages captured against reference");
    for(let i=0;i<layout.length;i+=2){assert.deepEqual(layout[i].colors,layout[i+1].colors);assert.equal(layout[i].font,layout[i+1].font);for(const selector of [".rail",".topbar",".page-heading","#config-title","#config-form",".two-fields"]){for(const field of ["x","y","width","height"])assert(Math.abs(layout[i].nodes[selector][field]-layout[i+1].nodes[selector][field])<1,`${layout[i].label} ${selector} ${field} diverged`);}}
    await context.clearCookies();await page.locator("#refresh").click();await page.locator("#auth-form").waitFor({state:"visible"});await notice(page,"登录已失效");checks.push("session loss returns to login, actual HTTP denial");
    await page.locator("#password").fill("wrong-synthetic-password");await page.locator("#auth-submit").click();await notice(page,"口令不正确");await shot(page,"login-error-mobile");await login(page);
    await page.locator("#logout").click();await page.locator("#auth-form").waitFor({state:"visible"});assert.equal((await call(page,"api/config")).status,401);assert.equal(await page.locator("#pairing-code").innerText(),"");await shot(page,"login-mobile");await login(page);checks.push("failed login, logout clears pairing/config drafts, successful relogin");
    assert.deepEqual(errors,[]);
    const report={status:"PASS",scope:"Actual isolated HTTPS ASGI app under /memo; synthetic providers, generated credentials and silent WAV only",checks,screenshots,layout,requests,javascript_errors:errors,browser_version:await browser.version(),playwright_version:require(path.join(modulePath,"package.json")).version,tls_note:"Self-signed fixture exception only in this isolated context; production security unchanged"};
    fs.writeFileSync(path.join(output,"result.json"),JSON.stringify(report,null,2)+"\n",{mode:0o600});
    console.log(JSON.stringify({status:report.status,scenarios:checks.length,screenshots:screenshots.length,browser_version:report.browser_version}));
  }finally{await context.close();await browser.close();}
})().catch(error=>{fs.writeFileSync(path.join(output,"failure.txt"),error.stack+"\n",{mode:0o600});fs.writeFileSync(path.join(output,"partial.json"),JSON.stringify({status:"FAIL",checks,layout,requests,errors},null,2),{mode:0o600});console.error(error.stack);process.exitCode=1;});
