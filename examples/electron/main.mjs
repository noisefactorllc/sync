import { app, BrowserWindow, net, protocol, session } from 'electron';
import { realpath } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const APP_ORIGIN = 'app://com.example.visualizer';
const EXAMPLES_ROOT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const BROWSER_ROOT = path.join(path.dirname(EXAMPLES_ROOT), 'browser');
const APP_PERMISSIONS = new Set([
  'clipboard-sanitized-write',
  'local-network-access',
  'loopback-network',
]);

protocol.registerSchemesAsPrivileged([{
  scheme: 'app',
  privileges: {
    standard: true,
    secure: true,
    supportFetchAPI: true,
    corsEnabled: true,
  },
}]);

function within(root, candidate) {
  const relative = path.relative(root, candidate);
  return relative === '' || (!relative.startsWith(`..${path.sep}`) && relative !== '..' && !path.isAbsolute(relative));
}

function isAppOrigin(value) {
  try {
    const url = new URL(value);
    return url.protocol === 'app:' && url.hostname === 'com.example.visualizer' &&
      !url.port && !url.username && !url.password;
  } catch {
    return false;
  }
}

async function appFile(requestUrl) {
  const url = new URL(requestUrl);
  if (url.protocol !== 'app:' || url.hostname !== 'com.example.visualizer' ||
      url.port || url.username || url.password) return null;
  const decoded = decodeURIComponent(url.pathname);
  if (decoded.includes('\\') || decoded.split('/').some((part) => part === '.' || part === '..')) return null;
  const selected = decoded === '/'
    ? { root: EXAMPLES_ROOT, file: 'index.html' }
    : decoded.startsWith('/browser/')
      ? { root: BROWSER_ROOT, file: decoded.slice('/browser/'.length) }
      : { root: EXAMPLES_ROOT, file: decoded.slice(1) };
  const root = await realpath(selected.root);
  const file = await realpath(path.resolve(root, selected.file));
  return within(root, file) ? file : null;
}

async function createWindow() {
  const window = new BrowserWindow({
    width: 1120,
    height: 820,
    backgroundColor: '#11110f',
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });
  await window.loadURL(`${APP_ORIGIN}/`);
}

app.whenReady().then(async () => {
  session.defaultSession.setPermissionCheckHandler((_contents, permission, requestingOrigin) =>
    isAppOrigin(requestingOrigin) && APP_PERMISSIONS.has(permission));
  session.defaultSession.setPermissionRequestHandler((_contents, permission, callback, details) => {
    callback(isAppOrigin(details.requestingUrl) && APP_PERMISSIONS.has(permission));
  });
  protocol.handle('app', async (request) => {
    try {
      const file = await appFile(request.url);
      return file ? net.fetch(pathToFileURL(file).toString()) : new Response('Not found\n', { status: 404 });
    } catch {
      return new Response('Not found\n', { status: 404 });
    }
  });
  await createWindow();
  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) void createWindow();
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
