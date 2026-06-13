// ============================================================
//  index_html.h
//  Web dashboard markup for the mining cluster master.
//
//  This lives in its own header (NOT in the .ino) on purpose:
//  the Arduino sketch preprocessor runs a ctags-based prototype
//  generator over every .ino file, and that parser does not
//  understand C++ raw string literals. When the HTML/JS below
//  was inline in Master.ino it mistook the embedded JavaScript
//  (e.g. "function fmtRate(...)") for C++ and the build failed
//  with "'function' does not name a type". Headers are not run
//  through that generator, so the raw string is left intact.
// ============================================================
#ifndef INDEX_HTML_H
#define INDEX_HTML_H

#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTMLDOC(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SHA-256 Cluster</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><rect width='32' height='32' rx='7' fill='%23001018'/><text x='16' y='23' font-size='19' text-anchor='middle' fill='%2300e5ff' font-family='monospace'>%23</text></svg>">
<style>
:root{--panel:#0c1322;--cyan:#00e5ff;--mag:#ff2bd6;--green:#39ff14;--red:#ff3b3b;--grey:#6b7a90;--text:#e8f1ff;--line:#16324a;}
*{box-sizing:border-box;}
body{margin:0;background:radial-gradient(circle at 50% -10%,#0a1530,#05070d 60%);color:var(--text);font-family:'Segoe UI',Roboto,system-ui,monospace;-webkit-font-smoothing:antialiased;}
.wrap{max-width:920px;margin:0 auto;padding:18px;}
.head{display:flex;align-items:center;gap:10px;}
h1{font-size:20px;letter-spacing:3px;margin:0;color:var(--cyan);text-shadow:0 0 8px var(--cyan);}
.live{margin-left:auto;font-size:11px;letter-spacing:2px;color:var(--green);display:flex;align-items:center;gap:7px;}
.live b{width:8px;height:8px;border-radius:50%;background:var(--green);box-shadow:0 0 8px var(--green);animation:blink 1.4s infinite;}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}
.rule{height:2px;border-radius:2px;margin:8px 0 4px;background:linear-gradient(90deg,var(--cyan),var(--mag),transparent);}
.sub{color:var(--grey);font-size:12px;margin:6px 0 16px;word-break:break-all;}
.hero{text-align:center;padding:22px;border:1px solid var(--line);border-radius:12px;background:linear-gradient(180deg,#0c1322,#070c16);animation:heroPulse 3s ease-in-out infinite;}
@keyframes heroPulse{0%,100%{box-shadow:0 0 22px rgba(0,229,255,.07)}50%{box-shadow:0 0 36px rgba(0,229,255,.17)}}
.hero .rate{font-size:48px;font-weight:700;color:var(--green);text-shadow:0 0 16px rgba(57,255,20,.5);}
.hero .unit{font-size:16px;color:var(--grey);}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(118px,1fr));gap:10px;margin:16px 0;}
.stat{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:10px 12px;}
.stat .k{font-size:11px;color:var(--grey);letter-spacing:1px;}
.stat .v{font-size:20px;font-weight:600;margin-top:2px;}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:10px;}
.node{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:11px;transition:transform .15s ease,border-color .15s ease;}
.node:hover{transform:translateY(-2px);border-color:var(--cyan);}
.nrow{display:flex;align-items:center;gap:10px;}
.dot{width:12px;height:12px;border-radius:50%;flex:none;box-shadow:0 0 8px currentColor;}
.node .id{font-size:18px;font-weight:700;line-height:1;}
.node .meta{font-size:10px;color:var(--grey);letter-spacing:.5px;margin-top:3px;}
.node .nr{margin-left:auto;text-align:right;color:var(--cyan);font-weight:600;font-size:13px;}
.tag{font-size:9px;letter-spacing:1px;padding:1px 5px;border-radius:5px;border:1px solid var(--line);color:var(--grey);}
.bar{height:4px;border-radius:3px;background:#0a1830;margin-top:9px;overflow:hidden;}
.bar i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--cyan),var(--mag));box-shadow:0 0 8px var(--cyan);transition:width .6s ease;}
.foot{margin-top:16px;color:var(--grey);font-size:11px;text-align:center;word-break:break-all;}
</style></head><body><div class="wrap">
<div class="head"><h1>SHA-256 MINING CLUSTER</h1><div class="live"><b></b>LIVE</div></div>
<div class="rule"></div>
<div class="sub" id="sub">connecting...</div>
<div class="hero"><div class="rate"><span id="rate">--</span> <span class="unit">kH/s</span></div></div>
<div class="stats">
<div class="stat"><div class="k">NODES ONLINE</div><div class="v" id="online">-/10</div></div>
<div class="stat"><div class="k">SHARES FOUND</div><div class="v" id="found">-</div></div>
<div class="stat"><div class="k">ACCEPTED</div><div class="v" style="color:var(--green)" id="acc">-</div></div>
<div class="stat"><div class="k">REJECTED</div><div class="v" id="rej">-</div></div>
<div class="stat"><div class="k">DIFFICULTY</div><div class="v" id="diff">-</div></div>
<div class="stat"><div class="k">UPTIME</div><div class="v" id="up">-</div></div>
</div>
<div class="grid" id="grid"></div>
<div class="foot" id="foot"></div>
</div><script>
var ST={0:'READY',1:'MINING',2:'SHARE',3:'DONE'};
function fmtRate(h){return (h/1000).toFixed(2);}
function fmtUp(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60),x=s%60,o='';if(d)o+=d+'d ';if(d||h)o+=h+'h ';return o+m+'m '+x+'s';}
function esc(t){return String(t).replace(/[&<>]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;'}[c];});}
async function tick(){
 var ctrl=new AbortController(),to=setTimeout(function(){ctrl.abort();},4000);
 try{
  var j=await (await fetch('/api',{signal:ctrl.signal})).json();clearTimeout(to);
  document.getElementById('rate').textContent=fmtRate(j.hashrate);
  document.getElementById('online').textContent=j.online+'/10';
  document.getElementById('found').textContent=j.sharesFound;
  document.getElementById('acc').textContent=j.sharesAccepted;
  document.getElementById('rej').textContent=j.sharesRejected;
  document.getElementById('diff').textContent=(+j.difficulty).toFixed(5);
  document.getElementById('up').textContent=fmtUp(j.uptime);
  document.getElementById('sub').textContent=j.pool+'   -   '+j.ip;
  document.getElementById('foot').textContent=j.worker;
  var max=1;j.slaves.forEach(function(s){if(s.online&&s.hashrate>max)max=s.hashrate;});
  var g=document.getElementById('grid');g.innerHTML='';
  j.slaves.forEach(function(s){
   var hex=s.addr.toString(16).toUpperCase().padStart(2,'0'),on=s.online;
   var tag=on?(ST[s.status]||'-'):'OFFLINE',col=on?'#39ff14':'#ff3b3b';
   var pct=on?Math.round(s.hashrate/max*100):0;
   var d=document.createElement('div');d.className='node';
   d.innerHTML='<div class="nrow"><div class="dot" style="color:'+col+'"></div>'+
     '<div><div class="id">'+hex+'</div><div class="meta">BUS '+s.bus+' &middot; '+(s.bus==0?'S3':'C3')+' &middot; <span class="tag">'+esc(tag)+'</span></div></div>'+
     '<div class="nr">'+(on?fmtRate(s.hashrate)+'<br><span style="color:var(--grey);font-weight:400">kH/s</span>':'<span style="color:var(--red)">--</span>')+'</div></div>'+
     '<div class="bar"><i style="width:'+pct+'%"></i></div>';
   g.appendChild(d);
  });
 }catch(e){clearTimeout(to);document.getElementById('sub').textContent='link lost... retrying';}
}
tick();setInterval(tick,2000);
</script></body></html>)HTMLDOC";

#endif // INDEX_HTML_H
