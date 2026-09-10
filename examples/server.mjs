import { realpath, readFile, stat } from 'node:fs/promises';
import { createServer } from 'node:http';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const EXAMPLES_ROOT = path.dirname(fileURLToPath(import.meta.url));
const REPOSITORY_ROOT = path.dirname(EXAMPLES_ROOT);
const BROWSER_ROOT = path.join(REPOSITORY_ROOT, 'browser');

const CONTENT_TYPES = new Map([
  ['.css', 'text/css; charset=utf-8'],
  ['.html', 'text/html; charset=utf-8'],
  ['.js', 'text/javascript; charset=utf-8'],
  ['.json', 'application/json; charset=utf-8'],
  ['.mjs', 'text/javascript; charset=utf-8'],
  ['.svg', 'image/svg+xml'],
]);

function validLoopbackHost(host) {
  if (host === '::1') return true;
  const parts = host.split('.');
  return parts.length === 4 && Number(parts[0]) === 127 && parts.every((part) =>
    /^\d{1,3}$/.test(part) && Number(part) <= 255);
}

function validatePort(port) {
  const value = typeof port === 'string' && /^\d+$/.test(port) ? Number(port) : port;
  if (!Number.isInteger(value) || value < 0 || value > 65_535) {
    throw new RangeError('port must be an integer from 0 to 65535');
  }
  return value;
}

function within(root, candidate) {
  const relative = path.relative(root, candidate);
  return relative === '' || (!relative.startsWith(`..${path.sep}`) && relative !== '..' && !path.isAbsolute(relative));
}

function requestedFile(rawUrl) {
  const rawPath = rawUrl.split('?', 1)[0];
  let pathname;
  try {
    pathname = decodeURIComponent(rawPath);
  } catch {
    return null;
  }
  if (pathname.includes('\0') || pathname.includes('\\')) return null;
  if (pathname.split('/').some((part) => part === '.' || part === '..')) return null;
  if (pathname === '/') return { root: EXAMPLES_ROOT, file: 'index.html' };
  if (pathname.startsWith('/browser/')) {
    return { root: BROWSER_ROOT, file: pathname.slice('/browser/'.length) };
  }
  return { root: EXAMPLES_ROOT, file: pathname.slice(1) };
}

async function safeFile(rawUrl) {
  const requested = requestedFile(rawUrl);
  if (!requested || requested.file.length === 0) return null;
  const root = await realpath(requested.root);
  const lexical = path.resolve(root, requested.file);
  if (!within(root, lexical)) return null;
  let resolved;
  try {
    resolved = await realpath(lexical);
    if (!within(root, resolved) || !(await stat(resolved)).isFile()) return null;
  } catch {
    return null;
  }
  return resolved;
}

function reply(response, status, body = '') {
  response.writeHead(status, {
    'Content-Type': 'text/plain; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Permissions-Policy': 'loopback-network=(self)',
    'X-Content-Type-Options': 'nosniff',
    'Cache-Control': 'no-store',
  });
  response.end(body);
}

export async function startExampleServer({ port = 4179, host = '127.0.0.1' } = {}) {
  if (typeof host !== 'string' || !validLoopbackHost(host)) {
    throw new TypeError('host must be an IPv4 or IPv6 loopback literal');
  }
  const selectedPort = validatePort(port);
  const server = createServer(async (request, response) => {
    try {
      if (request.method !== 'GET' && request.method !== 'HEAD') {
        response.setHeader('Allow', 'GET, HEAD');
        reply(response, 405, 'Method not allowed\n');
        return;
      }
      const file = await safeFile(request.url ?? '/');
      if (!file) {
        reply(response, 404, 'Not found\n');
        return;
      }
      const body = await readFile(file);
      response.writeHead(200, {
        'Content-Type': CONTENT_TYPES.get(path.extname(file)) ?? 'application/octet-stream',
        'Content-Length': body.byteLength,
        'Permissions-Policy': 'loopback-network=(self)',
        'X-Content-Type-Options': 'nosniff',
        'Cache-Control': 'no-store',
      });
      response.end(request.method === 'HEAD' ? undefined : body);
    } catch {
      reply(response, 500, 'Internal server error\n');
    }
  });

  await new Promise((resolve, reject) => {
    const failed = (error) => {
      server.removeListener('listening', ready);
      reject(error);
    };
    const ready = () => {
      server.removeListener('error', failed);
      resolve();
    };
    server.once('error', failed);
    server.once('listening', ready);
    server.listen(selectedPort, host);
  });
  return server;
}

async function main() {
  const server = await startExampleServer({ port: process.env.PORT ?? 4179 });
  const address = server.address();
  const host = address.address.includes(':') ? `[${address.address}]` : address.address;
  process.stdout.write(`http://${host}:${address.port}/\n`);

  const close = () => server.close(() => process.exit(0));
  process.once('SIGINT', close);
  process.once('SIGTERM', close);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((error) => {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  });
}
