import { ExportQueue, frameFrom, validateCallback, validateDescriptor, validateSlots } from '../export-queue.js';

// WebGPU flags are fixed by the API. Literals also permit CPU-only module import.
const MAP_READ = 1;
const COPY_DST = 8;
const COPY_SRC = 1;

export class WebGPUExportQueue extends ExportQueue {
  constructor({ device, slots = 3 } = {}) {
    super();
    if (!device || typeof device.createBuffer !== 'function' || typeof device.createCommandEncoder !== 'function' || !device.queue) {
      throw new TypeError('device must be a GPUDevice');
    }
    this._device = device;
    this._slotCount = validateSlots(slots);
    this._slots = [];
    device.lost?.then(() => this.close(), () => this.close());
  }

  get available() { return super.available && this._slots.some(slot => !slot.pending); }

  _releaseSlots() {
    for (const slot of this._slots) {
      slot.pending = null;
      slot.buffer.destroy();
    }
    this._slots = [];
  }

  configure(descriptor) {
    if (this._closed) return;
    const next = validateDescriptor(descriptor);
    const bytesPerRow = Math.ceil(next.width * 4 / 256) * 256;
    const size = bytesPerRow * next.height;
    if (!Number.isSafeInteger(size) || size > this._device.limits.maxBufferSize) throw new RangeError('readback size exceeds the GPU buffer limit');
    this._generation += 1;
    this._descriptor = null;
    this._releaseSlots();
    try {
      for (let i = 0; i < this._slotCount; i += 1) {
        const output = new Uint8Array(next.width * next.height * 4);
        const buffer = this._device.createBuffer({size, usage:MAP_READ | COPY_DST});
        this._slots.push({buffer, bytesPerRow, pending:null, ready:false, error:null, output});
      }
      this._descriptor = next;
    } catch (error) { this._releaseSlots(); throw error; }
  }

  enqueue(source, timestamp, onFrame, sequence) {
    if (!this.available) return false;
    validateCallback(onFrame);
    const descriptor = this._descriptor;
    if (!source || source.width !== descriptor.width || source.height !== descriptor.height) throw new RangeError('texture dimensions must equal descriptor dimensions');
    if (source.dimension !== '2d' || source.depthOrArrayLayers !== 1 || source.sampleCount !== 1) throw new RangeError('texture must be a single 2D image without multisampling');
    if (!(source.usage & COPY_SRC)) throw new RangeError('texture usage must include COPY_SRC');
    if (!['rgba8unorm','rgba8unorm-srgb','bgra8unorm','bgra8unorm-srgb'].includes(source.format)) throw new RangeError('texture format must contain four 8-bit RGBA or BGRA channels');
    const slot = this._slots.find(value => !value.pending);
    const pending = {descriptor, timestamp, onFrame, sequence, generation:this._generation, bgra:source.format.startsWith('bgra')};
    slot.pending = pending;
    slot.ready = false;
    slot.error = null;
    let scopeOpen = false;
    try {
      this._device.pushErrorScope('validation');
      scopeOpen = true;
      const encoder = this._device.createCommandEncoder();
      encoder.copyTextureToBuffer({texture:source}, {buffer:slot.buffer, bytesPerRow:slot.bytesPerRow, rowsPerImage:descriptor.height}, {width:descriptor.width, height:descriptor.height, depthOrArrayLayers:1});
      this._device.queue.submit([encoder.finish()]);
      const validation = this._device.popErrorScope();
      scopeOpen = false;
      let mapped = false;
      let checked = false;
      const current = () => !this._closed && pending.generation === this._generation && slot.pending === pending;
      validation.then(error => {
        if (!current()) return;
        if (error) slot.error = new Error(error.message || 'WebGPU copy validation failed');
        checked = true;
        slot.ready = mapped;
      }, error => {
        if (!current()) return;
        slot.error = error;
        checked = true;
        slot.ready = mapped;
      });
      slot.buffer.mapAsync(MAP_READ).then(() => {
        if (!current()) {
          slot.buffer.unmap();
          return;
        }
        mapped = true;
        slot.ready = checked;
      }, error => {
        if (current()) {
          slot.error = error;
          mapped = true;
          slot.ready = checked;
        }
      });
    } catch (error) {
      if (scopeOpen) this._device.popErrorScope().catch(() => {});
      slot.pending = null;
      slot.buffer.unmap();
      throw error;
    }
    return true;
  }

  poll() {
    if (this._closed || this._busy || !this._descriptor) return;
    for (const slot of [...this._slots]) {
      if (!slot.pending || !slot.ready) continue;
      const pending = slot.pending;
      slot.pending = null;
      slot.ready = false;
      const error = slot.error;
      slot.error = null;
      let output;
      try {
        if (error) throw error;
        const raw = new Uint8Array(slot.buffer.getMappedRange());
        const {width,height} = pending.descriptor;
        const stride = width * 4;
        output = slot.output;
        for (let row = 0; row < height; row += 1) output.set(raw.subarray(row * slot.bytesPerRow, row * slot.bytesPerRow + stride), row * stride);
        if (pending.bgra) {
          for (let i = 0; i < output.length; i += 4) [output[i], output[i + 2]] = [output[i + 2], output[i]];
        }
      } finally { slot.buffer.unmap(); }
      if (!this._closed && pending.generation === this._generation) {
        this._busy = true;
        try { pending.onFrame(frameFrom(pending.descriptor, output), pending.timestamp, pending.sequence); }
        finally { this._busy = false; }
      }
    }
  }

  close(options) {
    super.close(options);
    this._releaseSlots();
  }
}
