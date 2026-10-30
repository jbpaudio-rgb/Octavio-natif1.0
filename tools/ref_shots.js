const {chromium}=require('playwright');
const out=process.argv[2];
const shots=JSON.parse(require('fs').readFileSync(process.argv[3]));
(async()=>{const b=await chromium.launch();const p=await b.newPage({viewport:{width:1100,height:1000},deviceScaleFactor:1});
await p.goto('file:///root/.claude/projects/-home-claude/9ce91c03-575c-5b77-9332-be0b542a3ae1/tool-results/artifact-6a194d38-1793292356-7d6c.html');
await p.waitForTimeout(500);
await p.addStyleTag({content:'.panel .fp .scope{width:240px!important;min-width:240px!important;height:240px!important;padding:0!important;aspect-ratio:auto!important;position:fixed!important;left:0;top:0;z-index:99999;flex:none}'});
await p.evaluate(()=>{document.getElementById('pwrsw').click()});
await p.waitForTimeout(4500);
await p.evaluate(()=>{performance.now=()=>123456});
for(const [name,js] of shots){await p.evaluate(js);await p.waitForTimeout(500);await p.screenshot({path:out+'/'+name+'.png',clip:{x:0,y:0,width:240,height:240}})}
await b.close()})();
