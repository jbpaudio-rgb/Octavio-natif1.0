// Vactrol knob body (no ticks, no pointer) as RGBA, rendered by Chrome from the prototype's own kv() svg -> assets/knob.json
const {chromium}=require('playwright');const fs=require('fs');
const PROTO=process.env.PROTO;
(async()=>{const b=await chromium.launch();
const p0=await b.newPage({viewport:{width:1100,height:1000}});await p0.goto('file://'+PROTO);await p0.waitForTimeout(300);
let svg=await p0.evaluate(()=>kv(0,false));
svg=svg.replace(/<line x1="[0-9.]+" y1="[0-9.]+" x2="[0-9.]+" y2="[0-9.]+" stroke="#(f2efe6|3a3a3a)" stroke-width="2" stroke-linecap="round"\/>/g,'');
svg=svg.replace(/<g transform="rotate\([^"]*\)"><line [^>]*\/><\/g>/,'');
console.log('pointer left?',/stroke="#15181b"/.test(svg));
const S=43.2,cells=[[7.19,32.14],[123.59,32.14]];
const res=[];
const shots={};
for(const bg of ['#000','#fff']){const p=await b.newPage({viewport:{width:240,height:240}});
 let html='<body style="margin:0;background:'+bg+'">';
 cells.forEach(([x,y],i)=>{html+=`<div style="position:absolute;left:${x}px;top:${y}px;width:${S}px;height:${S}px">${svg.replace('<svg ',`<svg style="display:block;width:${S}px;height:${S}px" `)}</div>`});
 await p.setContent(html);await p.waitForTimeout(150);shots[bg]=await p.screenshot({type:'png'});await p.close()}
const p1=await b.newPage();
const dec=async(buf)=>p1.evaluate(async(b64)=>{const im=new Image();im.src='data:image/png;base64,'+b64;await im.decode();const c=document.createElement('canvas');c.width=im.width;c.height=im.height;const g=c.getContext('2d');g.drawImage(im,0,0);return Array.from(g.getImageData(0,0,c.width,c.height).data)},buf.toString('base64'));
const B=await dec(shots['#000']),Wh=await dec(shots['#fff']);
const out=[];
for(const [x,y] of cells){const x0=Math.floor(x),y0=Math.floor(y),N=44;const px=[];
 for(let yy=0;yy<N;yy++)for(let xx=0;xx<N;xx++){const k=((y0+yy)*240+(x0+xx))*4;
  const a=1-((Wh[k]-B[k])+(Wh[k+1]-B[k+1])+(Wh[k+2]-B[k+2]))/(3*255);
  px.push([B[k],B[k+1],B[k+2],Math.max(0,Math.min(255,Math.round(a*255)))])}
 out.push({x0,y0,n:N,px})}
fs.writeFileSync(__dirname+'/../assets/knob.json',JSON.stringify(out));console.log('ok');await b.close()})();
