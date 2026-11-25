'use strict';
// MeowBox web config. Plain JS, no deps. API contract: ../API.md
// Strings live in lang/<code>.json (one file per language, same keys). {name} in a string is filled by t(key,{name:...}).
let L={};
const RB='https://de1.api.radio-browser.info/json/stations/search',GEO='https://geocoding-api.open-meteo.com/v1/search',MAXFW=5505024,SHOT='/api/screenshot';
const ls=(k,v)=>{try{if(v===undefined)return localStorage.getItem(k);localStorage.setItem(k,v)}catch(e){}};
let lang=ls('lang')||'vi';
const t=(k,v)=>{const x=L[k];if(x===undefined)return k;return v&&typeof x=='string'?x.replace(/\{(\w+)\}/g,(_,n)=>v[n]??''):x};
// file names come from window.mbP (index.html): content-hashed in the build, plain in src/
const loadLang=async l=>{const r=await fetch(mbP('l',l));if(!r.ok)throw Error(r.status);return r.json()};
const $=s=>document.querySelector(s),main=$('#main'),nav=$('#nav'),html=document.documentElement;
function h(tag,a,...c){const e=document.createElement(tag);
for(const x of c.flat(9))if(x!=null&&x!==false)e.append(x.nodeType?x:''+x);
if(a)for(const k in a){const v=a[k];if(v==null||v===false)continue;
if(k.startsWith('on'))e.addEventListener(k.slice(2),v);else if(k=='class')e.className=v;else if(k in e)e[k]=v;else e.setAttribute(k,v)}
return e}
// section = label column + content column (no cards)
const sec=(title,...c)=>h('section',{class:'sec'},title?h('h2',0,title):h('span'),h('div',{class:'b'},c));
const fld=(l,el,hint)=>h('label',{class:'fld'},h('span',0,l),el,hint&&h('small',0,hint));
const grp=(l,el,hint)=>h('div',{class:'fld'},l?h('span',0,l):null,el,hint&&h('small',0,hint));
const row=(...c)=>h('div',{class:'row'},c);
const btn=(l,fn,cls,title)=>h('button',{type:'button',class:cls||'',onclick:fn,title,'aria-label':title},l);
const pad=n=>(''+n).padStart(2,'0');
const opt=o=>Object.entries(t(o));
const kv=a=>h('dl',{class:'kv'},a.filter(Boolean).map(([k,v,x,cl])=>h('div',0,h('dt',0,k),h('dd',{class:'v'},v),x?h('dd',{class:cl||''},x):null)));
// stroke icons, 16x16 grid, currentColor
const IC={home:'M2 8l6-5 6 5M4 7v6h8V7',radio:'<rect x="2" y="5" width="12" height="9"/><path d="M5 2l7 3"/><circle cx="6" cy="9.5" r="2"/>',alarm:'<circle cx="8" cy="9" r="5"/><path d="M8 6v3l2 1M3 3l2 1M13 3l-2 1"/>',cat:'M3 3l2 3h6l2-3v9H3z',bell:'M4 11V7a4 4 0 0 1 8 0v4l1 1H3zM7 14h2',pc:'<rect x="2" y="3" width="12" height="8"/><path d="M6 14h4M8 11v3"/>',set:'<path d="M2 4h6M12 4h2M2 12h2M8 12h6"/><circle cx="10" cy="4" r="2"/><circle cx="6" cy="12" r="2"/>',play:'M5 3l8 5-8 5z',stop:'<rect x="4" y="4" width="8" height="8"/>',prev:'M4 3v10M13 3L6 8l7 5z',next:'M12 3v10M3 3l7 5-7 5z',more:'<g fill="currentColor" stroke="none"><circle cx="3" cy="8" r="1.4"/><circle cx="8" cy="8" r="1.4"/><circle cx="13" cy="8" r="1.4"/></g>',x:'M4 4l8 8M12 4l-8 8'};
const ic=(n,s=16)=>{const e=h('span',{class:'i','aria-hidden':'true'});e.innerHTML=`<svg viewBox="0 0 16 16" width="${s}" height="${s}" fill="none" stroke="currentColor" stroke-width="1.5">${IC[n][0]=='<'?IC[n]:'<path d="'+IC[n]+'"/>'}</svg>`;return e};
// bind an input to o[k]. a: attrs + {tag, opts:[[v,label]]}
function bind(o,k,a={}){const{tag='input',opts,...r}=a;
const e=h(tag,r,opts&&opts.map(([v,l])=>h('option',{value:v},l)));
const ck=r.type=='checkbox',num=typeof o[k]=='number';
if(ck)e.checked=!!o[k];else e.value=o[k]??'';
e.addEventListener(ck||tag=='select'?'change':'input',()=>{o[k]=ck?e.checked:num?+e.value:e.value});return e}
const sw=(o,k,l)=>h('label',{class:'swl'},bind(o,k,{type:'checkbox',class:'sw'}),h('span',0,l));
function rng(o,k,l){const v=h('b',0,o[k]);const e=bind(o,k,{type:'range',min:0,max:100});e.addEventListener('input',()=>v.textContent=e.value);return fld(l,row(h('div',{class:'grow'},e),v))}
const hours=()=>[...Array(24).keys()].map(i=>[i,pad(i)+':00']);

function toast(m,bad,act){const d=h('div',{class:bad?'bad':'',role:'status'},h('span',0,m),act&&btn(act[0],()=>{d.remove();act[1]()},'sm ghost'));$('#toast').append(d);setTimeout(()=>d.remove(),act?10000:bad?4500:2200)}
// modal: btns=[[label,value,class]], last one is submit. Resolves value, or null on dismiss.
function modal(title,body,btns){return new Promise(res=>{let bg;
const close=v=>{bg.remove();document.removeEventListener('keydown',esc);res(v)};
const esc=e=>{if(e.key=='Escape')close(null)};
const last=btns[btns.length-1];
const f=h('form',{class:'modal',role:'dialog','aria-modal':'true',onsubmit:e=>{e.preventDefault();close(last[1])}},h('h3',0,title),body,
h('div',{class:'row end',style:'margin-top:16px'},btns.map((b,i)=>h('button',{type:i==btns.length-1?'submit':'button',class:b[2]||'',onclick:i==btns.length-1?null:()=>close(b[1])},b[0]))));
bg=h('div',{class:'mbg',onclick:e=>{if(e.target==bg)close(null)}},f);
document.addEventListener('keydown',esc);document.body.append(bg);
const fi=f.querySelector('input:not([readonly]),select');(fi||f.querySelector('[type=submit]')).focus()})}
const ask=(title,msg,okL,danger)=>modal(title,h('p',0,msg),[[t('cancel'),false,'ghost'],[okL||t('ok'),true,danger?'danger':'']]);
// form modal: fs=[[key,label,attrs]] -> object or null
async function askForm(title,fs,okL){const o={};
const body=h('div',0,fs.map(([k,l,a={},hint])=>{o[k]=a.value??'';return fld(l,bind(o,k,a),hint)}));
return await modal(title,body,[[t('cancel'),0,'ghost'],[okL||t('save'),1]])?o:null}
// small popup menu (the "⋯" button). items=[[label,fn,cls,disabled]]
let popEl=null;
const outside=e=>{if(popEl&&!popEl.contains(e.target))closePop()};
function closePop(){if(popEl){popEl.remove();popEl=null;document.removeEventListener('click',outside,true)}}
function pop(a,items){closePop();const m=h('div',{class:'pop',role:'menu'},items.map(([l,fn,cls,dis])=>{const b=btn(l,()=>{closePop();fn()},cls);b.setAttribute('role','menuitem');b.disabled=!!dis;return b}));
document.body.append(m);popEl=m;const r=a.getBoundingClientRect(),w=m.offsetWidth,hh=m.offsetHeight;
m.style.left=Math.max(8,Math.min(r.right-w,innerWidth-w-8))+'px';m.style.top=(r.bottom+hh+8>innerHeight?Math.max(8,r.top-hh-4):r.bottom+4)+'px';
setTimeout(()=>document.addEventListener('click',outside,true));const f=m.querySelector('button:not(:disabled)');f&&f.focus()}
const moreBtn=(label,items)=>{const b=btn(ic('more',18),()=>pop(b,items()),'ic',t('more')+': '+label);b.setAttribute('aria-haspopup','menu');return b};
const mvItems=(arr,i,cb)=>[[t('mvUp'),()=>{[arr[i-1],arr[i]]=[arr[i],arr[i-1]];cb()},'',i==0],[t('mvDn'),()=>{[arr[i+1],arr[i]]=[arr[i],arr[i+1]];cb()},'',i==arr.length-1]];

