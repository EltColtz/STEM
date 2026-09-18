# Kebun Biosfer — Broccoli Biosphere Cabin 🥦🌱

Automated climate-controlled plant chamber firmware and interactive web laboratory dashboard.

## Overview

This project provides an automated biosphere cabin for growing plants (such as broccoli) with sensory feedback, environmental control, and a gamified laboratory companion dashboard.

- **`BroccoliBiosphereCabin.ino`**: ESP32 Arduino sketch controlling GPIO relays (pumps, lights, ventilation, misting), analog soil sensors, temperature/humidity diagnostics, thermal interlock, and embedding the responsive single-page web application.
- **`server.js`**: Standalone Node.js simulation server replicating ESP32 sensor state and relay API endpoints for quick local development and UI testing without physical hardware.

---

## Getting Started

### Prerequisites
- [Node.js](https://nodejs.org/) (v16+ recommended, no external dependencies needed)
- [Arduino IDE](https://www.arduino.cc/en/software) with ESP32 board support (for hardware flashing)

### Running the Web Dashboard (Local Dev)
1. Start the simulation server:
   ```bash
   node server.js
   ```
2. Open your browser to:
   ```
   http://127.0.0.1:8080/
   ```

---

## Collaborating on this Project

1. **Clone the repository**:
   ```bash
   git clone https://github.com/akorite/STEM.git
   cd STEM
   ```
2. **Create a feature branch**:
   ```bash
   git checkout -b feature/your-feature-name
   ```
3. **Commit your changes**:
   ```bash
   git commit -m "feat: description of change"
   ```
4. **Push and open a Pull Request**:
   ```bash
   git push origin feature/your-feature-name
   ```
