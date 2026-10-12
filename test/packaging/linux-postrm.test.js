import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import test from 'node:test'
import { fileURLToPath } from 'node:url'

const root = fileURLToPath(new URL('../..', import.meta.url))
const linux = path.join(root, 'packaging/linux')
const managed = [
  ['udev/rules.d/70-noisedeck-sync-camera.rules', '70-noisedeck-sync-camera.rules'],
  ['modprobe.d/noisedeck-sync-camera.conf', 'noisedeck-sync-camera.modprobe'],
  ['modules-load.d/noisedeck-sync-camera.conf', 'noisedeck-sync-camera.modules-load'],
]

for (const modifiedRule of [false, true]) {
  test(`purge ${modifiedRule ? 'preserves a modified' : 'removes an unchanged'} camera rule`, () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'sync-postrm-'))
    try {
      // A Windows checkout materializes these files with CRLF line endings.
      // Normalize both sides so the script's heredoc and the fixture bytes
      // compare equal on every platform.
      let script = fs.readFileSync(path.join(linux, 'postrm'), 'utf8').replaceAll('\r\n', '\n')
      for (const [destination, template] of managed) {
        const fullPath = path.join(directory, destination)
        fs.mkdirSync(path.dirname(fullPath), { recursive: true })
        fs.writeFileSync(fullPath, fs.readFileSync(path.join(linux, template)).toString('utf8').replaceAll('\r\n', '\n'))
        script = script.replaceAll(`/etc/${destination}`, destination)
      }
      const udevRule = path.join(directory, managed[0][0])
      if (modifiedRule) fs.appendFileSync(udevRule, '# local change\n')
      const scriptPath = path.join(directory, 'postrm')
      fs.writeFileSync(scriptPath, script)
      const result = spawnSync('sh', [scriptPath, 'purge'], {
        cwd: directory,
        encoding: 'utf8',
      })
      assert.equal(result.status, 0, result.stderr)
      assert.equal(fs.existsSync(udevRule), modifiedRule, result.stderr)
      for (const [destination] of managed.slice(1)) {
        assert.equal(fs.existsSync(path.join(directory, destination)), false, result.stderr)
      }
      if (modifiedRule) assert.match(result.stderr, /preserving modified .*70-noisedeck-sync-camera\.rules/)
      assert.doesNotMatch(result.stderr, /preserving modified .*noisedeck-sync-camera\.conf/)
    } finally {
      fs.rmSync(directory, { recursive: true, force: true })
    }
  })
}
