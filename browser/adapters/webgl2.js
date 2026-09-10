import { ExportQueue, frameFrom, validateCallback, validateDescriptor, validateSlots } from '../export-queue.js';

export class WebGL2ExportQueue extends ExportQueue {
  constructor({ gl, slots = 3 } = {}) {
    super();
    if (!gl || typeof gl.fenceSync !== 'function' || typeof gl.getBufferSubData !== 'function') throw new TypeError('gl must be a WebGL2 context');
    this._gl = gl;
    this._slotCount = validateSlots(slots);
    this._slots = [];
  }

  get available() {
    return super.available && !this._gl.isContextLost() && this._slots.some(slot => !slot.pending);
  }

  _releaseSlots() {
    for (const slot of this._slots) {
      if (slot.sync) this._gl.deleteSync(slot.sync);
      this._gl.deleteBuffer(slot.buffer);
      slot.sync = null;
      slot.pending = null;
    }
    this._slots = [];
  }

  configure(descriptor) {
    if (this._closed) return;
    const next = validateDescriptor(descriptor);
    this._generation += 1;
    this._descriptor = null;
    this._releaseSlots();
    const gl = this._gl;
    if (gl.isContextLost()) throw new Error('the WebGL2 context is lost');
    const previous = gl.getParameter(gl.PIXEL_PACK_BUFFER_BINDING);
    try {
      for (let i = 0; i < this._slotCount; i += 1) {
        const size = next.width * next.height * 4;
        const raw = new Uint8Array(size);
        const output = new Uint8Array(size);
        const buffer = gl.createBuffer();
        if (!buffer) throw new Error('WebGL2 cannot create a readback buffer');
        this._slots.push({buffer, sync:null, pending:null, raw, output});
        gl.bindBuffer(gl.PIXEL_PACK_BUFFER, buffer);
        gl.bufferData(gl.PIXEL_PACK_BUFFER, next.width * next.height * 4, gl.STREAM_READ);
        if (typeof gl.getError === 'function' && gl.getError() !== gl.NO_ERROR) throw new Error('WebGL2 cannot allocate a readback buffer');
      }
      this._descriptor = next;
    } catch (error) {
      this._releaseSlots();
      throw error;
    } finally { gl.bindBuffer(gl.PIXEL_PACK_BUFFER, previous); }
  }

  enqueue(source, timestamp, onFrame, sequence) {
    if (!this.available) return false;
    validateCallback(onFrame);
    const gl = this._gl;
    const descriptor = this._descriptor;
    if (source === undefined) throw new TypeError('source must be a framebuffer or null');
    if (source === null && (gl.drawingBufferWidth !== descriptor.width || gl.drawingBufferHeight !== descriptor.height)) {
      throw new RangeError('drawing buffer dimensions must equal descriptor dimensions');
    }
    const slot = this._slots.find(value => !value.pending);
    const previousFramebuffer = gl.getParameter(gl.READ_FRAMEBUFFER_BINDING);
    const previousBuffer = gl.getParameter(gl.PIXEL_PACK_BUFFER_BINDING);
    const packNames = [gl.PACK_ALIGNMENT, gl.PACK_ROW_LENGTH, gl.PACK_SKIP_ROWS, gl.PACK_SKIP_PIXELS];
    const pack = packNames.map(name => gl.getParameter(name));
    let readBuffer;
    try {
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, source);
      readBuffer = gl.getParameter(gl.READ_BUFFER);
      gl.readBuffer(source === null ? gl.BACK : gl.COLOR_ATTACHMENT0);
      if (gl.checkFramebufferStatus(gl.READ_FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) throw new Error('the source framebuffer is incomplete');
      gl.bindBuffer(gl.PIXEL_PACK_BUFFER, slot.buffer);
      packNames.forEach((name, i) => gl.pixelStorei(name, i === 0 ? 1 : 0));
      gl.readPixels(0, 0, descriptor.width, descriptor.height, gl.RGBA, gl.UNSIGNED_BYTE, 0);
      if (typeof gl.getError === 'function' && gl.getError() !== gl.NO_ERROR) throw new Error('WebGL2 readback failed');
      slot.sync = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
      if (!slot.sync) throw new Error('WebGL2 cannot create a readback fence');
      slot.pending = {descriptor, timestamp, onFrame, sequence, generation:this._generation};
      gl.flush();
    } catch (error) {
      if (slot.sync) gl.deleteSync(slot.sync);
      slot.sync = null;
      slot.pending = null;
      throw error;
    } finally {
      if (readBuffer !== undefined) gl.readBuffer(readBuffer);
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, previousFramebuffer);
      gl.bindBuffer(gl.PIXEL_PACK_BUFFER, previousBuffer);
      packNames.forEach((name, i) => gl.pixelStorei(name, pack[i]));
    }
    return true;
  }

  poll() {
    if (this._closed || this._busy || !this._descriptor) return;
    const gl = this._gl;
    if (gl.isContextLost()) { this.close(); return; }
    for (const slot of [...this._slots]) {
      if (!slot.pending) continue;
      const status = gl.clientWaitSync(slot.sync, 0, 0);
      if (status === gl.TIMEOUT_EXPIRED) continue;
      const pending = slot.pending;
      slot.pending = null;
      gl.deleteSync(slot.sync);
      slot.sync = null;
      if (status !== gl.ALREADY_SIGNALED && status !== gl.CONDITION_SATISFIED) throw new Error('WebGL2 readback fence failed');
      const {width, height} = pending.descriptor;
      const raw = slot.raw;
      const previous = gl.getParameter(gl.PIXEL_PACK_BUFFER_BINDING);
      try {
        gl.bindBuffer(gl.PIXEL_PACK_BUFFER, slot.buffer);
        gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, raw);
        if (typeof gl.getError === 'function' && gl.getError() !== gl.NO_ERROR) throw new Error('WebGL2 buffer read failed');
      } finally { gl.bindBuffer(gl.PIXEL_PACK_BUFFER, previous); }
      const output = slot.output;
      const stride = width * 4;
      for (let row = 0; row < height; row += 1) output.set(raw.subarray((height - row - 1) * stride, (height - row) * stride), row * stride);
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
