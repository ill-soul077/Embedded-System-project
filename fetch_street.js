// Standalone script to fetch JSON from Firebase RTDB and print to console
// - Reads service account from ./service.json
// - Fetches /street
// - Prints raw JSON
// - Computes and prints average of speeds_m_s
// - Prints detailed error on failure

const path = require('path');
const fs = require('fs');
const admin = require('firebase-admin');

async function main() {
  try {
    const serviceAccountPath = path.join(__dirname, 'service.json');
    if (!fs.existsSync(serviceAccountPath)) {
      throw new Error('Missing service.json in project root');
    }

    const serviceAccount = require(serviceAccountPath);
    admin.initializeApp({
      credential: admin.credential.cert(serviceAccount),
      databaseURL: 'https://embedded-be95a-default-rtdb.asia-southeast1.firebasedatabase.app/'
    });

    const db = admin.database();
    const snapshot = await db.ref('/street').once('value');
    const street = snapshot.val() || {};

    // 1) Raw JSON data
    console.log('Raw /street data:');
    console.log(JSON.stringify(street, null, 2));

    // Determine speeds_m_s from either speeds_m_s or speeds
    let speeds_m_s = Array.isArray(street.speeds_m_s)
      ? street.speeds_m_s
      : Array.isArray(street.speeds)
        ? street.speeds
        : [];

    // 2) Calculated average speed
    if (speeds_m_s.length === 0) {
      console.log('Average speed (m/s): N/A (no speeds_m_s provided)');
    } else {
      const sum = speeds_m_s.reduce((acc, v) => acc + Number(v || 0), 0);
      const avg = sum / speeds_m_s.length;
      console.log(`Average speed (m/s): ${avg.toFixed(3)} from ${speeds_m_s.length} value(s)`);
    }

    // Cleanly exit
    process.exit(0);
  } catch (err) {
    // 3) Error message with details
    console.error('Failed to fetch /street data.');
    console.error('Message:', err && err.message ? err.message : String(err));
    if (err && err.stack) console.error(err.stack);
    process.exit(1);
  }
}

main();