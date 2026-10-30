// Renders the prototype's glyphs with Chrome (same raster as the prototype) -> assets/fonts.json
// usage: node tools/chrome_fonts.js   (needs playwright)
const {chromium}=require('playwright');const fs=require('fs');
const STACK0='ui-monospace, Menlo, Consolas, monospace';
const FONTS=JSON.parse(fs.readFileSync(__dirname+'/fonts_spec.json','utf8'));
const SPECIAL=['\u25CF','\u00B7','\u2014','\u2026','\u25B6','\u25A0'];
const charset=(set)=>{const a=[];for(let c=32;c<127;c++){const ch=String.fromCharCode(c);if(set==='upper'&&ch>='a'&&ch<='z')continue;a.push(ch)}return a.concat(SPECIAL)};
const PH=[0,0.25,0.5,0.75];
(async()=>{const b=await chromium.launch();const out={};
for(const F of FONTS){
 const STACK=F.stack||STACK0;const CH=F.chars?F.chars.split(''):charset(F.set||'full');
 const p=await b.newPage({viewport:{width:48*16,height:64*Math.ceil(CH.length/16)+8},deviceScaleFactor:1});
 const cell=(i)=>({x:(i%16)*48,y:Math.floor(i/16)*64+4});
 const phases=[];
 for(const ph of PH){
  let html='<body style="margin:0;background:#000">';
  CH.forEach((ch,i)=>{const c=cell(i);
   html+=`<div style="position:absolute;left:${c.x+4+ph}px;top:${c.y}px;height:0;line-height:0;white-space:pre;font:${F.weight} ${F.size}px ${STACK};color:#fff"><span>${ch.replace('&','&amp;').replace('<','&lt;')}</span><span style="display:inline-block;width:1px;height:1px;background:#f00;margin-left:26px"></span></div>`});
  await p.setContent(html);await p.waitForTimeout(150);
  const shot=await p.screenshot({type:'png'});
  const data=await p.evaluate(async(b64)=>{const im=new Image();im.src='data:image/png;base64,'+b64;await im.decode();const c=document.createElement('canvas');c.width=im.width;c.height=im.height;const g=c.getContext('2d');g.drawImage(im,0,0);return Array.from(g.getImageData(0,0,c.width,c.height).data)},shot.toString('base64'));
  const W=48*16;const glyphs={};
  CH.forEach((ch,i)=>{const c=cell(i);
   let my=-1,mx=-1;
   for(let y=c.y;y<c.y+64;y++)for(let x=c.x;x<c.x+48;x++){const k=(y*W+x)*4;if(data[k]>200&&data[k+1]<60&&data[k+2]<60){my=y;mx=x}}
   const base=my+1,ox=c.x+4;
   const isM=(x,y)=>x>=mx-1&&x<=mx+1&&y>=my-1&&y<=my+1;
   const px=(x,y)=>{const k=(y*W+x)*4;return (data[k+1]&&Math.abs(data[k]-data[k+1])<4)?data[k]:0};
   let x0=1e9,x1=-1,y0=1e9,y1=-1;
   for(let y=c.y;y<c.y+64;y++)for(let x=c.x;x<c.x+48;x++){if(isM(x,y))continue;if(px(x,y)){x0=Math.min(x0,x);x1=Math.max(x1,x);y0=Math.min(y0,y);y1=Math.max(y1,y)}}
   if(x1<0){glyphs[ch]={w:0,h:0,dx:0,dy:0,a:[]};return}
   const a=[];for(let y=y0;y<=y1;y++)for(let x=x0;x<=x1;x++){const k=(y*W+x)*4;a.push(isM(x,y)?0:data[k])}
   glyphs[ch]={w:x1-x0+1,h:y1-y0+1,dx:x0-ox,dy:y0-base,a}});
  phases.push(glyphs)}
 const adv=await p.evaluate(({font})=>{const s=document.createElement('span');s.style.font=font;s.style.position='absolute';s.textContent='0000000000';document.body.append(s);return s.getBoundingClientRect().width/10},{font:`${F.weight} ${F.size}px ${STACK}`});
 out[F.name]={size:F.size,weight:F.weight,adv,chars:CH,phases};await p.close()}
fs.writeFileSync(__dirname+'/../assets/fonts.json',JSON.stringify(out));console.log(Object.keys(out).map(k=>k+' adv='+out[k].adv).join('\n'));
await b.close()})();
