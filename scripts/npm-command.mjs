import { execFile } from 'node:child_process';
import path from 'node:path';
import { promisify } from 'node:util';

const execute = promisify(execFile);

export function runNpm(args, options) {
  const cli = process.env.npm_execpath ?? (process.platform === 'win32'
    ? path.join(path.dirname(process.execPath), 'node_modules/npm/bin/npm-cli.js') : null);
  return cli
    ? execute(process.execPath, [cli, ...args], options)
    : execute('npm', args, options);
}
