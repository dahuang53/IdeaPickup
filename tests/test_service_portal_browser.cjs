'use strict';
// Isolated headless browser, production DOM + native AP/NVS fixture.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const modulePath=process.env.PLAYWRIGHT_MODULE;assert(modulePath);assert.equal(require(path.join(modulePath,'package.json')).version,'1.62.1');
const {chromium}=require(modulePath),root=path.resolve(__dirname,'..'),out=path.resolve(process.argv[2]);assert(out.startsWith(path.join(root,'private')+path.sep));
const {url}=JSON.parse(fs.readFileSync(path.join(out,'fixture.json')));assert.match(url,/^http:\/\/127\.0\.0\.1:\d+\/$/);
let browser;const checks=[],layouts=[],requests=[];
const action=(p,name)=>p.locator(`[data-action="${name}"]`);
async function control(p,v){await p.request.post(url+'__control',{data:v});}
async function ready(p){await p.waitForFunction(()=>document.querySelector('.a-ap-pill')?.textContent.includes('热点已连'));}
async function layout(p,name){const v=await p.evaluate(()=>({text:[...document.querySelectorAll('code')].flatMap(e=>{if(!e.getClientRects().length)return [];const range=document.createRange();range.selectNodeContents(e);const parent=e.parentElement.getBoundingClientRect();return [...range.getClientRects()].map(r=>({x:r.x,right:r.right,left:parent.left,bound:parent.right}));}),scroll:document.documentElement.scrollWidth,width:innerWidth,controls:[...document.querySelectorAll('input,button,select')].map(e=>{const r=e.getBoundingClientRect();return {id:e.id,width:r.width,x:r.x,right:r.right};})}));assert(v.scroll<=v.width);for(const t of v.text)assert(t.x>=t.left-1&&t.right<=t.bound+1,`clipped code text at ${name}`);for(const c of v.controls)assert(c.x>=0&&c.right<=v.width);layouts.push({name,...v});await p.screenshot({path:path.join(out,name+'.png'),fullPage:true});}
(async()=>{
 browser=await chromium.launch({executablePath:process.env.CHROME_EXECUTABLE,headless:true});const p=await browser.newPage({viewport:{width:1280,height:1000}});
 p.on('request',r=>{if(r.method()==='POST')requests.push({path:new URL(r.url()).pathname,body:r.postDataJSON()});});
 await p.goto(url);await ready(p);assert.equal(await p.title(),'IdeaPickup · 联网与服务设置');assert.equal(await p.locator('.preview-workbench').count(),0);assert.equal(await p.locator('[data-mode="local"]').getAttribute('aria-pressed'),'true');
 await action(p,'toggle-a-wifi').click();await p.locator('#wifi-net-select').click();await p.locator('[data-wifi-opt="ap:0"]').click();await p.locator('#wifi-pwd-input').fill('synthetic-password');await action(p,'submit-wifi').click();await control(p,{wifi_process:true});await p.waitForFunction(()=>document.querySelector('.a-status-tag')?.textContent.includes('已连'));checks.push('actual async Wi-Fi save and real persisted status');
 await p.locator('[data-mode="service"]').click();assert.equal(await p.locator('#srv-url-input,#srv-code-input').count(),0);assert.equal(await action(p,'submit-service').isDisabled(),true);
 await layout(p,'desktop-unconfigured');await p.locator('#qwen-key-input').fill('fixture-browser-key');await p.locator('#siyuan-token-input').fill('fixture-browser-token');
 await action(p,'save-credentials').click();await p.waitForFunction(()=>!document.querySelector('[data-action="submit-service"]')?.disabled);
 assert.equal(await p.locator('#qwen-key-input').inputValue(),'');assert.equal(await p.locator('#siyuan-token-input').inputValue(),'');checks.push('web credential save, no echo and no legacy inputs');
 await action(p,'submit-service').click();await p.waitForFunction(()=>[...document.querySelectorAll('.a-status-tag')].some(e=>e.textContent.includes('凭据已保存')));assert.match(await p.locator('.a-row-item').nth(1).textContent(),/尚未验证/);checks.push('saved mode has no false verified/paired result');
 await control(p,{task_state:9});await p.reload();await ready(p);assert.match(await p.locator('.a-row-item').nth(1).textContent(),/已送达思源/);checks.push('actual task result is distinct from credential status');
 for(const width of [320,360,390,768]){await p.setViewportSize({width,height:900});await action(p,'toggle-a-wifi').click().catch(()=>{});await layout(p,'width-'+width);}
 await p.setViewportSize({width:320,height:900});if(await action(p,'toggle-a-service').count())await action(p,'toggle-a-service').click();await layout(p,'width-320-settings');
 await p.locator('[data-mode="local"]').click();await action(p,'submit-service').click();await p.waitForFunction(()=>[...document.querySelectorAll('.a-status-tag')].some(e=>e.textContent.trim()==='✓ 仅本地使用'));await p.locator('[data-mode="service"]').click();if(await action(p,'toggle-a-service').count())await action(p,'toggle-a-service').click();
 p.once('dialog',d=>d.accept());await action(p,'clear-qwen').click();await p.waitForFunction(()=>document.body.textContent.includes('未配置'));checks.push('explicit clear switches to local and preserves other provider');
 await p.reload();await ready(p);await layout(p,'after-clear');assert.equal(await p.evaluate(()=>localStorage.length+sessionStorage.length),0);assert.equal(requests.filter(r=>'qwen_key' in r.body||'siyuan_token' in r.body).length,1);
 await action(p,'exit-hotspot').click();await p.waitForFunction(()=>document.body.textContent.includes('热点已关'));checks.push('explicit exit and no credential browser persistence');
 fs.writeFileSync(path.join(out,'browser-result.json'),JSON.stringify({status:'PASS',scope:'headless production DOM, actual native AP/settings; not iPhone/RF',checks,layouts},null,2));console.log(JSON.stringify({status:'PASS',checks,layouts:layouts.map(x=>x.name)}));
})().catch(e=>{console.error(e);process.exitCode=1;}).finally(async()=>{if(browser)await browser.close();});
