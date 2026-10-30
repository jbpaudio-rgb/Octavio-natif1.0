// Glyphs of the drum sequencer's lane labels (DSEQ page, ddseq()): bold 16px 'Courier New', drawn on the 330x280 canvas that CSS shows at 225.625 x 187.19 px at (7.19, 35.73).
// Merged into assets/fonts2.json as 'dql' (the other entries are left as they are).   usage: NODE_PATH=... node tools/chrome_fonts3.js
const {chromium}=require('playwright');const fs=require('fs');
const SX=225.625/330, SY=187.1875/280, X0=7.1875, Y0=35.734, PH=[0,.25,.5,.75];
const F={name:'dql',px:16,weight:'bold',chars:'BDSCHOPKIRNTMLA ',yc:100};
(async()=>{const b=await chromium.launch();const p=await b.newPage({viewport:{width:400,height:300},deviceScaleFactor:1});
const font=`${F.weight} ${F.px}px 'Courier New',monospace`;
const phases=[];let adv=0;
const base=Math.round(Y0+F.yc*SY);
for(const ph of PH){
 const glyphs={};
 for(const ch of F.chars){
  const r=await p.evaluate(({ch,ph,font,yc,X0,Y0,SX,SY})=>{
   document.body.style.cssText='margin:0;background:#000';
   document.body.innerHTML='<canvas id=c width=330 height=280 style="position:absolute;left:'+X0+'px;top:'+Y0+'px;width:225.625px;height:187.1875px"></canvas>';
   const c=document.getElementById('c'),g=c.getContext('2d');g.fillStyle='#fff';g.font=font;g.textAlign='left';g.textBaseline='middle';
   const cx=(27+ph-X0)/SX;g.fillText(ch,cx,yc);return g.measureText(ch).width},{ch,ph,font,yc:F.yc,X0,Y0,SX,SY});
  adv=r;
  const shot=await p.screenshot({type:'png',clip:{x:0,y:0,width:240,height:240}});
  const data=await p.evaluate(async(b64)=>{const im=new Image();im.src='data:image/png;base64,'+b64;await im.decode();const c=document.createElement('canvas');c.width=im.width;c.height=im.height;const g=c.getContext('2d');g.drawImage(im,0,0);return {w:im.width,h:im.height,d:Array.from(g.getImageData(0,0,c.width,c.height).data)}},shot.toString('base64'));
  const W=data.w;let x0=1e9,x1=-1,y0=1e9,y1=-1;
  for(let y=0;y<data.h;y++)for(let x=0;x<W;x++)if(data.d[(y*W+x)*4]>0){x0=Math.min(x0,x);x1=Math.max(x1,x);y0=Math.min(y0,y);y1=Math.max(y1,y)}
  if(x1<0){glyphs[ch]={w:0,h:0,dx:0,dy:0,a:[]};continue}
  const a=[];for(let y=y0;y<=y1;y++)for(let x=x0;x<=x1;x++)a.push(data.d[(y*W+x)*4]);
  glyphs[ch]={w:x1-x0+1,h:y1-y0+1,dx:x0-27,dy:y0-base,a};
 }
 phases.push(glyphs)}
const f2=JSON.parse(fs.readFileSync(__dirname+'/../assets/fonts2.json'));
f2[F.name]={size:F.px*SX,weight:700,adv:adv*SX,chars:F.chars.split(''),phases,base:base,yc:F.yc};
console.log(F.name,'adv px',adv*SX,'base',base);
fs.writeFileSync(__dirname+'/../assets/fonts2.json',JSON.stringify(f2));await b.close()})();