let authed=false,loginShown=false;
async function api(p,o={}){const q={method:o.m||(o.b!==undefined?'POST':'GET'),headers:{},credentials:'same-origin'};
if(o.b!==undefined){q.body=JSON.stringify(o.b);q.headers['Content-Type']='application/json'}
if(o.tok)q.headers.Authorization='Bearer '+o.tok;
let r;try{r=await fetch(p,q)}catch(e){if(!o.quiet)toast(t('netErr'),1);throw e}
const j=await r.json().catch(()=>({}));
if(r.status==401){showLogin();throw Error('auth')}
if(!r.ok){if(!o.quiet)toast(t('err')+': '+(j.err||r.status),1);const e=Error(j.err||r.status);e.status=r.status;throw e}
return j}
const put=(p,b)=>api(p,{m:'PUT',b});
const radio=b=>api('/api/radio/play',{b});
async function copy(s){try{await navigator.clipboard.writeText(s)}catch(e){const x=h('textarea',{value:s});document.body.append(x);x.select();document.execCommand('copy');x.remove()}toast(t('copied'))}

// ---------- theme + self-hosted fonts ----------
let FT={};try{FT=JSON.parse(ls('mbF'))||{}}catch(e){}   // theme id -> font files (cached from /api/theme(s))
let devTh='swiss',THS=[],fmap=null,fcur=null;
// data-theme = speaker theme, unless this browser chose "always Swiss"
function setTheme(id,fonts){if(id)devTh=id;if(fonts){FT[devTh]=fonts;ls('mbF',JSON.stringify(FT))}
const u=ls('webT')=='swiss'?'swiss':devTh;html.dataset.theme=u;ls('mbT',u);loadFonts(FT[u]||[]);
const c=$('#thc'),href=mbP('t',u);if(c&&c.getAttribute('href')!=href)c.setAttribute('href',href)}
// @font-face for only this theme's files, described by /fonts/fonts.json {file:{family,weight,style,stretch}}. Missing -> CSS fallback stacks.
async function loadFonts(fs){const k=fs.join();if(k===fcur)return;fcur=k;
if(fs.length&&!fmap){try{const r=await fetch('/fonts/fonts.json');fmap=r.ok?await r.json():{}}catch(e){fmap={}}fmap=fmap.fonts||fmap;if(Array.isArray(fmap))fmap=Object.fromEntries(fmap.map(x=>[x.file,x]))}
if(k!==fcur)return;const q=s=>(''+s).replace(/[^\w %.-]/g,''),M=fmap||{};
// exact file name, else its weight/style variants ("chakra-petch-vi.woff2" -> "chakra-petch-400-vi.woff2", ...)
$('#ff').textContent=[...new Set(fs.flatMap(f=>M[f]?[f]:Object.keys(M).filter(x=>x.startsWith(f.replace(/(-vi)?\.woff2$/,'')+'-'))))].filter(f=>M[f].family).map(f=>{const v=fmap[f];return`@font-face{font-family:"${q(v.family)}";src:url("/fonts/${encodeURIComponent(f)}") format("woff2");font-weight:${q(v.weight||400)};font-style:${q(v.style||'normal')};font-stretch:${q(v.stretch||'100%')};font-display:swap}`}).join('')}
async function syncTheme(){try{const[d,l]=await Promise.all([api('/api/config/display',{quiet:1}),api('/api/themes',{quiet:1})]);
THS=l.themes||[];for(const x of THS)FT[x.id]=x.fonts||[];ls('mbF',JSON.stringify(FT));html.dataset.fx=d.fx||'full';setTheme(d.theme||l.current)}catch(e){}}
// a theme's variables, read from themes.css (for preview placeholders of themes that are not active)
const tvars=id=>{for(const s of document.styleSheets){let rs;try{rs=s.cssRules}catch(e){continue}for(const r of rs)if(r.selectorText&&r.selectorText.includes(`"${id}"`))return r.style}};

const fUp=s=>{const d=s/86400|0;return(d?d+t('d'):'')+pad(s%86400/3600|0)+':'+pad(s%3600/60|0)};
const fB=b=>b>=1048576?(b/1048576).toFixed(1)+' MB':Math.round(b/1024)+' KB';
// device time is always ICT (UTC+7)
const ict=s=>new Date((s+25200)*1e3),fHr=s=>{const d=ict(s);return pad(d.getUTCHours())+':'+pad(d.getUTCMinutes())};
const fT=s=>{const d=ict(s);return pad(d.getUTCDate())+'/'+pad(d.getUTCMonth()+1)+' '+fHr(s)};
const fMS=s=>(s>=3600?(s/3600|0)+':':'')+pad(s%3600/60|0)+':'+pad(s%60);
const sig=r=>t(r>-60?'good':r>-72?'fair':'weak');
const wxT=c=>{const m=t('wx');let k=0;for(const x of Object.keys(m))if(c>=+x)k=x;return m[k]};
const logo=s=>{const e=document.createElementNS('http://www.w3.org/2000/svg','svg');e.setAttribute('class','logo');e.setAttribute('aria-hidden','true');e.innerHTML='<use href="#cat"/>';return e};
let timers=[],onVis=null;const every=(fn,ms)=>timers.push(setInterval(fn,ms));
document.addEventListener('visibilitychange',()=>{if(!document.hidden&&onVis)onVis()});

