'use strict';
// Production portal DOM + actual native AP/config bodies served by service_portal_fixture.py.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const modulePath=process.env.PLAYWRIGHT_MODULE;assert(modulePath,'set PLAYWRIGHT_MODULE to pinned 1.62.1');assert.equal(require(path.join(modulePath,'package.json')).version,'1.62.1');
const {chromium}=require(modulePath),root=path.resolve(__dirname,'..'),out=path.resolve(process.argv[2]);assert(out.startsWith(path.join(root,'private')+path.sep));
const {url}=JSON.parse(fs.readFileSync(path.join(out,'fixture.json')));assert.match(url,/^http:\/\/127\.0\.0\.1:\d+\/$/);
const checks=[],layouts=[],shots=[],requests=[];let browser;
async function control(page,value){await page.request.post(url+'__control',{data:value})}
async function ready(page){await page.waitForFunction(()=>!document.getElementById('service-save').disabled)}
async function checkLayout(page,name){
 const value=await page.evaluate(()=>({scroll:document.documentElement.scrollWidth,width:innerWidth,inputs:[...document.querySelectorAll('input,button,select')].filter(e=>!e.hidden).map(e=>{const r=e.getBoundingClientRect();return {id:e.id,x:r.x,right:r.right,width:r.width,height:r.height}})}));
 assert(value.scroll<=value.width,name+' page overflow');for(const r of value.inputs){assert(r.x>=0&&r.right<=value.width,name+' control out of view '+r.id)}layouts.push({name,...value});
 await page.screenshot({path:path.join(out,name+'.png'),fullPage:true});shots.push(name+'.png');
}
(async()=>{
 browser=await chromium.launch({executablePath:process.env.CHROME_EXECUTABLE,headless:true});const page=await browser.newPage({viewport:{width:1280,height:1000}});
 page.on('request',r=>{if(r.method()==='POST')requests.push({path:new URL(r.url()).pathname,body:r.postDataJSON()})});
 await page.goto(url);await ready(page);await page.waitForFunction(()=>document.querySelector('#networks').options.length===2);
 assert.match(await page.locator('#service-status').innerText(),/仅本地/);assert.equal(await page.locator('#server-url').isDisabled(),true);await checkLayout(page,'desktop-local');checks.push('fresh local-only, scan de-duplicates and escapes SSID');
 await page.locator('#networks').selectOption('Home <&>');assert.equal(await page.locator('#ssid').inputValue(),'Home <&>');await page.locator('#password').fill('synthetic-only');
 // Delay actual request response to prove duplicate event guards, not merely disabled styling.
 await page.route('**/submit',async route=>{await new Promise(r=>setTimeout(r,250));await route.continue()});
 await page.locator('#wifi-save').click();await page.locator('#wifi-form').evaluate(e=>e.dispatchEvent(new Event('submit',{bubbles:true,cancelable:true})));await ready(page);
 assert.equal(requests.filter(r=>r.path==='/submit').length,1);assert.match(await page.locator('#wifi-feedback').innerText(),/已连接并保存/);assert.equal(await page.locator('#password').inputValue(),'');assert(!requests.some(r=>r.path==='/exit'));checks.push('independent Wi-Fi commit, duplicate prevention, no automatic exit');
 await control(page,{wifi_fail:true});await page.locator('#wifi-save').click();await ready(page);assert.match(await page.locator('#wifi-feedback').innerText(),/Wi-Fi 连接失败/);await control(page,{});checks.push('Wi-Fi failure retains usable retry');
 await page.locator('#local-only').uncheck();await page.locator('#server-url').fill('http://invalid.example');const before=requests.length;await page.locator('#service-save').click();assert.equal(requests.length,before);assert.match(await page.locator('#service-feedback').innerText(),/HTTPS/);
 await page.locator('#server-url').fill('https://RELAY.example:9443/memo');await page.locator('#pairing').fill('23456789ab');await page.locator('#service-save').click();await ready(page);
 assert.match(await page.locator('#service-status').innerText(),/关闭热点后联网配对/);assert.equal(await page.locator('#pairing').inputValue(),'');assert.equal(await page.locator('#server-url').inputValue(),'https://relay.example:9443/memo');checks.push('HTTPS validation + actual normalized durable pending; no premature pairing success');
 await control(page,{paired:true});await page.waitForFunction(()=>document.querySelector('#service-status').textContent==='服务已配对');await checkLayout(page,'desktop-paired');checks.push('redacted current-service paired result');
 await control(page,{busy:true});await page.locator('#server-url').fill('https://new.example/path');await page.locator('#pairing').fill('23456789AB');await page.locator('#service-save').click();await ready(page);assert.match(await page.locator('#service-feedback').innerText(),/正在处理/);assert.equal(await page.locator('#pairing').inputValue(),'23456789AB');await control(page,{});checks.push('unique audio busy: visible failure, inputs retained, retry enabled');
 await control(page,{fault:2});await page.locator('#service-save').click();await ready(page);assert.match(await page.locator('#service-feedback').innerText(),/未确认/);await control(page,{});await page.locator('#service-save').click();await ready(page);assert.match(await page.locator('#service-status').innerText(),/关闭热点后联网配对/);checks.push('actual persistence fault + successful manual retry');
 // Lose a successful native save response; the next ordinary retry is identical and idempotent.
 await page.route('**/service',async route=>{if(route.request().method()==='POST'){await route.fetch();await route.abort();await page.unroute('**/service')}else await route.continue()});
 await page.locator('#local-only').check();await page.locator('#service-save').click();await ready(page);assert.match(await page.locator('#service-feedback').innerText(),/网络响应未确认/);await page.locator('#service-save').click();await ready(page);assert.match(await page.locator('#service-status').innerText(),/仅本地/);checks.push('lost commit response, ordinary retry recovers local-only');
 let dropped=false;await page.route('**/service',async route=>{if(route.request().method()==='GET'&&!dropped){dropped=true;await route.abort()}else await route.continue()});
 await page.waitForFunction(()=>document.querySelector('#service-status').textContent.includes('设置读取失败'));assert.equal(await page.locator('#service-save').isDisabled(),true);await ready(page);await page.unroute('**/service');checks.push('temporary status failure recovers automatically, buttons regain availability');
 const mobile=await browser.newPage({viewport:{width:390,height:844},deviceScaleFactor:1,isMobile:true,hasTouch:true});await mobile.goto(url);await ready(mobile);await checkLayout(mobile,'mobile-local');await mobile.locator('#local-only').uncheck();await checkLayout(mobile,'mobile-service');
 const narrow=await browser.newPage({viewport:{width:320,height:640},deviceScaleFactor:1,isMobile:true,hasTouch:true});await narrow.goto(url);await ready(narrow);await narrow.locator('#local-only').uncheck();
 for(const id of ['ssid','password','local-only','server-url','pairing','service-save','service-status','exit']){
   const element=narrow.locator('#'+id);await element.scrollIntoViewIfNeeded();assert(await element.isVisible(),id+' missing in 320 viewport');
   const bounds=await element.boundingBox();assert(bounds.x>=0&&bounds.x+bounds.width<=320,id+' offscreen in 320 viewport');
 }
 await narrow.evaluate(()=>scrollTo(0,0));await checkLayout(narrow,'mobile-320');checks.push('390/320 fresh browser contexts, all controls visible after actual scroll');
 await mobile.locator('#exit').click();await ready(mobile);assert.match(await mobile.locator('#exit-feedback').innerText(),/热点即将关闭/);checks.push('explicit exit uses protected actual handler');
 fs.writeFileSync(path.join(out,'browser-results.json'),JSON.stringify({status:'PASS',browser_version:browser.version(),playwright_version:require(path.join(modulePath,'package.json')).version,checks,layouts,shots,requests,boundaries:['loopback fixture; Host/Origin mapping','ESP httpd/RF/NVS/task shims','paired receipt controlled via actual current-config commit, provider pairing covered separately','physical phone/hardware/listening not run']},null,2));
 console.log('PASS portal browser:',checks.length,'behavior groups,',layouts.length,'layouts');
})().catch(e=>{console.error(e);process.exitCode=1}).finally(async()=>{if(browser)await browser.close()});
