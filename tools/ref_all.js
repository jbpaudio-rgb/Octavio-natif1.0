// node ref_all.js OUTDIR : screenshots + text dump of every page of the prototype (Vactrol engine)
const {chromium}=require('playwright');const fs=require('fs');const out=process.argv[2];fs.mkdirSync(out,{recursive:true});
const S=[['lfo0',['LFO']],['lfo1',['LFO','LFO']],['lfo2',['LFO','LFO','LFO']],['lfo3',['LFO','LFO','LFO','LFO']],['lfo4',['LFO','LFO','LFO','LFO','LFO']],['lfo5',['LFO','LFO','LFO','LFO','LFO','LFO']]];
for(let i=0;i<6;i++)S.push(['fx'+i,['FX'].concat(Array(i).fill('FX'))]);
S.push(['lfo0on',['LFO'],'(()=>{const e=document.querySelector(\'.md[data-k="m0y"]\');e.checked=true;e.dispatchEvent(new Event("input"));page()})()'],['arp',['ARP']],['arp2',[],'pg="ARP2";page()'],['arp3',[],'pg="ARP3";page()'],['home',['HOME']],['seq',[],'pg="SEQ";page()'],['tempo',[],'pg="TEMPO";page()'],['menu',[],'pg="MENU";page()'],['menu2',[],'pg="MENU2";page()'],['glo',['EDIT']],['edit',['GLO']],['sel',['FX'],'pg="FXO";page()']);
(async()=>{const b=await chromium.launch();const p=await b.newPage({viewport:{width:1100,height:1000},deviceScaleFactor:1});
await p.goto('file:///root/.claude/projects/-home-claude/9ce91c03-575c-5b77-9332-be0b542a3ae1/tool-results/artifact-6a194d38-1793292356-7d6c.html');
await p.waitForTimeout(500);
await p.addStyleTag({content:'.panel .fp .scope{width:240px!important;min-width:240px!important;height:240px!important;padding:0!important;aspect-ratio:auto!important;position:fixed!important;left:0;top:0;z-index:99999;flex:none}'});
await p.evaluate(()=>{document.getElementById('pwrsw').click()});await p.waitForTimeout(4500);
const dump={};
for(const [n,btns,js] of S){
 await p.evaluate(()=>{pg="GLO";fxo=0;src=0;page()});
 for(const t of btns){await p.evaluate(t=>{var b0=[...document.querySelectorAll('.bt button')].find(b=>b.textContent===t);const e=(n)=>b0.dispatchEvent(new PointerEvent(n,{bubbles:true}));b0.click();e('pointerdown');e('pointerup')},t);await p.waitForTimeout(500)}
 if(js){await p.evaluate(js);await p.waitForTimeout(300)}
 await p.waitForTimeout(500);
 dump[n]=await p.evaluate(()=>({pg,title:document.getElementById('lft').textContent,cells:[...document.querySelectorAll('#lfk .kk')].map(k=>[k.querySelector('.kt span')?.textContent,k.querySelector('.kt b')?.textContent,k.className]),strips:document.getElementById('lfk').className}));
 await p.screenshot({path:out+'/'+n+'.png',clip:{x:0,y:0,width:240,height:240}});
}
fs.writeFileSync(out+'/dump.json',JSON.stringify(dump,null,1));await b.close()})();