// ---------- pages ----------
async function pOv(){
const[s0,cat,pc]=await Promise.all([api('/api/status'),api('/api/config/cat',{quiet:1}).catch(()=>null),api('/api/config/pc',{quiet:1}).catch(()=>null)]);
// live speaker screen: reload every 5 s while the tab is visible; placeholder if the device has no /api/screenshot
const ph=h('div',{class:'ph',hidden:true},t('shotNo')),shot=h('div',{class:'shot'},ph);let img=null;
const snap=()=>{if(document.hidden)return;const n=new Image();n.alt=t('shotAlt');n.width=592;n.height=480;
n.onload=()=>{if(img)img.replaceWith(n);else shot.prepend(n);img=n;ph.hidden=true};
n.onerror=()=>{if(img){img.remove();img=null}ph.hidden=false};n.src=SHOT+'?t='+Date.now()};
snap();every(snap,5000);onVis=snap;
const s=cat&&cat.stats||{},h1=h('h1',0,!cat?'MeowBox':s.away?t('away'):cat.name+' '+(t('mood')[s.mood]||''));
const lb=h('div',{class:'lbl'}),nm=h('div',{class:'stn'}),sub=h('div'),pb=btn('',null),sl=h('div',{class:'muted'});
const v={vol:0},vol=bind(v,'vol',{type:'range',min:0,max:100,'aria-label':t('vol')}),vb=h('b');
vol.addEventListener('input',()=>vb.textContent=vol.value);vol.onchange=()=>radio({cmd:'vol',vol:v.vol});
const kvb=h('div');let st;
pb.onclick=async()=>{const ps=(st.play||{}).st||'stop',r=await radio(ps=='stop'||ps=='err'?{cmd:'play'}:{cmd:'stop'});st.play=r.play;draw(st)};
const low=Object.entries(t('stats')).map(([k,l])=>[l,s[k]]).filter(x=>x[1]<30).sort((a,b)=>a[1]-b[1])[0];
const draw=x=>{st=x;const p=x.play||{st:'stop'},on=p.st=='play'||p.st=='buf';
lb.textContent=on?t('nowPlay'):t('nRadio');nm.textContent=p.name||'';nm.hidden=!p.name;
sub.textContent=p.st=='play'?p.title||'':t(p.st=='buf'?'buf':p.st=='err'?'perr':'stop_');sub.className=p.st=='play'?'':p.st=='err'?'err':'muted';
pb.replaceChildren(ic(on?'stop':'play'),on?t('stop'):t('play'));
sl.textContent=x.sleep?t('sleepLeft')+' '+fMS(x.sleep):'';
if(document.activeElement!=vol){vol.value=v.vol=x.vol;vb.textContent=x.vol}
kvb.replaceChildren(kv([[t('nWifi'),x.mode=='ap'?t('apMode'):x.ssid,x.mode=='ap'?null:sig(x.rssi)+', '+x.rssi+' dBm'],
cat&&[t('nCat'),(t('stages')[s.stage]||'')+', '+s.age+t('d').trimEnd(),low?low[0]+' '+t('low')+', '+low[1]:t('moods')[s.mood],low?'err':''],
pc&&[t('nPc'),pc.st&&pc.st.on?pc.st.name:t('pcOff'),pc.st&&pc.st.on?pc.st.ip:null],
[t('time'),fT(x.time),'ICT (UTC+7)'],[t('ip'),x.ip,location.hostname]]))};
draw(s0);every(async()=>{try{draw(await api('/api/status',{quiet:1}))}catch(e){}},3000);
const np=h('div',{class:'np'},lb,nm,sub,h('div',{class:'row'},pb,btn(ic('prev'),()=>radio({cmd:'prev'}).then(r=>{st.play=r.play;draw(st)}),'ic bd',t('prev')),btn(ic('next'),()=>radio({cmd:'next'}).then(r=>{st.play=r.play;draw(st)}),'ic bd',t('next'))),
h('label',{class:'vol'},t('vol'),vol,vb),sl);
let log=null;try{const r=await api('/api/notify/history',{quiet:1});if(r.items.length)log=sec(t('recent'),h('ol',{class:'log'},r.items.slice(0,4).map(n=>h('li',{class:n.priority=='high'?'hi':''},h('time',0,fHr(n.ts)),h('span',0,(n.title?n.title+': ':'')+n.text)))),h('a',{href:'#/notify',class:'lk'},t('seeAll')))}catch(e){}
return[h('section',{class:'hero','aria-label':t('shotAlt')},shot,h('div',{class:'txt'},h1,h('p',0,t('shotHint')),np)),h('div',{class:'brush',role:'presentation'}),
await batSec(s0),sec(t('stT'),kvb),log]}
// ---------- battery ----------
const fDur=m=>(m>=60?t('durH',{n:Math.floor(m/60)})+' ':'')+t('durM',{n:m%60});
const B=k=>t('bat')[k];
const fHM=m=>Math.floor(m/60)+'h'+pad(m%60);
// chart: full width, 0/50/100 % guides, time axis (ICT)
function batChart(pts){if(!pts||pts.length<2)return null;const t0=pts[0][0],dt=Math.max(1,pts[pts.length-1][0]-t0),X=ts=>(ts-t0)/dt*100,Y=p=>100-p,ns='vector-effect="non-scaling-stroke"';
const pl=h('div',{class:'pl'}),z=pts[pts.length-1];
pl.innerHTML=`<svg viewBox="0 0 100 100" preserveAspectRatio="none" role="img" aria-label="${B('title')} ${pts[0][1]}% ${fHr(t0)}, ${z[1]}% ${fHr(z[0])}"><line class="g" x1="0" x2="100" y1="0" y2="0" ${ns}/><line class="g" x1="0" x2="100" y1="50" y2="50" ${ns}/><line class="g0" x1="0" x2="100" y1="100" y2="100" ${ns}/><path class="ln" d="${pts.map((p,i)=>(i?'L':'M')+X(p[0]).toFixed(2)+' '+Y(p[1]).toFixed(2)).join('')}" ${ns}/></svg>`;
pl.append(h('span',{class:'pt',style:`left:${X(z[0])}%;top:${Y(z[1])}%`}));
return h('div',{class:'chart'},h('div',{class:'ya','aria-hidden':'true'},['100%','50%','0%'].map(x=>h('span',0,x))),pl,h('div',{class:'xa'},[0,.25,.5,.75,1].map(f=>h('span',0,fHr(t0+dt*f)))))}
async function batSec(s){let b=null;try{b=await api('/api/battery',{quiet:1})}catch(e){}
const head=(pct,l1,l2)=>h('div',{class:'batt'},h('div',{class:'big'},pct<0?'—':pct+'%'),h('div',{class:'lines'},h('span',0,l1),l2?h('span',{class:'muted'},l2):null));
if(!b)return sec(B('title'),head(s.bat,s.chg?B('chg'):t('onBat')));
const l2=[b.mv>0?(b.mv/1000).toFixed(2)+' V':'',b.cur&&b.cur.open?B('ran')+' '+fDur(b.cur.min):'',b.rate>0?B('rate')+' '+b.rate+'%/h':''].filter(Boolean).join(', ');
const ss=b.sessions||[];
return sec(B('title'),head(b.pct,b.chg?B('chg'):b.eta>0?t('batEta',{d:fDur(b.eta)}):B('est'),l2),batChart(b.pts),h('h3',0,B('sess')),
ss.length?h('div',{class:'tw'},h('table',0,h('thead',0,h('tr',0,B('cols').map(c=>h('th',{scope:'col'},c)))),h('tbody',0,ss.map(x=>h('tr',0,h('td',0,fT(x.start)),h('td',0,fHM(x.min)),h('td',0,x.from+'–'+x.to),h('td',0,B('end')[x.end]||x.end),h('td',0,x.full_h?x.full_h+' h':'—')))))):h('p',{class:'muted'},B('none')),
h('a',{href:'/api/battery/log.csv',class:'lk'},B('csv')))}

async function pWifi(){
const c=await api('/api/config/wifi'),sl=h('ul',{class:'list'}),scl=h('ul',{class:'list'});
const save=async()=>{Object.assign(c,await put('/api/config/wifi',{nets:c.nets.map(n=>({ssid:n.ssid}))}));draw();toast(t('saved'))};
const draw=()=>sl.replaceChildren(...(c.nets.length?c.nets.map((n,i)=>h('li',0,h('span',{class:'num'},i+1),h('div',{class:'main'},h('b',0,n.ssid),n.ssid==c.cur?h('small',{class:'okc'},t('connected')):null),
moreBtn(n.ssid,()=>[...mvItems(c.nets,i,save),[t('del'),async()=>{if(await ask(t('delNet'),n.ssid,t('del'),1)){c.nets.splice(i,1);save()}},'dg']]))):[h('li',{class:'muted'},t('none'))]));
const conn=async(ssid,sec_)=>{const known=c.nets.some(n=>n.ssid==ssid);
const fs=[['pass',t('pass'),{type:'password',autocomplete:'off'},known?t('keepPw'):null]];if(!ssid)fs.unshift(['ssid',t('ssid'),{}]);
const r=sec_||!ssid?await askForm(ssid||t('hidden'),fs,t('connect')):{pass:''};if(!r)return;
const b={ssid:ssid||r.ssid};if(r.pass||!known)b.pass=r.pass;if(!b.ssid)return;
await api('/api/wifi/connect',{b});toast(t('connecting'))};
const sb=btn(t('scan'),scan,'sm');
async function scan(){sb.disabled=true;scl.replaceChildren(h('li',{class:'muted'},t('scanning')));
try{const r=await api('/api/wifi/scan');scl.replaceChildren(...r.aps.map(a=>h('li',0,h('div',{class:'main'},h('b',0,a.ssid),h('small',0,a.rssi+' dBm, '+sig(a.rssi)+', '+t('ch')+' '+a.ch+(a.sec?', '+t('secured'):''))),btn(t('connect'),()=>conn(a.ssid,a.sec),'sm ghost'))))}
catch(e){scl.replaceChildren()}finally{sb.disabled=false}}
const ps=h('b'),pb=btn('',null,'sm ghost');
const pdraw=p=>{ps.textContent=p.on?t('apOn')+', '+p.ssid+(p.off_in=='client'?', '+t('apClient'):typeof p.off_in=='number'?', '+t('apOffIn')+' '+Math.max(1,Math.ceil(p.off_in/60))+' '+t('apMin'):''):t('apOff');
pb.textContent=p.on?t('apStop'):t('apStart');pb.onclick=async()=>{pb.disabled=true;try{pdraw(await api('/api/wifi/portal',{b:{on:!p.on}}))}catch(e){}finally{pb.disabled=false}}};
api('/api/wifi/portal').then(pdraw).catch(()=>{});
draw();scan();
return[sec(t('savedNets'),h('p',{class:'muted'},t('prio')),sl),sec(t('scanT'),row(sb,btn(t('hidden'),()=>conn('',1),'sm ghost')),scl),
sec(t('apT'),h('p',{class:'muted'},t('apHelp')),h('ul',{class:'list'},h('li',0,h('div',{class:'main'},ps),pb)))]}

