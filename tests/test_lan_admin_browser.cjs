'use strict';
// Real HTTP on a private LAN interface: no secure-context override, TLS bypass,
// application mocks or external provider calls. Fixture credentials are synthetic.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const modulePath=process.env.PLAYWRIGHT_MODULE;
assert.equal(require(path.join(modulePath,'package.json')).version,'1.62.1');
const {chromium}=require(modulePath),out=path.resolve(process.argv[2]);
assert(out.startsWith(path.resolve(__dirname,'../private')+path.sep));
const fixture=JSON.parse(fs.readFileSync(path.join(out,'fixture.json')));
const url=new URL(fixture.url);assert.equal(url.protocol,'http:');
assert(!['localhost','127.0.0.1'].includes(url.hostname));
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROME_EXECUTABLE,headless:true});
 const context=await browser.newContext(),page=await context.newPage(),errors=[],checks=[];
 page.on('pageerror',e=>errors.push(e.message));
 try {
  await page.goto(fixture.url);
  assert.equal(await page.evaluate(()=>isSecureContext),false);
  await page.locator('#bootstrap-code').fill(fixture.code);
  await page.locator('#password').fill(fixture.password);
  await page.locator('#auth-submit').click();await page.locator('#workspace').waitFor({state:'visible'});
  const cookies=await context.cookies();assert.equal(cookies.length,1);
  assert.equal(cookies[0].name,'voicememo-admin-lan');assert.equal(cookies[0].secure,false);
  assert.equal(cookies[0].httpOnly,true);assert.equal(cookies[0].sameSite,'Strict');
  checks.push('actual non-secure LAN origin initializes/logs in with accepted HttpOnly Strict cookie');
  await page.reload();await page.locator('#workspace').waitFor({state:'visible'});
  await page.locator('[data-page="devices"]').click();await page.locator('#issue-code').click();
  await page.locator('#pairing-panel').waitFor({state:'visible'});
  assert.match(await page.locator('#pairing-code').innerText(),/^[A-Z0-9]{10}$/);
  await page.locator('#copy-code').click();
  await page.waitForFunction(()=>document.querySelector('#notice').textContent.includes('配对码已复制'));
  checks.push('refresh retains session; actual pairing code generation and HTTP clipboard fallback');
  const denied=await page.evaluate(async()=>{
   const r=await fetch(new URL('api/devices/pairing-code',document.baseURI),{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});return r.status;
  });assert.equal(denied,403);checks.push('browser cookie alone cannot bypass CSRF');
  await page.screenshot({path:path.join(out,'lan-devices.png'),fullPage:true});
  await page.locator('#logout').click();await page.locator('#gate').waitFor({state:'visible'});
  await page.reload();await page.locator('#password').fill(fixture.password);
  await page.locator('#auth-submit').click();await page.locator('#workspace').waitFor({state:'visible'});
  checks.push('logout and fresh login work on HTTP');assert.deepEqual(errors,[]);
  fs.writeFileSync(path.join(out,'browser-result.json'),JSON.stringify({status:'PASS',checks,errors},null,2));
  console.log('PASS LAN admin browser: '+checks.length+' checks');
 } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
