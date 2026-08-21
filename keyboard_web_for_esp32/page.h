#pragma once
// 页面单独放头文件:Arduino 的 .ino 预处理器不理解原始字符串字面量,
// 会把页面 JS 里的 function xx(){ 误当 C++ 函数定义,生成原型并注入 #line 到字符串里,
// 导致浏览器 SyntaxError: Bare private name(实测于 esp32 core 3.3.11)。
// 预处理器只处理 .ino,不碰 .h,放这里就安全。

// ---------------- 虚拟键盘网页(PROGMEM,三个占位符运行时替换) ----------------
// {{SSID}} {{IP}} {{CNT}}
// 交互模型与真键盘一致:按下发 /down(按键保持),松开发 /up;
// 长按的自动重复由 PS5 端实现(真键盘也是如此);多指可组合键。
// Shift/Caps 为页面切换开关(不发 HID 事件,直接发大写/符号 ASCII)。
static const char PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>
<title>PS5 Remote Keyboard</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;user-select:none;-webkit-user-select:none}
html,body{overscroll-behavior:none}
body{font-family:-apple-system,sans-serif;background:#f2f3f5;margin:0;padding:10px 8px 30px;text-align:center}
h2{margin:8px 0 2px;font-size:18px}
.info{color:#777;font-size:12px;margin-bottom:8px}
.bar{display:flex;gap:6px;justify-content:center;flex-wrap:wrap;margin:8px 0}
.bar button{font-size:15px;padding:12px 22px;border:0;border-radius:8px;color:#fff;background:#2f6df6}
.bar button:active{background:#1c4fd0}
#kb{max-width:720px;margin:0 auto}
.row{display:flex;gap:4px;margin:4px 0;justify-content:center}
.k{flex:1;min-width:0;padding:12px 0;border:0;border-radius:6px;background:#fff;
   font-size:15px;color:#222;box-shadow:0 1px 2px #0003;cursor:pointer;overflow:hidden;white-space:nowrap;
   touch-action:none;transition:background .06s,transform .06s}
.k.pressed{background:#2f6df6;color:#fff;transform:scale(.94);box-shadow:none}
.w15{flex:1.5}.w20{flex:2}.w25{flex:2.5}.sp{flex:6}
.on{background:#2f6df6;color:#fff}
.on.pressed{background:#1c4fd0}
.foot{color:#999;font-size:12px;margin-top:16px}
a{color:#2f6df6}
</style></head><body>
<h2>PS5 远程键盘</h2>
<div class='info'>WiFi: {{SSID}} | IP: {{IP}} | 已发送按键: <b id='cnt'>{{CNT}}</b></div>
<div class='bar'>
  <button id='bigEnter'>按 回车 (确认弹窗)</button>
  <button onclick="ent3()">连按 3 次</button>
</div>
<div id='kb'></div>
<div class='foot'>固件 v3.5(POST+CORS) | 按住不放 = 长按连发(PS5 端重复) | 可多指组合键 | 短按 BOOT 键本地触发回车 | <a href='/resetwifi'>重新配网</a> | <a href='/reboot'>重启</a></div>
<script>
var SH={'1':'!','2':'@','3':'#','4':'$','5':'%','6':'^','7':'&','8':'*','9':'(','0':')','-':'_','=':'+','[':'{',']':'}','\\':'|',';':':',"'":'"',',':'<','.':'>','/':'?'};
var shift=false,caps=false;
var ROWS=[
 [177,'1','2','3','4','5','6','7','8','9','0','-','=',178],
 [179,'q','w','e','r','t','y','u','i','o','p','[',']','\\'],
 [193,'a','s','d','f','g','h','j','k','l',';',"'",176],
 ['SHIFT','z','x','c','v','b','n','m',',','.','/','SHIFT'],
 [128,130,32,216,218,217,215]
];
var NAME={177:'Esc',178:'Bksp',179:'Tab',193:'Caps',176:'Enter',32:'Space',216:'←',218:'↑',217:'↓',215:'→',128:'Ctrl',130:'Alt'};
function disp(k){
  if(typeof k=='number')return NAME[k]||k;
  if(k=='SHIFT')return 'Shift';
  if(shift)return SH[k]||k.toUpperCase();
  return k;
}
function code(k){
  if(typeof k=='number')return k;
  var up=shift;
  if(/^[a-z]$/.test(k))up=(shift!=caps);
  if(up){var s=SH[k];return s?s.charCodeAt(0):k.toUpperCase().charCodeAt(0);}
  return k.charCodeAt(0);
}
function cnt(){fetch('/cnt').then(function(r){return r.text()}).then(function(t){document.getElementById('cnt').textContent=t})}
function ent3(){fetch('/enter?n=3&gap=800');cnt()}
// 大回车按钮:同真键盘,按下即 down,松开才 up
(function(){
  var b=document.getElementById('bigEnter');
  b.addEventListener('pointerdown',function(e){e.preventDefault();
    try{b.setPointerCapture(e.pointerId)}catch(e2){}
    b.classList.add('pressed');fetch('/down?d=176');});
  function up(){b.classList.remove('pressed');fetch('/up?d=176');cnt()}
  b.addEventListener('pointerup',up);
  b.addEventListener('pointercancel',up);
})();
function render(){
  var kb=document.getElementById('kb');kb.innerHTML='';
  ROWS.forEach(function(row){
    var r=document.createElement('div');r.className='row';
    row.forEach(function(k){
      var b=document.createElement('button');b.className='k';
      b.textContent=disp(k);
      if(k==176)b.className+=' w25';
      if(k==178||k==179)b.className+=' w15';
      if(k==193&&caps)b.className+=' on';
      if(k=='SHIFT'){b.className+=' w20';if(shift)b.className+=' on'}
      if(k==32)b.className+=' sp';
      if(k==128||k==130)b.className+=' w20';
      // ---- 按下/松开,和真键盘一致 ----
      b.addEventListener('pointerdown',function(e){
        e.preventDefault();
        try{b.setPointerCapture(e.pointerId)}catch(e2){}
        if(k=='SHIFT'){shift=!shift;render();return;}   // 切换开关,不发 HID
        if(k==193){caps=!caps;render();return;}         // 切换开关,不发 HID
        b.dataset.c=code(k);
        b.classList.add('pressed');
        fetch('/down?d='+b.dataset.c);
      });
      function up(){
        if(b.dataset.c){fetch('/up?d='+b.dataset.c);delete b.dataset.c;cnt();}
        b.classList.remove('pressed');
      }
      b.addEventListener('pointerup',up);
      b.addEventListener('pointercancel',up);
      b.addEventListener('lostpointercapture',up);
      b.oncontextmenu=function(){return false};        // 长按不弹菜单
      r.appendChild(b);
    });
    kb.appendChild(r);
  });
}
render();
</script></body></html>)rawliteral";