const stTypes=[['stream','Stream (MP3/AAC)'],['hls','HLS (.m3u8)'],['youtube','YouTube']],TY={stream:'Stream',hls:'HLS',youtube:'YouTube'};
async function pRadio(){
const[c,st0,sl0]=await Promise.all([api('/api/config/radio'),api('/api/status').catch(()=>({})),api('/api/sleep').catch(()=>({left:0}))]);let st=st0;
const ul=h('ol',{class:'list'}),cnt=h('span',{class:'muted'}),right=h('section',{class:'rc','aria-label':t('stations')});
const save=async()=>{Object.assign(c,await put('/api/config/radio',{st:c.st}));toast(t('saved'))};
const has=u=>c.st.some(s=>s.url==u);
const addSt=async s=>{if(c.st.length>=(c.max||50))return toast(t('full'),1);s.id=c.st.reduce((m,x)=>Math.max(m,x.id),0)+1;s.name=s.name.slice(0,48);c.st.push(s);await save();draw()};
const edit=async s=>{const r=await askForm(s?t('edit'):t('addSt'),[['name',t('name'),{value:s?s.name:'',required:true,maxLength:48}],['url',t('url'),{value:s?s.url:'',required:true,type:'url'}],['type',t('type'),{tag:'select',opts:stTypes,value:s?s.type:'stream'}]]);
if(!r)return;if(s){Object.assign(s,r);await save();draw()}else await addSt(r);show('list')};
const isNow=s=>st.play&&st.play.id==s.id&&st.play.st!='stop';
// stream numbers under the playing station + a hint when something is off (docs/firmware_v0.2.0.md, 4.3)
const netTxt=()=>{const p=st.play||{},n=p.net;if(!n)return['',''];
const line=p.st=='buf'?t('netBuf',{fill:n.fill}):t('netLine',{kbps:n.kbps,br:n.br||'?',buf:Math.round(n.buf),under:n.under});
const hint=st.rssi&&st.rssi<-75?t('hWeak'):n.br&&n.kbps<n.br*0.9&&(n.under>0||p.st=='buf')?t('hSlow'):(p.st!='play'&&n.err&&/^(connect|http)/.test(n.err))?t('hConn'):'';
return[line,hint]};
const netUp=()=>{const[l,hn]=netTxt(),a=ul.querySelector('.netl'),b=ul.querySelector('.neth');if(a)a.textContent=l;if(b){b.textContent=hn;b.hidden=!hn}};
const play=async s=>{st.play=(await radio(isNow(s)?{cmd:'stop'}:{cmd:'play',id:s.id})).play;draw()};
const draw=()=>{cnt.textContent=c.st.length+' '+t('of')+' '+(c.max||50);ul.replaceChildren(...(c.st.length?c.st.map((s,i)=>{const now=isNow(s);
return h('li',{class:now?'now':''},h('span',{class:'num'},i+1),h('div',{class:'main'},h('b',0,s.name),now&&st.play.net?h('small',{class:'netl'},netTxt()[0]):h('small',0,s.url.replace(/^https?:\/\/(www\.)?/,'')),now?h('small',{class:'neth err',hidden:!netTxt()[1]},netTxt()[1]):null),
h('span',{class:'tag'},now?t('nowPlay'):TY[s.type]||s.type),btn(ic(now?'stop':'play'),()=>play(s),'ic bd',(now?t('stop'):t('play'))+': '+s.name),
moreBtn(s.name,()=>[...mvItems(c.st,i,()=>{draw();save()}),[t('edit'),()=>edit(s)],[t('del'),async()=>{if(await ask(t('delSt'),s.name,t('del'),1)){c.st.splice(i,1);draw();save()}},'dg']]))}):[h('li',{class:'muted'},t('none'))]))};
const listV=[h('div',{class:'lh'},h('h2',0,t('stations')),cnt),ul,h('div',0,btn(t('findRB'),()=>show('find'),'lk'),btn(t('addUrl'),()=>edit(),'lk'))];
const show=k=>right.replaceChildren(...(k=='find'?tFind():listV));
function tFind(){const f={cc:'VN',name:'',tag:''},res=h('ul',{class:'list'});
let dn;try{dn=new Intl.DisplayNames([lang],{type:'region'})}catch(e){}
const cc=['VN','US','GB','JP','KR','FR','DE','TH',''].map(x=>[x,x?(dn?dn.of(x):x):t('all')]);
const go=async e=>{e.preventDefault();res.replaceChildren(h('li',{class:'muted'},t('loading')));
const q=new URLSearchParams({limit:40,hidebroken:'true',order:'clickcount',reverse:'true'});
if(f.cc)q.set('countrycode',f.cc);if(f.name)q.set('name',f.name);if(f.tag)q.set('tag',f.tag.trim().toLowerCase());
let j;try{j=await(await fetch(RB+'?'+q)).json()}catch(x){return res.replaceChildren(h('li',{class:'err'},t('extErr')))}
res.replaceChildren(...(j.length?j.map(s=>{const u=s.url_resolved||s.url,ty=s.hls==1||/\.m3u8/i.test(u)?'hls':'stream';
const ab=btn(has(u)?t('added'):t('add'),async()=>{await addSt({name:s.name.trim(),url:u,type:ty});ab.textContent=t('added');ab.disabled=true},'sm ghost');ab.disabled=has(u);
return h('li',0,h('div',{class:'main'},h('b',0,s.name),h('small',0,[s.codec+(s.bitrate?' '+s.bitrate+'k':''),s.countrycode,s.tags.split(',').slice(0,3).join(', ')].filter(Boolean).join(', '))),
btn(ic('play'),()=>radio({cmd:'play',url:u,name:s.name.slice(0,48)}),'ic bd',t('preview')+': '+s.name),ab)}):[h('li',{class:'muted'},t('noRes'))]))};
return[h('div',{class:'lh'},h('h2',0,t('tab_find')),btn(t('stations'),()=>show('list'),'lk')),h('form',{onsubmit:go,style:'margin-top:16px'},h('div',{class:'two'},fld(t('country'),bind(f,'cc',{tag:'select',opts:cc})),fld(t('tag'),bind(f,'tag',{placeholder:t('tagPh')}))),
fld(t('name'),bind(f,'name',{type:'search'})),h('button',0,t('search'))),h('p',{class:'muted',style:'margin-top:8px'},'radio-browser.info'),res]}
// left column: YouTube, volume, sleep timer
const y={url:'',name:''},pcl=h('small',{class:'muted'});
api('/api/config/pc',{quiet:1}).then(p=>{pcl.textContent=t('ytHint')+' '+t('nPc')+': '+(p.st&&p.st.on?t('pcOn'):t('pcOff'))+'.'}).catch(()=>{});
const yt=h('form',{onsubmit:async e=>{e.preventDefault();
if(!/^(https?:\/\/)?((www|m|music)\.)?(youtube\.com\/(watch|playlist|live|shorts|@)|youtu\.be\/)/i.test(y.url.trim()))return toast(t('ytBad'),1);
let u=y.url.trim();if(!/^https?:/i.test(u))u='https://'+u;await addSt({name:y.name.trim()||'YouTube',url:u,type:'youtube'});e.target.reset();y.url=y.name=''}},
fld(t('ytAdd'),bind(y,'url',{type:'url',placeholder:'https://www.youtube.com/@LofiGirl/live',required:true})),fld(t('ytName'),bind(y,'name',{maxLength:48,placeholder:'Lofi Girl'})),pcl,h('button',{style:'align-self:flex-start'},t('ytSave')));
const v={vol:c.vol},vol=bind(v,'vol',{type:'range',min:0,max:100}),vb=h('b',0,c.vol);vol.addEventListener('input',()=>vb.textContent=vol.value);vol.onchange=()=>radio({cmd:'vol',vol:v.vol});
let left=sl0.left,picked=0;const lt=h('span',{class:'muted'}),chips=h('div',{class:'chips'});
const drawS=()=>{lt.textContent=left>0?t('left')+' '+fMS(left):t('off');chips.replaceChildren(...[15,30,60,90].map(m=>btn(m+"'",()=>setS(m),left>0&&picked==m?'on':'ghost',m+' '+t('min'))),btn(t('cancel'),()=>setS(0),'ghost'))};
const setS=async m=>{left=(await api('/api/sleep',{b:{min:m}})).left;picked=m;drawS();toast(m?t('sleepSet')+' '+m+' '+t('min'):t('off'))};
every(()=>{if(left>0){left--;lt.textContent=t('left')+' '+fMS(left);if(!left)drawS()}},1000);drawS();
const cu={m:45};
draw();show('list');
let lastNow='';every(async()=>{try{st=await api('/api/status',{quiet:1})}catch(e){return}
const k=(st.play&&st.play.st!='stop'?st.play.id:'')+'/'+!!(st.play&&st.play.net);if(k!=lastNow){lastNow=k;draw()}else netUp()},3000);
return h('div',{class:'split'},h('div',{class:'side'},h('h1',0,t('nRadio')),h('p',{class:'muted'},t('radioHint')),yt,
h('div',0,h('label',{class:'vol'},h('b',{style:'width:auto'},t('vol')),vol,vb)),
h('div',0,h('b',0,t('nSleep')),chips,lt,h('form',{class:'row',onsubmit:e=>{e.preventDefault();setS(cu.m)}},h('div',{class:'grow'},bind(cu,'m',{type:'number',min:1,max:240,'aria-label':t('custom')})),h('button',{class:'ghost'},t('on'))))),right)}

