// The setup page served by the Tab5's own Wi-Fi network. One self-contained file: the phone has no internet while it is
// joined to the Tab5, so nothing (fonts, scripts) may be fetched from elsewhere. Mirrors the design mockup.
#pragma once
#include <pgmspace.h>

static const char PORTAL_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tab5 setup</title>
<style>
:root{--bg:#f3f5f7;--card:#fff;--ink:#14181c;--mute:#5a6672;--line:#d5dbe2;--acc:#1e47d1;--ok:#1e8e5a;--bad:#b3261e;--tint:#eef2ff}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;display:flex;flex-direction:column;min-height:100vh}
header{background:var(--card);border-bottom:1px solid var(--line);padding:18px 20px 12px}
header .t{display:flex;justify-content:space-between;align-items:baseline}
header b{font-size:15px}header span{font-size:13px;color:var(--mute)}
#bar{display:flex;gap:4px;margin-top:12px}#bar i{flex:1;height:4px;border-radius:2px;background:var(--line)}#bar i.on{background:var(--acc)}
main{flex:1;padding:22px 20px;max-width:560px;width:100%;margin:0 auto}
footer{background:var(--card);border-top:1px solid var(--line);padding:14px 20px 22px;display:flex;gap:10px;max-width:560px;width:100%;margin:0 auto}
h1{margin:0;font-size:24px;letter-spacing:-.01em}
p.s{margin:6px 0 18px;color:var(--mute);font-size:14px}
label{display:block;font-weight:600;font-size:13px;margin:0 0 6px}
input[type=text],input[type=password],input[type=number],textarea{width:100%;min-height:46px;padding:0 12px;border:1px solid #b8c0c9;border-radius:10px;background:#fff;font:inherit;color:var(--ink)}
textarea{min-height:120px;padding:10px 12px;font:12px/1.3 ui-monospace,Menlo,monospace}
.mono{font-family:ui-monospace,Menlo,monospace}
.f{margin-bottom:16px}
button{font:inherit;font-weight:600;min-height:48px;border-radius:10px;cursor:pointer;border:1px solid #b8c0c9;background:#fff;color:var(--ink);padding:0 14px}
button.p{background:var(--acc);border-color:var(--acc);color:#fff;flex:2}button.g{flex:1}
button:disabled{background:#8a96a3;border-color:#8a96a3;color:#fff;cursor:default}
.net{width:100%;display:flex;justify-content:space-between;align-items:center;text-align:left;min-height:58px;margin-bottom:8px;font-weight:600}
.net small{display:block;font-weight:400;color:var(--mute);font-size:12px}
.net.sel{border:2px solid var(--acc)}
.seg{display:flex;gap:8px}.seg button{flex:1}.seg button.sel{border:2px solid var(--acc);background:var(--tint)}
.card{background:#fff;border:1px solid #b8c0c9;border-radius:12px;padding:14px 16px;margin-bottom:10px}
.card.ok{border-color:var(--ok)}
.card .h{display:flex;justify-content:space-between;align-items:baseline;gap:10px;font-weight:600}
.card .h small{font:12px ui-monospace,Menlo,monospace;color:var(--mute);font-weight:400}
.note{background:var(--tint);border-radius:10px;padding:14px 16px;font-size:14px;margin-top:16px}
.err{color:var(--bad);font-size:13px;margin-top:6px}
.good{color:#1e6b47;font-size:13px;margin-top:6px}
.row{display:flex;justify-content:space-between;gap:12px;padding:11px 0;border-bottom:1px solid #e1e6eb;font-size:14px}
.row span:first-child{color:var(--mute)}.row span:last-child{font:13px ui-monospace,Menlo,monospace;text-align:right;overflow-wrap:anywhere}
a{color:var(--acc)}
.done{text-align:center;padding-top:60px}
</style></head><body>
<header><div class="t"><b>Tab5 setup</b><span id="st"></span></div><div id="bar"></div></header>
<main id="m"></main>
<footer id="ft"><button class="g" id="bk">Back</button><button class="p" id="nx">Next</button></footer>
<script>
const $=id=>document.getElementById(id);
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const NAMES=['Wi-Fi','Broker','Certificates','PIN','Check'];
const S={step:0,ssid:'',wifiPass:'',host:'',port:'',auth:'cert',user:'',pass:'',pin:'',pin2:'',
  pem:{ca:'',crt:'',key:''},info:{ca:null,crt:null,key:null},paste:{ca:false,crt:false,key:false},
  nets:null,sv:{},busy:false,err:'',done:false};
const CERTS=[
 {k:'ca',title:'Broker CA certificate',file:'ca.crt',why:'The Tab5 only talks to a broker signed by this.',has:'hasCa'},
 {k:'crt',title:'Device certificate',file:'tab5.crt',why:'Shows the broker which device is connecting.',has:'hasCrt'},
 {k:'key',title:'Device private key',file:'tab5.key',why:'Stored encrypted. It never leaves the Tab5.',has:'hasKey'}];
const need=()=>S.auth==='cert'?CERTS:CERTS.slice(0,1);

async function api(p,body){
  const r=await fetch(p,body===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  return r.json();
}
async function boot(){
  try{S.sv=await api('/api/state');}catch(e){S.sv={};}
  const v=S.sv;
  if(v.configured){S.ssid=v.ssid||'';S.host=v.host||'';S.auth=v.useCert?'cert':'pw';S.port=String(v.port||'');S.user=v.user||'';}
  if(!S.port)S.port=S.auth==='cert'?'8884':'8883';
  render();scan();
}
async function scan(){
  try{const r=await api('/api/scan');S.nets=r.nets||[];if(r.scanning)setTimeout(scan,2500);}catch(e){S.nets=[];}
  if(S.step===0&&!document.activeElement.matches('input'))render();
}
function keepWifi(){return S.sv.configured&&S.sv.hasWifiPass&&S.ssid===S.sv.ssid;}
function openNet(){const n=(S.nets||[]).find(x=>x.ssid===S.ssid);return n&&n.open;}
function ok(){
  switch(S.step){
   case 0:return S.ssid.length>0&&(S.wifiPass.length>=8||(keepWifi()&&!S.wifiPass)||(openNet()&&!S.wifiPass));
   case 1:{const p=+S.port;if(!S.host||!(p>0&&p<65536))return false;
     if(S.auth==='pw'){return S.user&&(S.pass||(S.sv.hasBrokerPass&&S.sv.user===S.user&&!S.sv.useCert));}return true;}
   case 2:return need().every(c=>S.info[c.k]&&S.info[c.k].ok||(!S.pem[c.k]&&S.sv[c.has]&&S.sv.configured&&!S.sv.recheck));
   case 3:{if(!S.pin&&!S.pin2)return !!S.sv.configured;return /^\d{6}$/.test(S.pin)&&S.pin===S.pin2;}
  }return true;
}
function bar(){$('bar').innerHTML=[0,1,2,3,4].map(i=>'<i class="'+(i<=S.step?'on':'')+'"></i>').join('');
  $('st').textContent=S.done?'Done':'Step '+(S.step+1)+' of 5: '+NAMES[S.step];}
function render(){
  bar();const m=$('m');
  if(S.done){m.innerHTML='<div class="done"><h1>Saved to the Tab5</h1><p class="s">The Tab5 is joining <b class="mono">'+esc(S.ssid)+'</b> and connecting to your broker. Its setup network is closing, so this page will stop responding. You can close it.</p></div>';$('ft').style.display='none';return;}
  $('bk').style.visibility=S.step?'visible':'hidden';
  $('nx').textContent=S.step===4?(S.busy?'Saving...':'Save to Tab5'):'Next';
  let h='';
  if(S.step===0){
    h+='<h1>Choose your Wi-Fi</h1><p class="s">The Tab5 will join this network after setup. It supports 2.4 GHz only.</p>';
    if(S.nets===null)h+='<p class="s">Looking for networks...</p>';
    else h+=S.nets.map((n,i)=>'<button class="net'+(n.ssid===S.ssid?' sel':'')+'" data-n="'+i+'"><span>'+esc(n.ssid)+'<small>'+(n.open?'Open':'Secured')+'</small></span><span class="mono" style="font-size:12px;color:var(--mute)">'+n.rssi+' dBm</span></button>').join('');
    h+='<div class="f"><label for="ssid">Network name</label><input type="text" id="ssid" value="'+esc(S.ssid)+'" autocapitalize="none" autocomplete="off"></div>';
    h+='<div class="f"><label for="wp">Wi-Fi password</label><input type="password" id="wp" value="'+esc(S.wifiPass)+'" autocomplete="off" placeholder="'+(keepWifi()?'Unchanged. Type to replace':'')+'"></div>';
    h+='<p class="s"><a href="#" id="rescan">Scan again</a></p>';
  }
  if(S.step===1){
    h+='<h1>Connect to your broker</h1><p class="s">Where the Tab5 sends its readings.</p>';
    h+='<div class="f"><label for="host">Broker address</label><input type="text" class="mono" id="host" value="'+esc(S.host)+'" autocapitalize="none" autocomplete="off"></div>';
    h+='<div class="f"><span style="display:block;font-weight:600;font-size:13px;margin-bottom:6px">Sign in with</span><div class="seg"><button data-a="pw" class="'+(S.auth==='pw'?'sel':'')+'">Username and password</button><button data-a="cert" class="'+(S.auth==='cert'?'sel':'')+'">Device certificate</button></div></div>';
    h+='<div class="f"><label for="port">Port</label><input type="number" class="mono" id="port" value="'+esc(S.port)+'"></div>';
    if(S.auth==='pw'){
      h+='<div class="f"><label for="user">Username</label><input type="text" class="mono" id="user" value="'+esc(S.user)+'" autocapitalize="none" autocomplete="off"></div>';
      h+='<div class="f"><label for="pass">Password</label><input type="password" id="pass" value="'+esc(S.pass)+'" autocomplete="off" placeholder="'+(S.sv.hasBrokerPass&&!S.sv.useCert?'Unchanged. Type to replace':'')+'"></div>';
    }else h+='<div class="note"><b>No username or password.</b> The Tab5 signs in with a device certificate and key, which you add on the next step.</div>';
  }
  if(S.step===2){
    h+='<h1>'+(S.auth==='cert'?'Add the certificates':'Add the broker certificate')+'</h1><p class="s">'+(S.auth==='cert'?'The Tab5 and the broker check each other. Use the three files from your broker setup script.':'The Tab5 only talks to a broker signed by this certificate. Use ca.crt from your broker setup script.')+'</p>';
    h+=need().map(c=>{
      const i=S.info[c.k],has=S.sv.configured&&S.sv[c.has]&&!S.pem[c.k];
      let body='';
      if(i&&i.ok)body='<div class="good">&#10003; '+esc(i.detail)+'</div><button data-clr="'+c.k+'" style="margin-top:10px;width:100%;min-height:44px">Replace</button>';
      else{
        if(has)body+='<div class="good">&#10003; Already stored on the Tab5. Choose a file only to replace it.</div>';
        else body+='<p class="s" style="margin:6px 0 10px">'+c.why+'</p>';
        if(S.paste[c.k])body+='<textarea data-pt="'+c.k+'" placeholder="-----BEGIN ...">'+esc(S.pem[c.k])+'</textarea><button data-pok="'+c.k+'" style="margin-top:8px;width:100%">Use this text</button>';
        else body+='<input type="file" data-f="'+c.k+'" accept=".crt,.pem,.key,.cer,text/plain" style="width:100%">';
        body+='<p class="s" style="margin:8px 0 0"><a href="#" data-pa="'+c.k+'">'+(S.paste[c.k]?'Choose a file instead':'Paste the text instead')+'</a></p>';
        if(i&&!i.ok)body+='<div class="err">'+esc(i.error)+'</div>';
      }
      return '<div class="card'+((i&&i.ok)||has?' ok':'')+'"><div class="h"><span>'+c.title+'</span><small>'+c.file+'</small></div>'+body+'</div>';
    }).join('');
  }
  if(S.step===3){
    const re=S.sv.configured;
    h+='<h1>'+(re?'Change the PIN':'Set a PIN')+'</h1><p class="s">Asked on the Tab5 before it lets you set it up again.'+(re?' Leave both boxes empty to keep the current PIN.':'')+'</p>';
    h+='<div class="f"><label for="pin">PIN, 6 digits</label><input type="password" class="mono" id="pin" inputmode="numeric" maxlength="6" value="'+esc(S.pin)+'" autocomplete="off" style="letter-spacing:.3em"></div>';
    h+='<div class="f"><label for="pin2">Enter it again</label><input type="password" class="mono" id="pin2" inputmode="numeric" maxlength="6" value="'+esc(S.pin2)+'" autocomplete="off" style="letter-spacing:.3em"></div>';
    h+='<div id="pm" class="'+(S.pin&&S.pin===S.pin2&&/^\d{6}$/.test(S.pin)?'good':'err')+'">'+(S.pin||S.pin2?(S.pin===S.pin2&&/^\d{6}$/.test(S.pin)?'&#10003; The two PINs match':'Enter the same 6 digits twice'):'')+'</div>';
    h+='<p class="s" style="margin-top:12px">After 5 wrong tries the Tab5 waits 1 minute. A forgotten PIN cannot be recovered. The only way back in is a full reset, which erases everything saved here.</p>';
  }
  if(S.step===4){
    const R=(a,b)=>'<div class="row"><span>'+a+'</span><span>'+esc(b)+'</span></div>';
    const kept=(typed,has)=>typed?'••••••••':(has?'Unchanged':'(none)');
    h+='<h1>Check and save</h1><p class="s">Passwords and the key are stored encrypted and will not be shown again.</p><div class="card" style="padding:4px 16px">';
    h+=R('Wi-Fi',S.ssid)+R('Wi-Fi password',kept(S.wifiPass,keepWifi()))+R('Broker',S.host+':'+S.port);
    h+=R('Broker CA',S.info.ca&&S.info.ca.ok?S.info.ca.name:'Unchanged');
    if(S.auth==='pw')h+=R('Username',S.user)+R('Broker password',kept(S.pass,S.sv.hasBrokerPass&&!S.sv.useCert));
    else h+=R('Device certificate',S.info.crt&&S.info.crt.ok?S.info.crt.name:'Unchanged')+R('Device key',S.info.key&&S.info.key.ok?'••••••••':'Unchanged');
    h+=R('PIN',S.pin?'••••••':'Unchanged')+'</div>';
    h+='<p class="s" style="margin-top:14px">The Tab5 restarts after saving and then joins the Wi-Fi. If it cannot join, it opens this setup again so you can fix the password.</p>';
    if(S.err)h+='<div class="err" style="font-size:15px">'+esc(S.err)+'</div>';
  }
  m.innerHTML=h;bind();
  $('nx').disabled=S.busy||!ok();
}
function bind(){
  const v=(id,k)=>{const e=$(id);if(e)e.oninput=()=>{S[k]=e.value;if(k==='ssid')S.err='';chk();};};
  v('ssid','ssid');v('wp','wifiPass');v('host','host');v('port','port');v('user','user');v('pass','pass');v('pin','pin');v('pin2','pin2');
  document.querySelectorAll('[data-n]').forEach(b=>b.onclick=()=>{S.ssid=S.nets[+b.dataset.n].ssid;render();});
  document.querySelectorAll('[data-a]').forEach(b=>b.onclick=()=>{const a=b.dataset.a;if(a!==S.auth){S.auth=a;if(S.port===(a==='pw'?'8884':'8883'))S.port=a==='pw'?'8883':'8884';}render();});
  document.querySelectorAll('[data-f]').forEach(e=>e.onchange=async()=>{if(e.files[0])setPem(e.dataset.f,await e.files[0].text());});
  document.querySelectorAll('[data-pa]').forEach(a=>a.onclick=ev=>{ev.preventDefault();S.paste[a.dataset.pa]=!S.paste[a.dataset.pa];render();});
  document.querySelectorAll('[data-pt]').forEach(t=>t.oninput=()=>{S.pem[t.dataset.pt]=t.value;});
  document.querySelectorAll('[data-pok]').forEach(b=>b.onclick=()=>setPem(b.dataset.pok,document.querySelector('[data-pt="'+b.dataset.pok+'"]').value));
  document.querySelectorAll('[data-clr]').forEach(b=>b.onclick=()=>{S.info[b.dataset.clr]=null;S.pem[b.dataset.clr]='';render();});
  const rs=$('rescan');if(rs)rs.onclick=ev=>{ev.preventDefault();S.nets=null;render();scan();};
}
function chk(){$('nx').disabled=S.busy||!ok();const pm=$('pm');if(pm&&S.step===3){const g=S.pin&&S.pin===S.pin2&&/^\d{6}$/.test(S.pin);pm.className=g?'good':'err';pm.innerHTML=S.pin||S.pin2?(g?'&#10003; The two PINs match':'Enter the same 6 digits twice'):'';}}
async function setPem(k,text){
  S.pem[k]=text;S.info[k]={ok:false,error:'Checking...'};render();
  try{const r=await api('/api/check',{kind:k,pem:text});
    S.info[k]=r.ok?{ok:true,detail:r.detail,name:r.name}:{ok:false,error:r.error||'Not accepted'};if(!r.ok)S.pem[k]='';}
  catch(e){S.info[k]={ok:false,error:'The Tab5 did not answer'};S.pem[k]='';}
  render();
}
$('bk').onclick=()=>{if(S.step>0){S.step--;S.err='';render();}};
$('nx').onclick=async()=>{
  if(S.step<4){S.step++;render();window.scrollTo(0,0);return;}
  S.busy=true;S.err='';render();
  const b={ssid:S.ssid,wifiPass:S.wifiPass,host:S.host,port:+S.port,auth:S.auth,user:S.user,pass:S.pass,pin:S.pin,
    ca:S.pem.ca,crt:S.auth==='cert'?S.pem.crt:'',key:S.auth==='cert'?S.pem.key:''};
  try{const r=await api('/api/save',b);
    if(r.ok){S.done=true;}else S.err=r.error||'The Tab5 did not accept the settings';}
  catch(e){S.err='The Tab5 did not answer. If it restarted, the settings may have been saved.';}
  S.busy=false;render();
};
boot();
</script></body></html>
)HTML";
