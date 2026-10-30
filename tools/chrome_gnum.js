// The ENV graph's time label ("0.67 s"): canvas text '600 22px Helvetica,Arial,sans-serif' on the 330x120 canvas the prototype shows at 225.63 x 76.8 px.
// Rendered the same way (canvas scaled by CSS), 4 sub-pixel phases -> assets/gnum.json   usage: node tools/chrome_gnum.js
const {chromium}=require('playwright');const fs=require('fs');
const CH=['0','1','2','3','4','5','6','7','8','9','.',' ','s'];
const SX=225.63/330, PH=[0,.25,.5,.75];
(async()=>{const b=await chromium.launch();const p=await b.newPage({viewport:{width:600,height:300},deviceScaleFactor:1});
const out={chars:CH,adv:{},phases:[]};
for(const ph of PH){
 const glyphs={};
 for(const ch of CH){
  const r=await p.evaluate(async({ch,ph,SX})=>{
   document.body.style.cssText='margin:0;background:#000';document.body.innerHTML='<canvas id=c width=330 height=120 style="position:absolute;left:0;top:0;width:225.63px;height:76.8px"></canvas>';
   const c=document.getElementById('c'),g=c.getContext('2d');g.fillStyle='#fff';g.font='600 22px Helvetica,Arial,sans-serif';g.textAlign='left';
   const cx=(27+ph)/SX; g.fillText(ch,cx,60);
   const adv=g.measureText(ch).width;
   return {adv,cx};},{ch,ph,SX});
  out.adv[ch]=r.adv;
  const shot=await p.screenshot({type:'png',clip:{x:0,y:0,width:226,height:77}});
  const data=await p.evaluate(async(b64)=>{const im=new Image();im.src='data:image/png;base64,'+b64;await im.decode();const c=document.createElement('canvas');c.width=im.width;c.height=im.height;const g=c.getContext('2d');g.drawImage(im,0,0);return {w:im.width,h:im.height,d:Array.from(g.getImageData(0,0,c.width,c.height).data)}},shot.toString('base64'));
  const W=data.w;let x0=1e9,x1=-1,y0=1e9,y1=-1;
  for(let y=0;y<data.h;y++)for(let x=0;x<W;x++){if(data.d[(y*W+x)*4]>0){x0=Math.min(x0,x);x1=Math.max(x1,x);y0=Math.min(y0,y);y1=Math.max(y1,y)}}
  // origin = the pen position on screen: x = cx*SX, baseline y = 60*0.64
  const ox=27, base=Math.round(60*0.64);
  if(x1<0){glyphs[ch]={w:0,h:0,dx:0,dy:0,a:[]};continue}
  const a=[];for(let y=y0;y<=y1;y++)for(let x=x0;x<=x1;x++)a.push(data.d[(y*W+x)*4]);
  glyphs[ch]={w:x1-x0+1,h:y1-y0+1,dx:x0-ox,dy:y0-base,a};
 }
 out.phases.push(glyphs);
}
fs.writeFileSync(__dirname+'/../assets/gnum.json',JSON.stringify(out));console.log(JSON.stringify(out.adv));
await b.close()})();