async function pAlarm(){
const[c,r]=await Promise.all([api('/api/config/alarms'),api('/api/config/radio')]);const box=h('div');
const sounds=[['chime',t('chime')],...r.st.map(s=>['st'+s.id,'Radio: '+s.name])];
const addB=btn(t('addAlarm'),()=>{c.al.push({on:true,h:7,m:0,days:31,snd:'chime',st:0});draw()},'ghost');
const one=(a,i)=>{const tm=h('input',{type:'time',value:pad(a.h)+':'+pad(a.m),required:true,'aria-label':t('nAlarm')+' '+(i+1)});
tm.oninput=()=>{const[x,y]=tm.value.split(':');if(y){a.h=+x;a.m=+y}};
const s={v:a.snd=='radio'&&r.st.some(x=>x.id==a.st)?'st'+a.st:'chime'},se=bind(s,'v',{tag:'select',opts:sounds});
se.addEventListener('change',()=>{if(s.v=='chime'){a.snd='chime';a.st=0}else{a.snd='radio';a.st=+s.v.slice(2)}});
const days=h('div',{class:'days'},t('days').map((d,b)=>{const e=btn(d,()=>{a.days^=1<<b;e.classList.toggle('on')},a.days>>b&1?'on':'');return e}));
return h('div',{class:'al'},row(bind(a,'on',{type:'checkbox',class:'sw','aria-label':t('on')}),tm,h('span',{class:'sp'}),btn(ic('x',18),()=>{c.al.splice(i,1);draw()},'ic bd',t('del'))),
grp('',days),fld(t('sound'),se))};
const draw=()=>{addB.disabled=c.al.length>=(c.max||5);box.replaceChildren(...c.al.map(one))};draw();
return[sec(t('maxAl'),h('p',{class:'muted'},t('once')),box,h('div',{class:'row',style:'margin-top:16px'},addB)),sec(t('opts'),h('div',{class:'lim'},fld(t('snooze'),bind(c,'snooze',{type:'number',min:1,max:30})),
btn(t('save'),async()=>{Object.assign(c,await put('/api/config/alarms',{snooze:c.snooze,al:c.al}));draw();toast(t('saved'))})))]}

async function pWx(){const c=await api('/api/config/weather'),cur=h('div'),res=h('ul',{class:'list'}),f={q:''};
const drawC=()=>{const n=c.now;cur.replaceChildren(...[h('div',{class:'stn'},c.name||t('none')),c.lat!=null?h('p',{class:'muted'},c.lat.toFixed(3)+', '+c.lon.toFixed(3)):null,
n?h('p',0,wxT(n.code)+', '+n.t+'°, '+t('hilo')+' '+n.hi+'°/'+n.lo+'°'):null].filter(Boolean))};
const save=async()=>{Object.assign(c,await put('/api/config/weather',{on:c.on,name:c.name,lat:c.lat,lon:c.lon,unit:c.unit}));drawC();toast(t('saved'))};
const find=async e=>{e.preventDefault();const q=f.q.trim();if(!q)return;res.replaceChildren(h('li',{class:'muted'},t('loading')));
let j;try{j=await(await fetch(GEO+'?'+new URLSearchParams({name:q,count:8,language:lang,format:'json'}))).json()}catch(x){return res.replaceChildren(h('li',{class:'err'},t('extErr')))}
const rs=j.results||[];res.replaceChildren(...(rs.length?rs.map(x=>h('li',0,h('div',{class:'main'},h('b',0,x.name),h('small',0,[x.admin1,x.country,x.latitude.toFixed(2)+', '+x.longitude.toFixed(2)].filter(Boolean).join(', '))),
btn(t('choose'),async()=>{Object.assign(c,{name:x.name.replace(/Ð/g,'Đ').replace(/ð/g,'đ').slice(0,48),lat:+x.latitude.toFixed(4),lon:+x.longitude.toFixed(4)});await save();res.replaceChildren()},'sm ghost'))):[h('li',{class:'muted'},t('noRes'))]))};
drawC();
return[sec(t('curLoc'),cur),sec(t('findCity'),h('form',{class:'row',onsubmit:find},h('div',{class:'grow'},bind(f,'q',{type:'search',placeholder:t('cityPh'),'aria-label':t('findCity')})),h('button',0,t('search'))),res,h('p',{class:'muted',style:'margin-top:8px'},'open-meteo.com')),
sec(t('opts'),h('div',{class:'lim'},sw(c,'on',t('wxOn')),fld(t('unit'),bind(c,'unit',{tag:'select',opts:[['c','°C'],['f','°F']]})),btn(t('save'),save)))]}

