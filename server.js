const http = require('http');
const fs = require('fs');

function getIndexHtml() {
  const code = fs.readFileSync('BroccoliBiosphereCabin.ino', 'utf8');
  const startMarker = 'const char INDEX_HTML[] PROGMEM = R"rawliteral(';
  const endMarker = ')rawliteral";';
  const startIdx = code.indexOf(startMarker);
  const endIdx = code.indexOf(endMarker);

  if (startIdx === -1 || endIdx === -1) {
    throw new Error('Failed to locate INDEX_HTML markers in BroccoliBiosphereCabin.ino');
  }
  return code.substring(startIdx + startMarker.length, endIdx);
}

let state = {
  ip: "127.0.0.1:8080",
  uptimeSec: 540,
  status: "OPTIMAL",
  autoMode: true,
  phase: 2,
  day: 5,
  activeTank: 1,
  temp: 20.3,
  rh: 58.0,
  soil: 56.4,
  vpd: 0.62,
  pcs: 97.2,
  tank1Vol: 885.0,
  tank2Vol: 920.0,
  relays: {
    peltier: false,
    fan: false,
    blower: false,
    light: true,
    spray1: false,
    spray2: false
  },
  valves: {
    v1: 0,
    v2: 0
  },
  history: {
    temp: [20.1, 20.2, 20.1, 20.3, 20.5, 20.4, 20.3, 20.5, 20.3, 20.2, 20.4, 20.3, 20.3, 20.4, 20.5, 20.4, 20.3, 20.4, 20.3, 20.3],
    rh: [58.0, 58.5, 59.0, 58.8, 57.9, 58.1, 58.3, 58.0, 57.8, 58.2, 58.4, 58.1, 58.0, 57.9, 58.2, 58.0, 58.3, 58.1, 58.0, 58.0]
  },
  logs: [
    { t: "00:09:00", m: "Brocco Maskot Berjalan Aktif di Port 8080 🌸" },
    { t: "00:07:30", m: "Kondisi Prima: Suhu 20.3°C, RH 58.0%, VPD 0.62 kPa ✨" },
    { t: "00:05:42", m: "Siklus Otomatis Aktif - Skor Kenyamanan 97.2%" },
    { t: "00:03:15", m: "Fotoperiode Aktif (LED Grow Light ON) - Hari ke-5" },
    { t: "00:00:00", m: "Inisialisasi Pengontrol Kabin Biosfer Brokoli..." }
  ]
};

