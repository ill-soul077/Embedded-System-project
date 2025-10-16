// Minimal Express server that reads from Firebase Realtime Database
// Uses firebase-admin with service account at project root: service.json
// Serves API: GET /api/street (raw + computed fields)
// Serves static UI at /

const express = require('express');
const path = require('path');
const fs = require('fs');
const admin = require('firebase-admin');

// Ensure service.json exists
const serviceAccountPath = path.join(__dirname, 'service.json');
if (!fs.existsSync(serviceAccountPath)) {
  console.error('Missing service.json in project root. Place your Firebase service account JSON as ./service.json');
  process.exit(1);
}

// Initialize Firebase Admin SDK
const serviceAccount = require(serviceAccountPath);
admin.initializeApp({
  credential: admin.credential.cert(serviceAccount),
  databaseURL: 'https://embedded-be95a-default-rtdb.asia-southeast1.firebasedatabase.app/'
});

const db = admin.database();
const app = express();

// Serve static files from ./public
app.use(express.static(path.join(__dirname, 'public')));

// Helper to safely get an array and enforce 4 items for relays
function normalizeRelays(relays) {
  const base = Array.isArray(relays) ? relays.slice(0, 4) : [];
  while (base.length < 4) base.push(false);
  return base;
}

// GET /api/street => returns raw /street object + computed fields
app.get('/api/street', async (req, res) => {
  try {
    const snapshot = await db.ref('/street').once('value');
    const street = snapshot.val() || {};

    const relays4 = normalizeRelays(street.relays);
    const relays_status = relays4.map((b) => (b ? 'ON' : 'OFF'));
    const lights_on_count = relays4.filter(Boolean).length;

    // Prefer speeds_m_s, fallback to speeds
    const speeds_m_s = Array.isArray(street.speeds_m_s)
      ? street.speeds_m_s
      : Array.isArray(street.speeds)
        ? street.speeds
        : [];
    const speeds_kmh = speeds_m_s.map((s) => Number((s * 3.6).toFixed(3)));

    const timestamp_ms = street.timestamp_ms;
    const timestamp_iso = typeof timestamp_ms === 'number' ? new Date(timestamp_ms).toISOString() : null;

    res.json({
      // raw fields spread first
      ...street,
      // computed fields
      relays_status,
      lights_on_count,
      speeds_kmh,
      timestamp_iso,
    });
  } catch (err) {
    console.error('Error fetching /street from RTDB:', err);
    res.status(500).json({ error: 'Failed to fetch street data' });
  }
});

const PORT = process.env.PORT || 3000;
app.listen(PORT, () => {
  console.log(`Server listening on http://localhost:${PORT}`);
});