// Settings > Appearance: theme cards (apply at once + undo), screen, night mode, effects, web-only option
async function pLook(){const[c,l]=await Promise.all([api('/api/config/display'),api('/api/themes')]);
THS=l.themes||[];c.theme=c.theme||l.current||'swiss';c.night_mode=c.night_mode||(c.night===false?'off':'auto');c.fx=c.fx||'full';
const grid=h('div',{class:'themes',role:'radiogroup','aria-label':t('theme')}),bad=new Set(),nm=x=>x.name[lang]||x.name.vi;
const ph=id=>{const e=h('div',{class:'pv ph','aria-hidden':'true'},h('b',0,'07:30'),h('i'),h('s')),st=tvars(id);
if(st)for(const k of['--bg','--surface','--text','--dim','--line','--accent','--accent2','--font-display','--display-stretch'])e.style.setProperty(k,st.getPropertyValue(k));return e};
const fxSec=h('div',{'data-audit':'theme'}); // shown only for themes with effects (audit.mjs skips it)
const draw=()=>{const cur=THS.find(x=>x.id==c.theme);fxSec.hidden=!!cur&&cur.fx===false;
grid.replaceChildren(...THS.map(x=>{const on=x.id==c.theme;let pv;
if(bad.has(x.id))pv=ph(x.id);else{pv=h('img',{class:'pv',src:x.preview,alt:'',loading:'lazy'});pv.onerror=()=>{bad.add(x.id);pv.replaceWith(ph(x.id))}}
return h('label',{class:'tc'+(on?' on':'')},h('input',{type:'radio',name:'th',value:x.id,checked:on,class:'vh',onchange:()=>pick(x.id)}),pv,
h('span',{class:'nm'},nm(x),x.default?h('em',0,t('defT')):null),h('small',0,x.desc[lang]||x.desc.vi))}))};
const pick=async(id,undo)=>{const prev=c.theme;if(id==prev)return;
try{Object.assign(c,await put('/api/config/display',{theme:id}))}catch(e){draw();return}
setTheme(c.theme,(THS.find(x=>x.id==c.theme)||{}).fonts);draw();
const x=THS.find(y=>y.id==c.theme);if(!undo)toast(t('themeOk')+(x?nm(x):c.theme),0,[t('undo'),()=>pick(prev,1)])};
const save=async()=>{const{tz,night,...b}=c;Object.assign(c,await put('/api/config/display',b));html.dataset.fx=c.fx;toast(t('saved'))};
const fx=bind(c,'fx',{tag:'select',opts:opt('fxs')});fx.addEventListener('change',save);
const w={v:ls('webT')=='swiss'?'swiss':'device'},ws=bind(w,'v',{tag:'select',opts:opt('webs')});ws.addEventListener('change',()=>{ls('webT',w.v);setTheme()});
fxSec.append(fld(t('fx'),fx,t('fxHint')));
draw();
return[sec(t('theme'),grid,h('a',{class:'btn ghost',href:SHOT,target:'_blank',rel:'noopener'},t('shot'))),
sec(t('screen'),h('div',{class:'lim'},rng(c,'bri',t('bri')),rng(c,'idle_bri',t('idleBri')),fld(t('idleS'),bind(c,'idle_s',{type:'number',min:0,max:3600})),fld(t('sleepS'),bind(c,'sleep_s',{type:'number',min:0,max:3600}),t('sleepHint')),sw(c,'h24',t('h24')),
fld(t('devLang'),bind(c,'lang',{tag:'select',opts:[['vi','Tiếng Việt'],['en','English']]})),fld(t('tz'),h('input',{value:t('tzV'),readOnly:true})),btn(t('save'),save))),
sec(t('night'),h('div',{class:'lim'},fld(t('night'),bind(c,'night_mode',{tag:'select',opts:opt('nm')})),h('div',{class:'two'},fld(t('from'),bind(c,'n_from',{tag:'select',opts:hours()})),fld(t('to'),bind(c,'n_to',{tag:'select',opts:hours()}))),
rng(c,'n_bri',t('nBri')),btn(t('save'),save))),
sec(t('fx'),h('div',{class:'lim'},fxSec,fld(t('web'),ws,t('webHint'))))]}

const FUR={tabby:'repeating-linear-gradient(135deg,#8a8070 0 5px,#4e463c 5px 9px)',orange:'repeating-linear-gradient(135deg,#f0a04b 0 5px,#c8742a 5px 9px)',black:'#222',white:'#f7f4ee',calico:'conic-gradient(#f7f4ee 0 35%,#e08a3c 0 68%,#2a2a2a 0)',gray:'#8e9198'};
async function pCat(){let c=await api('/api/config/cat');const sbox=h('div'),fb=h('div',{class:'furs'});
const drawS=()=>{const s=c.stats||{};sbox.replaceChildren(...[h('div',{class:'stn'},c.name),h('p',{class:'muted'},[t('stages')[s.stage],t('age')+' '+s.age+t('d').trimEnd(),t('moods')[s.mood]].filter(Boolean).join(', ')),
s.away?h('p',{class:'err'},t('away')):null,...Object.entries(t('stats')).map(([k,l])=>h('div',{class:'stat'},h('span',0,l),h('div',{class:'bar'+((s[k]|0)<30?' low':'')},h('div',{style:'width:'+(s[k]|0)+'%'})),h('small',0,s[k]|0)))].filter(Boolean))};
const drawF=()=>fb.replaceChildren(...Object.entries(t('furs')).map(([k,l])=>h('label',{class:c.fur==k?'on':''},h('input',{type:'radio',name:'fur',value:k,checked:c.fur==k,onchange:()=>{c.fur=k;drawF()}}),h('span',{class:'swatch',style:'background:'+FUR[k]}),l)));
drawS();drawF();
return[sec(t('catSt'),h('div',{class:'lim'},sbox)),sec(t('opts'),h('div',{class:'lim'},fld(t('catName'),bind(c,'name',{maxLength:16})),grp(t('fur'),fb),fld(t('diff'),bind(c,'diff',{tag:'select',opts:opt('diffs')})),
btn(t('save'),async()=>{c=Object.assign(c,await put('/api/config/cat',{name:c.name,fur:c.fur,diff:c.diff}));drawS();toast(t('saved'))}))),
sec(t('danger'),btn(t('reset'),async()=>{if(await ask(t('reset'),t('resetQ'),t('reset'),1)){Object.assign(c,await api('/api/cat/reset',{b:{}}));drawS();toast(t('saved'))}},'danger'))]}

async function pNotify(){const c=await api('/api/config/notify'),hl=h('ol',{class:'log'}),ex=h('pre'),tk={v:c.token};
const ti=bind(tk,'v',{type:'password',readOnly:true,style:'font-family:var(--font-data)','aria-label':t('token')});
const drawEx=()=>{const H=location.host||'meowbox-xxxx.local',T=c.token;tk.v=ti.value=T;ex.textContent=
`curl -X POST http://${H}/api/notify -H "Authorization: Bearer ${T}" -H "Content-Type: application/json" -d '{"title":"Build xong","text":"esp32 OK","sound":"chime","priority":"normal","duration":10}'\n\ncurl "http://${H}/api/notify?token=${T}&text=Xin%20chao"\n\ncurl -X POST http://${H}/api/notify/audio -H "Authorization: Bearer ${T}" -H "Content-Type: audio/mpeg" --data-binary @tieng.mp3\n\n# PowerShell\nInvoke-RestMethod -Method Post -Uri http://${H}/api/notify -Headers @{Authorization="Bearer ${T}"} -ContentType "application/json; charset=utf-8" -Body '{"text":"Xong"}'`};
const loadH=async()=>{const r=await api('/api/notify/history',{tok:c.token});hl.replaceChildren(...(r.items.length?r.items.map(n=>h('li',{class:n.priority=='high'?'hi':''},h('time',0,fHr(n.ts)),h('span',0,h('b',0,(n.title?n.title+': ':'')+n.text),h('br'),h('small',{class:'muted'},[fT(n.ts),t('pr')[n.priority]||n.priority,t('snd')[n.sound]||n.sound,n.src].join(', '))))):[h('li',{class:'muted'},t('none'))]))};
const m={title:t('testT'),text:t('testX'),sound:'chime',priority:'normal',duration:c.dur};
drawEx();loadH();
return[sec(t('token'),h('div',{class:'lim'},row(h('div',{class:'grow'},ti),btn(t('show'),()=>ti.type=ti.type=='password'?'text':'password','sm ghost'),btn(t('copy'),()=>copy(c.token),'sm ghost')),
h('p',{style:'margin-top:12px'},btn(t('regen'),async()=>{if(await ask(t('regen'),t('regenQ'),t('regen'),1)){Object.assign(c,await put('/api/config/notify',{regen:true}));drawEx();toast(t('saved'))}},'sm danger')))),
sec(t('test'),h('form',{class:'lim',onsubmit:async e=>{e.preventDefault();await api('/api/notify',{b:m,tok:c.token});toast(t('sent'));setTimeout(loadH,500)}},
fld(t('title'),bind(m,'title',{maxLength:48})),fld(t('text'),bind(m,'text',{required:true,maxLength:200})),
h('div',{class:'two'},fld(t('sound'),bind(m,'sound',{tag:'select',opts:opt('snd')})),fld(t('prio_'),bind(m,'priority',{tag:'select',opts:opt('pr')}))),
fld(t('dur'),bind(m,'duration',{type:'number',min:1,max:300})),h('button',0,t('test')))),
sec(t('nset'),h('div',{class:'lim'},fld(t('dur'),bind(c,'dur',{type:'number',min:1,max:300})),rng(c,'vol',t('vol')),sw(c,'quiet_low',t('quiet')),fld(t('ntfy'),bind(c,'ntfy',{placeholder:'meowbox-…'})),
btn(t('save'),async()=>{Object.assign(c,await put('/api/config/notify',{dur:c.dur,vol:c.vol,quiet_low:c.quiet_low,ntfy:c.ntfy}));toast(t('saved'))}))),
sec(t('hist'),hl,btn(t('refresh'),loadH,'lk')),sec(t('ex'),ex)]}

