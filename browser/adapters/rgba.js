import { ExportQueue, frameFrom, validateCallback } from '../export-queue.js';

export class RgbaExportQueue extends ExportQueue {
  configure(descriptor) {
    super.configure(descriptor);
    if (this._closed) return;
    this._output = new Uint8Array(this._descriptor.width * this._descriptor.height * 4);
  }

  // Admission copies the source before the callback and before enqueue returns.
  enqueue(source, timestamp, onFrame, sequence) {
    if (!this.available) return false;
    validateCallback(onFrame);
    const descriptor = this._descriptor;
    if (!source || source.width !== descriptor.width || source.height !== descriptor.height) {
      throw new RangeError('source dimensions must equal descriptor dimensions');
    }
    const { width, height, rowStride, data } = source;
    if (!Number.isSafeInteger(rowStride) || rowStride < width * 4 || rowStride > 0xffffffff) {
      throw new RangeError('rowStride must be an integer of at least width * 4');
    }
    const requiredBytes = (height - 1) * rowStride + width * 4;
    if (!Number.isSafeInteger(requiredBytes) || requiredBytes > 0xffffffff) throw new RangeError('source size exceeds protocol limits');
    if (!(data instanceof ArrayBuffer) && !ArrayBuffer.isView(data)) throw new TypeError('data must be an ArrayBuffer or a typed array');
    const bytes = data instanceof ArrayBuffer ? new Uint8Array(data) : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    if (bytes.byteLength < requiredBytes) throw new RangeError('data does not contain all source rows');
    const output = this._output;
    for (let row = 0; row < height; row += 1) {
      output.set(bytes.subarray(row * rowStride, row * rowStride + width * 4), row * width * 4);
    }
    this._busy = true;
    try { onFrame(frameFrom(descriptor, output), timestamp, sequence); }
    finally { this._busy = false; }
    return true;
  }

  close(options) {
    super.close(options);
    this._output = null;
  }
}
