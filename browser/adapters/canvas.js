import { ExportQueue, frameFrom, validateCallback, validateDescriptor } from '../export-queue.js';

export class CanvasExportQueue extends ExportQueue {
  constructor({ canvas } = {}) {
    super();
    if (!canvas || !Number.isSafeInteger(canvas.width) || !Number.isSafeInteger(canvas.height)) {
      throw new TypeError('canvas must expose integer width and height');
    }
    this._canvas = canvas;
    this._scratch = null;
    this._context = null;
  }

  configure(descriptor) {
    if (this._closed) return;
    const next = validateDescriptor(descriptor);
    this._descriptor = null;
    this._generation += 1;
    if (this._scratch) { this._scratch.width = 0; this._scratch.height = 0; }
    this._scratch = null;
    this._context = null;
    const { width, height, colorSpace } = next;
    this._scratch = typeof globalThis.OffscreenCanvas === 'function'
      ? new globalThis.OffscreenCanvas(width, height)
      : this._canvas.ownerDocument?.createElement('canvas');
    if (!this._scratch) {
      this._descriptor = null;
      throw new Error('a separate 2D canvas is unavailable');
    }
    this._scratch.width = width;
    this._scratch.height = height;
    this._context = this._scratch.getContext('2d', { colorSpace, willReadFrequently: true, alpha: true });
    if (!this._context) {
      this._descriptor = null;
      throw new Error('the 2D canvas context is unavailable');
    }
    this._descriptor = next;
    this._output = new Uint8Array(width * height * 4);
  }

  enqueue(_source, timestamp, onFrame, sequence) {
    if (!this.available) return false;
    validateCallback(onFrame);
    const descriptor = this._descriptor;
    const { width, height, colorSpace, alphaMode } = descriptor;
    if (this._canvas.width !== width || this._canvas.height !== height) throw new RangeError('canvas dimensions must equal descriptor dimensions');
    this._busy = true;
    try {
      this._context.clearRect(0, 0, width, height);
      this._context.drawImage(this._canvas, 0, 0);
      const pixels = this._context.getImageData(0, 0, width, height, { colorSpace, pixelFormat: 'rgba-unorm8' });
      if (pixels.colorSpace !== colorSpace) throw new Error('the canvas cannot produce the requested color space');
      const output = this._output;
      output.set(pixels.data);
      for (let i = 0; i < output.length; i += 4) {
        if (alphaMode === 'premultiplied') {
          const alpha = output[i + 3] / 255;
          output[i] = Math.round(output[i] * alpha);
          output[i + 1] = Math.round(output[i + 1] * alpha);
          output[i + 2] = Math.round(output[i + 2] * alpha);
        } else if (alphaMode === 'opaque') output[i + 3] = 255;
      }
      onFrame(frameFrom(descriptor, output), timestamp, sequence);
    } finally { this._busy = false; }
    return true;
  }

  close(options) {
    super.close(options);
    if (this._scratch) { this._scratch.width = 0; this._scratch.height = 0; }
    this._scratch = null;
    this._context = null;
    this._canvas = null;
    this._output = null;
  }
}