async function pPc(){const c=await api('/api/config/pc'),g=h('div');
const rl=h('p',{class:'muted'}),rb=btn(t('relayTest'),async()=>{rb.disabled=true;rl.className='muted';rl.textContent=t('relayTesting');
try{const r=await api('/api/pc/relay_test',{b:{},quiet:1});rl.className=r.ok?'okc':'err';rl.textContent=r.ok?t('relayOk',{ms:r.ms}):t('relayFail',{e:t('relayE')[r.err]||r.err})}
catch(e){rl.className='err';rl.textContent=t('err')+': '+e.message}rb.disabled=false},'ghost sm');
// agent version vs the protocol this firmware speaks (docs/firmware_v0.2.0.md, 2.6)
const old=s=>s.proto>=0&&s.proto<(s.proto_need||1);
const draw=()=>{const s=c.st||{};g.replaceChildren(kv([[t('pcSt'),s.on?t('pcOn'):t('pcOff'),null,s.on?'okc':''],['PC',s.name||'—',s.ip],['Agent',s.ver||(s.proto==0?t('agentOld'):'—'),old(s)?t('agentUpd'):null,'err'],[t('seen'),s.on?s.seen+' '+t('ago'):'—']]),
h('p',0,rb),rl,h('p',0,h('a',{href:'https://github.com/phuc4real/meowbox-esp32/releases',target:'_blank',rel:'noopener',class:'lk'},t('agentGet')+' ↗')))};
draw();every(async()=>{try{c.st=(await api('/api/config/pc',{quiet:1})).st;draw()}catch(e){}},5000);
return[sec(t('pcSt'),g),sec(t('opts'),h('div',{class:'lim'},h('p',{class:'muted'},t('pcHint')),sw(c,'auto',t('auto')),h('div',{class:'two'},fld(t('pcHost'),bind(c,'host',{placeholder:'192.168.1.10'})),fld(t('port'),bind(c,'port',{type:'number',min:1,max:65535}))),
btn(t('save'),async()=>{Object.assign(c,await put('/api/config/pc',{host:c.host.trim(),port:c.port,auto:c.auto}));draw();toast(t('saved'))})))]}

async function waitBack(msg){const p=h('p',0,msg+' '+t('waiting'));main.replaceChildren(p);const t0=Date.now();await new Promise(r=>setTimeout(r,4000));
while(Date.now()-t0<120000){try{const r=await fetch('/api/session',{cache:'no-store'});if(r.status<500){toast(t('back'));return location.reload()}}catch(e){}await new Promise(r=>setTimeout(r,2000))}
p.textContent=t('netErr')}
// ---------- firmware update from GitHub Releases (docs/firmware_v0.2.0.md, 3.5; API.md "Update") ----------
let updAvail=null;
const updErr=e=>t('updE')[e]||e;
async function updSec(){let u=await api('/api/update');const cfg={auto:!!u.auto};
const box=h('div'),stat=h('p',{class:'muted'}),pr=h('progress',{max:100,value:0,hidden:true}),pt=h('small',{class:'muted'});
const chk=btn(t('updCheck'),async()=>{chk.disabled=true;try{u=await api('/api/update/check',{b:{},quiet:1});draw();poll()}catch(e){chk.disabled=false;if(e.message!='auth')toast(updErr(e.message),1)}},'ghost sm');
const go=async()=>{if(!await ask(t('updGo'),t('updQ',{v:u.new}),t('updGo')))return;
try{u=await api('/api/update',{b:{ver:u.new},quiet:1});poll()}catch(e){toast(updErr(e.message),1);if(e.message=='stale')chk.click()}};
let pt_=null;const poll=()=>{if(pt_)return;pt_=setInterval(async()=>{try{u=await api('/api/update',{quiet:1})}catch(e){return}
draw();if(u.state=='reboot'){clearInterval(pt_);pt_=null;waitBack(t('updReboot'))}else if(!['check','download','verify'].includes(u.state)){clearInterval(pt_);pt_=null}},1000);timers.push(pt_)};
const draw=()=>{const busy=['check','download','verify','reboot'].includes(u.state);chk.disabled=busy;
stat.textContent=t('updCur',{v:u.cur})+' · '+(u.state=='check'?t('updChecking'):u.checked?t('updChecked',{t:fT(u.checked)}):t('updNever'));
pr.hidden=!['download','verify'].includes(u.state);pr.value=u.pct||0;
pt.textContent=u.state=='download'?t('updDl',{p:u.pct||0}):u.state=='verify'?t('updVerify'):u.state=='reboot'?t('updReboot'):'';
const k=[];
if(u.last&&u.last.ver)k.push(h('p',{class:u.last.err?'err':'okc'},u.last.err?t('updRb',{v:u.last.ver,c:u.cur}):t('updOk',{v:u.last.ver})));
if(u.state=='error'&&u.err)k.push(h('p',{class:'err'},updErr(u.err)));
if(u.new){updAvail=u.new;const n=(u.notes&&(u.notes[lang]||u.notes.vi))||[];
k.push(h('div',{class:'upd'},h('b',0,t('updNew',{v:u.new})+(u.date?' ('+u.date+')':'')),n.length?h('ul',0,n.map(x=>h('li',0,x))):null,
u.page?h('a',{href:u.page,target:'_blank',rel:'noopener',class:'lk'},t('updMore')+' ↗'):null,
u.usb_only?h('p',{class:'err'},t('updUsb')):busy?null:h('p',0,btn(t('updGo'),go,'',t('updGo')))))}
else if(u.checked&&u.state!='check'){updAvail=null;k.push(h('p',{class:'muted'},t('updNone')))}
box.replaceChildren(...k);drawNav()};
const au=sw(cfg,'auto',t('updAuto'));au.querySelector('input').addEventListener('change',async()=>{try{await put('/api/config/update',{auto:cfg.auto});toast(t('saved'))}catch(e){}});
draw();if(['check','download','verify'].includes(u.state))poll();
return[h('div',{class:'row'},stat,chk),pr,pt,box,fld('',au,t('updAutoHint'))]}
async function pSys(){const s=await api('/api/status'),pw={old:'',new:'',n2:''},fi=h('input',{type:'file',accept:'.bin,application/octet-stream'}),pr=h('progress',{max:100,value:0,hidden:true}),pt=h('small',{class:'muted'});
const up=btn(t('upload'),async()=>{const f=fi.files[0];if(!f)return;
if(f.size>MAXFW)return toast(t('otaBig'),1);if(new Uint8Array(await f.slice(0,1).arrayBuffer())[0]!=0xE9)return toast(t('otaBad'),1);
up.disabled=true;pr.hidden=false;const x=new XMLHttpRequest();x.open('POST','/api/ota');x.setRequestHeader('Content-Type','application/octet-stream');
x.upload.onprogress=e=>{if(e.lengthComputable){pr.value=e.loaded/e.total*100;pt.textContent=Math.round(pr.value)+'%, '+fB(e.loaded)+' / '+fB(e.total)}};
x.onload=()=>{up.disabled=false;if(x.status==200){toast(t('otaOk'));waitBack(t('otaOk'))}else{let e;try{e=JSON.parse(x.responseText).err}catch(_){e=x.status}if(x.status==401)return showLogin();toast(t('err')+': '+e,1)}};
x.onerror=()=>{up.disabled=false;toast(t('netErr'),1)};x.send(f)});
return[sec(t('dev'),kv([[t('ver'),s.ver,s.build],[t('up'),fUp(s.up)],[t('ip'),s.ip,s.mode=='ap'?t('apMode'):s.ssid],[t('heap'),fB(s.heap),t('minL')+' '+fB(s.heap_min)],[t('psram'),fB(s.psram),t('tot')+' '+fB(s.psram_tot)],[t('time'),fT(s.time),'ICT (UTC+7)']])),
sec(t('ota'),h('div',{class:'lim'},...await updSec(),h('details',{class:'ota'},h('summary',0,t('updFile')),h('p',0,t('otaHint')),fld(t('ota'),fi),pr,pt,h('p',{style:'margin-top:12px'},up)))),
sec(t('chpw'),h('form',{class:'lim',onsubmit:async e=>{e.preventDefault();if(pw.new!=pw.n2)return toast(t('pwMis'),1);if(pw.new.length<6||pw.new.length>32)return toast(t('pwLen'),1);
try{await api('/api/password',{b:{old:pw.old,new:pw.new},quiet:1});toast(t('pwOk'));banner(false);e.target.reset();pw.old=pw.new=pw.n2=''}catch(x){toast(x.status==403?t('badOld'):t('err')+': '+x.message,1)}}},
fld(t('oldPw'),bind(pw,'old',{type:'password',autocomplete:'current-password',required:true})),fld(t('newPw'),bind(pw,'new',{type:'password',autocomplete:'new-password',required:true,minLength:6,maxLength:32})),
fld(t('newPw2'),bind(pw,'n2',{type:'password',autocomplete:'new-password',required:true})),h('button',0,t('chpw')))),
sec(t('nSys'),row(btn(t('reboot'),async()=>{if(await ask(t('reboot'),t('rebootQ'),t('reboot'))){await api('/api/reboot',{b:{}});waitBack(t('reboot'))}},'ghost'),btn(t('logout'),logout,'ghost'))),
sec(t('danger'),h('div',{class:'lim'},h('p',{class:'muted'},t('dataQ')),btn(t('dataReset'),async()=>{if(await ask(t('dataReset'),t('dataQ'),t('dataReset'),1)){await api('/api/factory_reset',{b:{confirm:true,scope:'data'}});waitBack(t('dataOk'))}},'danger'),
h('p',{class:'muted'},t('factoryQ')),btn(t('factory'),async()=>{if(await ask(t('factory'),t('factoryQ'),t('factory'),1)){await api('/api/factory_reset',{b:{confirm:true,scope:'all'}});main.replaceChildren(h('p',0,t('factoryOk')))}},'danger')))]}

