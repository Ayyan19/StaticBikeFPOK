// =============================================================================
//  index_html.h  --  Dashboard live disajikan langsung dari ESP32.
//  Klien (HP/laptop) buka http://<ip-esp32>/ lalu menerima data via WebSocket.
//  Disimpan di PROGMEM agar tidak memakan RAM.
// =============================================================================
#ifndef INDEX_HTML_H
#define INDEX_HTML_H

const char INDEX_HTML[] PROGMEM = R"HTMLDOC(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Static Bike Monitor</title>
<style>
  :root{--bg:#0b1020;--card:#151b2e;--acc:#2dd4bf;--txt:#e6edf3;--dim:#8b97a7;}
  *{box-sizing:border-box;font-family:system-ui,Segoe UI,Roboto,sans-serif;}
  body{margin:0;background:var(--bg);color:var(--txt);}
  header{padding:16px 20px;display:flex;align-items:center;gap:12px;
    border-bottom:1px solid #202840;}
  header h1{font-size:18px;margin:0;font-weight:600;}
  #status{font-size:12px;padding:3px 10px;border-radius:20px;background:#3a2020;color:#ff6b6b;}
  #status.on{background:#183a2a;color:#4ade80;}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));
    gap:12px;padding:16px;max-width:900px;margin:0 auto;}
  .card{background:var(--card);border-radius:14px;padding:16px;text-align:center;
    border:1px solid #1e2740;}
  .label{font-size:12px;color:var(--dim);text-transform:uppercase;letter-spacing:.5px;}
  .value{font-size:34px;font-weight:700;margin-top:6px;line-height:1;}
  .unit{font-size:13px;color:var(--dim);margin-left:4px;font-weight:400;}
  .hr .value{color:#ff6b6b;} .pw .value{color:#f59e0b;}
  .sp .value{color:#4ade80;} .tq .value{color:#facc15;} .cd .value{color:var(--acc);}
  .barwrap{height:8px;background:#0b1020;border-radius:6px;margin-top:10px;overflow:hidden;}
  .bar{height:100%;background:var(--acc);width:0%;transition:width .3s;}
  footer{text-align:center;color:var(--dim);font-size:12px;padding:14px;}
  canvas{width:100%;max-width:900px;margin:0 auto;display:block;background:var(--card);
    border-radius:14px;}
</style>
</head>
<body>
<header>
  <h1>🚴 Static Bike Monitor</h1>
  <span id="status">terputus</span>
  <span id="clock" style="margin-left:auto;color:var(--dim);font-size:14px">00:00</span>
</header>

<div class="grid">
  <div class="card cd"><div class="label">Cadence</div>
    <div class="value"><span id="cadence">0</span><span class="unit">rpm</span></div></div>
  <div class="card sp"><div class="label">Speed</div>
    <div class="value"><span id="speed">0</span><span class="unit">km/j</span></div></div>
  <div class="card tq"><div class="label">Torsi</div>
    <div class="value"><span id="torque">0</span><span class="unit">Nm</span></div></div>
  <div class="card pw"><div class="label">Power</div>
    <div class="value"><span id="power">0</span><span class="unit">W</span></div></div>
  <div class="card"><div class="label">Level Beban</div>
    <div class="value"><span id="level">1</span></div>
    <div class="barwrap"><div class="bar" id="levelbar"></div></div></div>
  <div class="card hr"><div class="label">Heart Rate</div>
    <div class="value"><span id="hr">--</span><span class="unit">bpm</span></div></div>
  <div class="card"><div class="label">Energi</div>
    <div class="value"><span id="energy">0</span><span class="unit">kJ</span></div></div>
  <div class="card"><div class="label">Kalori</div>
    <div class="value"><span id="cal">0</span><span class="unit">kcal</span></div></div>
</div>

<canvas id="chart" width="900" height="200"></canvas>
<footer>Data real-time via WebSocket &bull; ESP32</footer>

<script>
const $ = id => document.getElementById(id);
const badge = $('status');
let ws, hist = [];          // riwayat power untuk grafik
const MAXPTS = 120;

function connect(){
  ws = new WebSocket('ws://' + location.host + '/ws');
  ws.onopen  = () => { badge.textContent='terhubung'; badge.classList.add('on'); };
  ws.onclose = () => { badge.textContent='terputus'; badge.classList.remove('on');
                        setTimeout(connect, 1500); };
  ws.onmessage = e => { try{ render(JSON.parse(e.data)); }catch(_){} };
}

function render(d){
  $('cadence').textContent = (d.cadence ?? 0).toFixed(0);
  $('speed').textContent   = (d.speed ?? 0).toFixed(1);
  $('torque').textContent  = (d.torque ?? 0).toFixed(1);
  $('power').textContent   = (d.power ?? 0).toFixed(0);
  $('level').textContent   = d.level ?? 1;
  $('hr').textContent      = d.hr ? d.hr : '--';
  $('energy').textContent  = (d.energyKJ ?? 0).toFixed(1);
  $('cal').textContent     = (d.cal ?? 0).toFixed(0);
  $('levelbar').style.width = (100 * (d.level ?? 1) / 8) + '%';
  const s = d.elapsed ?? 0;
  $('clock').textContent = String(Math.floor(s/60)).padStart(2,'0')+':'+String(s%60).padStart(2,'0');
  hist.push(d.power ?? 0); if(hist.length>MAXPTS) hist.shift();
  drawChart();
}

function drawChart(){
  const c = $('chart'), ctx = c.getContext('2d');
  const W = c.width, H = c.height;
  ctx.clearRect(0,0,W,H);
  const max = Math.max(50, ...hist);
  ctx.strokeStyle = '#f59e0b'; ctx.lineWidth = 2; ctx.beginPath();
  hist.forEach((v,i)=>{
    const x = i*(W/MAXPTS), y = H - (v/max)*(H-20) - 10;
    i? ctx.lineTo(x,y) : ctx.moveTo(x,y);
  });
  ctx.stroke();
  ctx.fillStyle = '#8b97a7'; ctx.font = '12px sans-serif';
  ctx.fillText('Power (W) — max ' + max.toFixed(0), 10, 16);
}
connect();
</script>
</body>
</html>
)HTMLDOC";

#endif // INDEX_HTML_H
