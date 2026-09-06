import http from 'node:http';

// Import the small HTTP client before streaming starts. Node's first fetch()
// loads Undici synchronously, after the caller has already armed its deadline.
// On a cold runner that work can stall both the probe and the stream's timers.
export function probeHealth(port, { origin, timeoutMs = 2000 } = {}) {
  return new Promise((resolve, reject) => {
    let settled = false;
    const finish = (error) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      request.destroy();
      if (error) reject(error);
      else resolve();
    };
    const request = http.get({
      hostname: '127.0.0.1', port, path: '/health', agent: false,
      headers: origin === undefined ? {} : { Origin: origin },
    }, (response) => {
      response.on('error', finish);
      response.on('end', () => finish(response.statusCode === 200 ? null
        : new Error(`health returned HTTP ${response.statusCode}`)));
      response.resume();
    });
    request.on('error', finish);
    // An absolute deadline covers connection, headers AND body. An inactivity
    // timeout would let a peer keep the check alive by dribbling body bytes.
    const timer = setTimeout(() => finish(new Error('health request timed out')), timeoutMs);
  });
}