const server = http.createServer((req, res) => {
  const url = new URL(req.url, `http://${req.headers.host}`);

  if (url.pathname === '/' || url.pathname === '/index.html') {
    try {
      const html = getIndexHtml();
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      res.end(html);
    } catch (err) {
      res.writeHead(500, { 'Content-Type': 'text/plain' });
      res.end('Error loading dashboard: ' + err.message);
    }
  } else if (url.pathname === '/api/telemetry') {
    state.uptimeSec += 1;
    const h = String(Math.floor(state.uptimeSec / 3600)).padStart(2, '0');
    const m = String(Math.floor((state.uptimeSec % 3600) / 60)).padStart(2, '0');
    const s = String(state.uptimeSec % 60).padStart(2, '0');

    // Natural gentle breathing variation in temperature and humidity
    state.temp = +(20.3 + Math.sin(state.uptimeSec * 0.15) * 0.25).toFixed(1);
    state.rh = +(58.0 + Math.cos(state.uptimeSec * 0.15) * 0.4).toFixed(1);

    if (state.uptimeSec % 3 === 0) {
      state.history.temp.shift();
      state.history.temp.push(state.temp);
      state.history.rh.shift();
      state.history.rh.push(state.rh);
    }

    const payload = {
      ip: state.ip,
      uptime: `${h}:${m}:${s}`,
      status: state.status,
      autoMode: state.autoMode,
      phase: state.phase,
      day: state.day,
      activeTank: state.activeTank,
      temp: state.temp,
      rh: state.rh,
      soil: state.soil,
      vpd: state.vpd,
      pcs: state.pcs,
      tank1Vol: state.tank1Vol,
      tank2Vol: state.tank2Vol,
      relays: state.relays,
      valves: state.valves,
      history: state.history,
      logs: state.logs
    };

    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(payload));
  } else if (url.pathname === '/api/control') {
    const action = url.searchParams.get('action');
    const nowTs = new Date().toTimeString().split(' ')[0];

    if (action === 'flush') {
      state.relays.spray1 = true;
      state.valves.v1 = 90;
      state.tank1Vol = Math.max(0, +(state.tank1Vol - 4.0).toFixed(1));
      state.logs.unshift({ t: nowTs, m: "Aksi Pengasuhan: Beri Minum (Semprot Air Baku Tangki 1 5 Detik)" });
      setTimeout(() => {
        state.relays.spray1 = false;
        state.valves.v1 = 0;
      }, 5000);
    } else if (action === 'toggleRelay') {
      const ch = url.searchParams.get('ch');
      if (ch === '6') {
        state.relays.spray2 = true;
        state.valves.v2 = 90;
        state.tank2Vol = Math.max(0, +(state.tank2Vol - 4.0).toFixed(1));
        state.logs.unshift({ t: nowTs, m: "Aksi Pengasuhan: Beri Vitamin (Nutrisi Mikro Tangki 2 Aktif)" });
        setTimeout(() => {
          state.relays.spray2 = false;
          state.valves.v2 = 0;
        }, 5000);
      } else if (ch === '3') {
        state.relays.blower = !state.relays.blower;
        state.logs.unshift({ t: nowTs, m: `Aksi Pengasuhan: Kipas Semilir (${state.relays.blower ? 'Nyala' : 'Mati'})` });
      } else if (ch === '1') {
        state.relays.peltier = !state.relays.peltier;
        if (state.relays.peltier) state.relays.fan = true;
        state.logs.unshift({ t: nowTs, m: `Manual: Peltier (${state.relays.peltier ? 'Nyala' : 'Mati'})` });
      } else if (ch === '2') {
        state.relays.fan = !state.relays.fan;
        if (!state.relays.fan) state.relays.peltier = false;
        state.logs.unshift({ t: nowTs, m: `Manual: Kipas Heatsink (${state.relays.fan ? 'Nyala' : 'Mati'})` });
      } else if (ch === '4') {
        state.relays.light = !state.relays.light;
        state.logs.unshift({ t: nowTs, m: `Manual: LED Grow Light (${state.relays.light ? 'Nyala' : 'Mati'})` });
      } else if (ch === '5') {
        state.relays.spray1 = !state.relays.spray1;
        state.valves.v1 = state.relays.spray1 ? 90 : 0;
      }
    } else if (action === 'mode') {
      state.autoMode = (url.searchParams.get('val') === 'auto');
      state.logs.unshift({ t: nowTs, m: `Mode Operasi Diubah: ${state.autoMode ? 'Otomatis' : 'Manual'}` });
    } else if (action === 'phase') {
      state.phase = parseInt(url.searchParams.get('val'), 10) || 1;
      state.logs.unshift({ t: nowTs, m: `Fase Diubah: ${state.phase === 1 ? 'Perkecambahan' : 'Autotrofik'}` });
    } else if (action === 'day') {
      state.day = parseInt(url.searchParams.get('val'), 10) || 1;
      state.logs.unshift({ t: nowTs, m: `Penyetel Hari: Hari ke-${state.day}` });
    } else if (action === 'refill') {
      const tank = url.searchParams.get('tank');
      if (tank === '1') {
        state.tank1Vol = 1000.0;
        state.logs.unshift({ t: nowTs, m: "Tangki 1 (Air Baku) Diisi Ulang ke 1000 mL" });
      }
      if (tank === '2') {
        state.tank2Vol = 1000.0;
        state.logs.unshift({ t: nowTs, m: "Tangki 2 (Nutrisi Mikro) Diisi Ulang ke 1000 mL" });
      }
    }

    if (state.logs.length > 10) state.logs.pop();

    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ success: true }));
  } else {
    res.writeHead(404);
    res.end('Not Found');
  }
});

server.listen(8080, '127.0.0.1', () => {
  console.log('Kebun Biosfer Web Server running at http://127.0.0.1:8080/');
});
