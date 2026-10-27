#pragma once
#include <Arduino.h>

// Web UI served from flash (PROGMEM). All dynamic text is inserted via
// textContent, so SSIDs / coin names can't break the markup.
const char HTML_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Crypto Ticker</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,system-ui,sans-serif;background:linear-gradient(135deg,#0f0f23,#1a1a3e);min-height:100vh;padding:15px;color:#fff}
.c{max-width:420px;margin:0 auto}
h1{text-align:center;margin-bottom:20px;color:#f7931a}
.card{background:rgba(255,255,255,.05);border-radius:12px;padding:15px;margin-bottom:15px;border:1px solid rgba(255,255,255,.1)}
.card h2{font-size:14px;margin-bottom:12px;color:#888;text-transform:uppercase}
input{width:100%;padding:10px;border:1px solid rgba(255,255,255,.15);border-radius:8px;background:rgba(0,0,0,.4);color:#fff;font-size:16px;margin-bottom:10px}
input:focus{outline:none;border-color:#f7931a}
button{width:100%;padding:12px;border:none;border-radius:8px;font-size:14px;font-weight:600;cursor:pointer;color:#fff}
.b1{background:linear-gradient(135deg,#f7931a,#ff6b00)}
.b2{background:rgba(255,255,255,.1);margin-bottom:10px}
#wl{max-height:180px;overflow-y:auto;margin-bottom:10px}
.wi{padding:10px;background:rgba(0,0,0,.3);border-radius:6px;margin-bottom:6px;cursor:pointer;display:flex;justify-content:space-between;gap:8px}
.wi span:first-child{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.wi:hover{background:rgba(247,147,26,.2)}
.wi.sel{background:rgba(247,147,26,.3);outline:1px solid #f7931a}
.st{text-align:center;padding:12px;border-radius:8px;font-size:13px;line-height:1.6}
.on{background:rgba(0,255,136,.15);color:#0f8}
.off{background:rgba(255,200,87,.15);color:#fc8}
.load{background:rgba(100,100,255,.15);color:#aaf}
.ip{font-family:monospace;background:rgba(0,0,0,.3);padding:4px 10px;border-radius:4px}
.cc{text-align:center;padding:15px;background:rgba(247,147,26,.1);border-radius:10px;margin-bottom:12px}
.sym{font-size:24px;font-weight:bold;color:#f7931a}
.pr{font-size:18px;margin-top:4px;font-variant-numeric:tabular-nums}
.up{color:#0f8}.dn{color:#f66}
.sb{position:relative;margin-bottom:10px}
.sb input{padding-left:35px;margin:0}
.sb::before{content:"\1F50D";position:absolute;left:10px;top:50%;transform:translateY(-50%)}
.cg{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;max-height:220px;overflow-y:auto;margin-bottom:8px}
.cb{padding:8px 4px;background:rgba(0,0,0,.3);border:1px solid transparent;border-radius:6px;font-size:12px;font-weight:400}
.cb:hover{background:rgba(247,147,26,.2)}
.cb.sel{background:rgba(247,147,26,.3);border-color:#f7931a}
.ld{text-align:center;padding:20px;display:none}
.ld.on{display:block;background:none}
.sp{width:24px;height:24px;border:3px solid rgba(255,255,255,.2);border-top-color:#f7931a;border-radius:50%;animation:spin 1s linear infinite;margin:0 auto 8px}
@keyframes spin{to{transform:rotate(360deg)}}
.toast{position:fixed;bottom:15px;left:50%;transform:translateX(-50%);background:#333;padding:10px 20px;border-radius:6px;opacity:0;transition:opacity .3s;z-index:99;pointer-events:none}
.toast.show{opacity:1}.toast.ok{background:#00a67e}.toast.err{background:#e74c3c}
.note{font-size:11px;color:#888;text-align:center}
</style></head><body><div class="c">
<h1>&#8383; Crypto Ticker</h1>
<div class="card"><div id="st" class="st load">Loading...</div></div>
<div class="card"><h2>Currency</h2>
<div class="cc"><div class="sym" id="cs">---</div><div class="pr" id="cp">---</div></div>
<div class="sb"><input type="text" id="q" placeholder="Search..." autocapitalize="characters"></div>
<div id="cl" class="ld on"><div class="sp"></div><div>Loading currencies...</div></div>
<div id="cg" class="cg"></div>
<div id="note" class="note"></div>
</div>
<div class="card"><h2>WiFi</h2>
<button class="b2" id="scanBtn">Scan Networks</button>
<div id="wld" class="ld"><div class="sp"></div><div>Scanning...</div></div>
<div id="wl"></div>
<input type="text" id="ssid" placeholder="WiFi Name (SSID)" maxlength="32">
<input type="password" id="pw" placeholder="Password" maxlength="64">
<button class="b1" id="saveBtn">Save &amp; Restart</button>
</div>
<div class="note" id="ver"></div>
</div><div id="toast" class="toast"></div>
<script>
const $=id=>document.getElementById(id);
const APIS=['https://api.bybit.com','https://api.bytick.com'];
const FALLBACK=['BTC','ETH','BNB','SOL','XRP','DOGE','ADA','AVAX','DOT','POL','LINK','LTC','UNI','ATOM','SHIB','PEPE','ARB','OP','SUI','APT','INJ','TIA','FET','WLD','TON','NEAR','S','ALGO','XLM','SAND','MANA','AXS','GALA','APE','FLOKI','BONK','WIF','RENDER','JUP','SEI','STRK','PYTH','ENA','TRX','BCH','HBAR','AAVE'];
let all=[],sel='',toastT;

function toast(m,t){const e=$('toast');e.textContent=m;e.className='toast show '+(t||'');clearTimeout(toastT);toastT=setTimeout(()=>e.className='toast',2500)}
function tfetch(u,o,ms){const c=new AbortController(),t=setTimeout(()=>c.abort(),ms||8000);return fetch(u,Object.assign({},o,{signal:c.signal})).finally(()=>clearTimeout(t))}
function post(u,d){const f=new URLSearchParams(d);return tfetch(u,{method:'POST',body:f},5000).then(r=>r.json())}

async function loadCoins(){
  for(const api of APIS){
    try{
      const d=await (await tfetch(api+'/v5/market/tickers?category=spot',{},8000)).json();
      if(d.retCode!==0)throw 0;
      const set=new Set();
      d.result.list.forEach(i=>{if(i.symbol.endsWith('USDT')&&i.symbol.length>4)set.add(i.symbol.slice(0,-4))});
      all=[...set].sort();
      $('note').textContent='Loaded '+all.length+' USDT pairs from Bybit';
      break;
    }catch(e){}
  }
  if(!all.length){all=FALLBACK.slice();$('note').textContent='Offline: built-in list ('+all.length+' coins)'}
  $('cl').classList.remove('on');
  render();
}

function render(){
  const v=$('q').value.trim().toUpperCase(),g=$('cg');
  const list=(v?all.filter(n=>n.includes(v)):all).slice(0,120);
  g.textContent='';
  list.forEach(n=>{
    const b=document.createElement('button');
    b.className='cb'+(n+'USDT'===sel?' sel':'');b.textContent=n;
    b.onclick=()=>selCoin(n);g.appendChild(b);
  });
}
$('q').oninput=render;

async function selCoin(n){
  try{
    const d=await post('/sc',{symbol:n+'USDT'});
    if(!d.ok)throw 0;
    sel=n+'USDT';$('cs').textContent=n;$('cp').textContent='...';$('cp').className='pr';
    render();toast(n+' selected','ok');
  }catch(e){toast('Error saving','err')}
}

let scanT;
async function scan(){
  $('wld').classList.add('on');$('wl').textContent='';
  try{await tfetch('/scan?start=1',{},3000)}catch(e){}
  const t0=Date.now();clearInterval(scanT);
  scanT=setInterval(async()=>{
    if(Date.now()-t0>15000){clearInterval(scanT);$('wld').classList.remove('on');toast('Scan timeout','err');return}
    try{
      const d=await (await tfetch('/scan',{},3000)).json();
      if(d.st!=='done')return;
      clearInterval(scanT);$('wld').classList.remove('on');
      const wl=$('wl');
      if(!d.n.length){wl.textContent='No networks found';return}
      d.n.forEach(x=>{
        const r=document.createElement('div'),a=document.createElement('span'),b=document.createElement('span');
        r.className='wi';a.textContent=x.s+(x.e?' \u{1F512}':'');
        b.textContent=x.r>-55?'▂▄▆█':x.r>-70?'▂▄▆':x.r>-80?'▂▄':'▂';
        r.append(a,b);
        r.onclick=()=>{$('ssid').value=x.s;wl.querySelectorAll('.wi').forEach(e=>e.classList.toggle('sel',e===r));$('pw').focus()};
        wl.appendChild(r);
      });
    }catch(e){}
  },1000);
}

async function saveWifi(){
  const s=$('ssid').value,p=$('pw').value;
  if(!s){toast('Enter SSID','err');return}
  if(p&&p.length<8){toast('Password: min 8 chars','err');return}
  try{
    const d=await post('/sw',{ssid:s,password:p});
    if(!d.ok)throw 0;
    toast('Saved! Restarting...','ok');
    $('st').className='st load';
    $('st').textContent='Restarting. Reconnect to "'+s+'" and open http://cryptoticker.local or the IP shown on the LCD.';
  }catch(e){toast('Error','err')}
}

async function status(){
  try{
    const d=await (await tfetch('/st',{},3000)).json(),e=$('st');
    e.textContent='';
    const add=(t,cls)=>{const s=document.createElement('span');s.textContent=t;if(cls)s.className=cls;e.appendChild(s);e.appendChild(document.createElement('br'))};
    if(d.c){e.className='st on';add('Connected: '+d.s);add(d.i,'ip')}
    else{e.className='st off';add('Setup mode (access point)');add('192.168.4.1','ip');add('Configure WiFi below')}
    if(d.y){
      sel=d.y;$('cs').textContent=d.n;
      if(d.p){$('cp').textContent='$'+d.p+(d.ch?'  '+d.ch:'');$('cp').className='pr '+(d.ch&&d.ch[0]==='-'?'dn':'up')}
      else $('cp').textContent=d.err||'---';
    }
    if(d.ss&&!$('ssid').value)$('ssid').value=d.ss;
    $('ver').textContent='v'+d.v+' · heap '+d.h+' B';
  }catch(e){}
}

$('scanBtn').onclick=scan;$('saveBtn').onclick=saveWifi;
status().then(loadCoins);
setInterval(status,5000);
</script></body></html>)rawliteral";
