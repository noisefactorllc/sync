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
      let script = fs.readFileSync(path.join(linux, 'postrm'), 'utf8')
      for (const [destination, template] of managed) {
        const fullPath = path.join(directory, destination)
        fs.mkdirSync(path.dirname(fullPath), { recursive: true })
        fs.writeFileSync(fullPath, fs.readFileSync(path.join(linux, template)))
        const shellPath = process.platform === 'win32'
          ? fullPath.replace(/^([A-Za-z]):/, (_, drive) => `/${drive.toLowerCase()}`).replaceAll('\\', '/')
          : fullPath
        script = script.replaceAll(`/etc/${destination}`, shellPath)
      }
      const udevRule = path.join(directory, managed[0][0])
      if (modifiedRule) fs.appendFileSync(udevRule, '# local change\n')
      const scriptPath = path.join(directory, 'postrm')
      fs.writeFileSync(scriptPath, script)
      const result = spawnSync('sh', [scriptPath, 'purge'], { encoding: 'utf8' })
      assert.equal(result.status, 0, result.stderr)
      assert.equal(fs.existsSync(udevRule), modifiedRule)
      for (const [destination] of managed.slice(1)) {
        assert.equal(fs.existsSync(path.join(directory, destination)), false)
      }
      if (modifiedRule) assert.match(result.stderr, /preserving modified .*70-noisedeck-sync-camera\.rules/)
    } finally {
      fs.rmSync(directory, { recursive: true, force: true })
    }
  })
}