// ---------- shell ----------
// key: [nav icon (0 = only in the settings sub-nav), label, page, settings group]
const R={ov:['home','nOv',pOv],radio:['radio','nRadio',pRadio],alarm:['alarm','nAlarm',pAlarm],cat:['cat','nCat',pCat],notify:['bell','nNotify',pNotify],pc:['pc','nPc',pPc],
look:['set','nLook',pLook,1],wifi:[0,'nWifi',pWifi,1],wx:[0,'nWx',pWx,1],sys:[0,'nSys',pSys,1]};
const ALIAS={disp:'look',sleep:'radio'},OWNH1={ov:1,radio:1};
const cur=()=>{let k=location.hash.slice(2);k=ALIAS[k]||k;return R[k]?k:'ov'};
const menu=o=>{nav.classList.toggle('open',!!o);$('#menu').setAttribute('aria-expanded',!!o)};
function drawNav(){const k=cur(),g=R[k][3];nav.replaceChildren(...Object.entries(R).filter(([,r])=>r[0]).map(([x,[i,n,,s]])=>{const on=x==k||s&&g;
return h('a',{href:'#/'+x,class:on?'on':'','aria-current':on?'page':null,onclick:()=>menu(0)},ic(i),t(s?'nSet':n),s&&updAvail?h('span',{class:'dot',title:t('updNew',{v:updAvail})}):null)}))}
let seq=0;
async function route(){if(!authed)return;timers.forEach(clearInterval);timers=[];onVis=null;closePop();const k=cur(),my=++seq,r=R[k];drawNav();menu(0);
main.replaceChildren(h('p',{class:'muted'},t('loading')));
const head=()=>[OWNH1[k]?null:h('h1',0,t(r[3]?'nSet':r[1])),r[3]?h('div',{class:'subnav'},Object.keys(R).filter(x=>R[x][3]).map(x=>h('a',{href:'#/'+x,class:x==k?'on':'','aria-current':x==k?'page':null},t(R[x][1])))):null].filter(Boolean);
try{const n=await r[2]();if(my==seq)main.replaceChildren(...head(),...[n].flat().filter(Boolean))}
catch(e){if(my==seq&&authed)main.replaceChildren(...head(),h('p',{class:'err'},t('loadErr')),btn(t('refresh'),route))}}
function banner(on){const b=$('#banner');b.hidden=!on;if(on)b.replaceChildren(h('span',0,t('defPw')),h('a',{href:'#/sys'},t('defPwLink')))}
function shell(on){nav.hidden=!on;$('#logout').hidden=!on;$('#menu').hidden=!on;if(!on){$('#banner').hidden=true;menu(0)}}
function showLogin(){authed=false;timers.forEach(clearInterval);timers=[];onVis=null;shell(false);if(loginShown)return;loginShown=true;
const o={user:'meowbox',pass:''},msg=h('p',{class:'err',role:'alert'});
main.replaceChildren(h('div',{class:'login'},h('div',{class:'panel'},logo(56),h('h1',0,'MeowBox'),h('form',{onsubmit:async e=>{e.preventDefault();msg.textContent='';
try{const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)});const j=await r.json().catch(()=>({}));
if(r.ok)enter(j.def_pw);else msg.textContent=t(r.status==429?'tooMany':'badLogin')}catch(x){msg.textContent=t('netErr')}}},
fld(t('user'),bind(o,'user',{autocomplete:'username',required:true})),fld(t('pass'),bind(o,'pass',{type:'password',autocomplete:'current-password',required:true})),msg,h('button',{type:'submit'},t('login'))))));
const p=main.querySelector('[type=password]');p&&p.focus()}
function enter(defPw){authed=true;loginShown=false;shell(true);banner(defPw);syncTheme();route();
api('/api/status',{quiet:1}).then(s=>{if(s.upd){updAvail=s.upd;drawNav()}}).catch(()=>{})}
async function logout(){try{await fetch('/api/logout',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'})}catch(e){}showLogin()}
async function setLang(l){try{L=await loadLang(l)}catch(e){return toast(t('netErr'),1)}
lang=l;ls('lang',l);html.lang=l;$('#lang').textContent=l=='vi'?'EN':'VI';$('#logout').title=t('logout');
if(authed){banner(!$('#banner').hidden);route()}else{loginShown=false;showLogin()}}
$('#lang').onclick=()=>setLang(lang=='vi'?'en':'vi');
$('#menu').onclick=()=>menu(!nav.classList.contains('open'));
$('#logout').onclick=logout;
$('#host').textContent=location.hostname;
addEventListener('hashchange',route);
addEventListener('keydown',e=>{if(e.key=='Escape')closePop()});
(async()=>{setTheme(html.dataset.theme);
// strings first: index.html already started fetching them (window.LP) in parallel with app.js
try{L=await(window.LP||loadLang(lang))}catch(e){try{lang='vi';L=await loadLang('vi')}catch(_){}}
html.lang=lang;$('#lang').textContent=lang=='vi'?'EN':'VI';$('#logout').title=t('logout');
// theme before login: unauthenticated GET /api/theme
fetch('/api/theme').then(r=>r.ok?r.json():{}).then(j=>{if(j.theme)setTheme(j.theme,j.fonts)}).catch(()=>{});
try{const r=await fetch('/api/session');if(r.ok)enter((await r.json()).def_pw);else showLogin()}catch(e){showLogin()}})();
