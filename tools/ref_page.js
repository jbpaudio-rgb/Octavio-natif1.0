// Reference screen of the prototype: node ref_page.js OUT.png [BUTTON ...] [-- js]   buttons are the panel's labels (ENV, LFO, FX, ARP, HOME, EDIT, GLO ...)
const {chromium}=require('playwright');
const out=process.argv[2];const rest=process.argv.slice(3);const di=rest.indexOf('--');
const btns=di<0?rest:rest.slice(0,di);const js=di<0?null:rest.slice(di+1).join(' ');
(async()=>{const b=await chromium.launch();const p=await b.newPage({viewport:{width:1100,height:1000},deviceScaleFactor:1});
await p.goto('file:///root/.claude/projects/-home-claude/9ce91c03-575c-5b77-9332-be0b542a3ae1/tool-results/artifact-6a194d38-1793292356-7d6c.html');
await p.waitForTimeout(500);
await p.addStyleTag({content:'.panel .fp .scope{width:240px!important;min-width:240px!important;height:240px!important;padding:0!important;aspect-ratio:auto!important;position:fixed!important;left:0;top:0;z-index:99999;flex:none}'});
await p.evaluate(()=>{document.getElementById('pwrsw').click()});
await p.waitForTimeout(4500);
for(const t of btns){await p.evaluate(t=>{[...document.querySelectorAll('.bt button')].find(b=>b.textContent===t);const e=(n)=>b0.dispatchEvent(new PointerEvent(n,{bubbles:true}));var b0=[...document.querySelectorAll('.bt button')].find(b=>b.textContent===t);b0.click();e('pointerdown');e('pointerup')},t);await p.waitForTimeout(300)}
if(js){await p.evaluate(js);await p.waitForTimeout(400)}
await p.waitForTimeout(500);
await p.screenshot({path:out,clip:{x:0,y:0,width:240,height:240}});
await b.close()})();
