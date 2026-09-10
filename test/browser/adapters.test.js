import test from 'node:test';
import assert from 'node:assert/strict';
import { RgbaExportQueue } from '../../browser/adapters/rgba.js';
import { CanvasExportQueue } from '../../browser/adapters/canvas.js';
import { WebGL2ExportQueue } from '../../browser/adapters/webgl2.js';
import { WebGPUExportQueue } from '../../browser/adapters/webgpu.js';

const descriptor = { width: 1, height: 2, format: 'rgba8unorm', colorSpace: 'srgb', alphaMode: 'straight', fps: 60 };
const topDown = [255, 0, 0, 255, 0, 0, 255, 128];

test('RGBA admission is synchronous and copies only active bytes from padded rows', () => {
  const queue = new RgbaExportQueue();
  const source = { width: 1, height: 2, rowStride: 8, data: new Uint8Array([255,0,0,255,99,99,99,99,0,0,255,128]) };
  assert.equal(queue.available, false);
  assert.equal(queue.enqueue(source, 10, () => assert.fail(), 1), false);
  queue.configure(descriptor);
  let frame;
  assert.equal(queue.enqueue(source, 10, (value, timestamp, sequence) => {
    frame = value;
    assert.equal(timestamp, 10);
    assert.equal(sequence, 1);
    assert.equal(queue.available, false);
    assert.equal(queue.enqueue(source, 11, () => assert.fail(), 2), false);
  }, 1), true);
  assert.deepEqual([...frame.data], topDown);
  assert.equal(frame.rowStride, 4);
  assert.equal(frame.colorSpace, 'srgb');
  assert.equal(frame.alphaMode, 'straight');
  source.data.fill(0);
  assert.deepEqual([...frame.data], topDown);
  assert.equal(queue.available, true);
  queue.close();
  queue.configure(descriptor);
  assert.equal(queue.available, false);
});

test('RGBA rejects invalid bounds, metadata, and frame bytes before admission', () => {
  const queue = new RgbaExportQueue();
  for (const override of [{width:0}, {height:1.5}, {width:0xffffffff}, {format:'bgra8unorm'}, {colorSpace:'linear'}, {alphaMode:'unknown'}, {fps:Infinity}]) {
    assert.throws(() => queue.configure({...descriptor, ...override}));
  }
  queue.configure(descriptor);
  for (const override of [{width:2}, {height:3}, {rowStride:3}, {rowStride:4.5}, {data:new Uint8Array(7)}, {data:[]}]) {
    assert.throws(() => queue.enqueue({width:1,height:2,rowStride:4,data:new Uint8Array(8),...override}, 1, () => assert.fail(), 1));
  }
  assert.throws(() => queue.enqueue({width:1,height:2,rowStride:4,data:new Uint8Array(8)}, 1, () => {throw Error('callback');}, 1), /callback/);
  assert.equal(queue.available, true);
});

function canvasFixture() {
  const calls = [];
  const ctx = {
    clearRect() {},
    drawImage(source, ...rest) { calls.push(['draw', source, ...rest]); },
    getImageData(x,y,w,h, options) {
      calls.push(['read', options]);
      return {data:new Uint8ClampedArray(topDown), colorSpace:'srgb'};
    },
  };
  const scratch = {width:0,height:0,getContext(type,options){calls.push(['context',type,options]);return ctx;}};
  const canvas = {width:1,height:2,ownerDocument:{createElement(type){assert.equal(type,'canvas');return scratch;}}};
  return {canvas,calls,scratch,ctx};
}

test('Canvas reads its source synchronously and applies explicit alpha metadata', () => {
  const {canvas, calls} = canvasFixture();
  const queue = new CanvasExportQueue({canvas});
  queue.configure({...descriptor, alphaMode:'premultiplied'});
  let frame;
  assert.equal(queue.enqueue(null, 12, (value) => {frame=value;}, 1), true);
  assert.deepEqual([...frame.data], [255,0,0,255,0,0,128,128]);
  assert.equal(calls.find(call => call[0] === 'draw')[1], canvas);
  assert.equal(frame.alphaMode, 'premultiplied');
  canvas.width = 2;
  assert.throws(() => queue.enqueue(null, 13, () => assert.fail(), 2), /dimensions/);
  queue.close();
  assert.equal(queue.available, false);
});

// Node does not expose browser GPU APIs. These doubles model resources and state.
// The browser acceptance suite checks the same queues against actual GPU pixels.
function glFixture() {
  const gl = {};
  ['READ_FRAMEBUFFER','READ_FRAMEBUFFER_BINDING','PIXEL_PACK_BUFFER','PIXEL_PACK_BUFFER_BINDING','PACK_ALIGNMENT','PACK_ROW_LENGTH','PACK_SKIP_ROWS','PACK_SKIP_PIXELS','READ_BUFFER','BACK','COLOR_ATTACHMENT0','FRAMEBUFFER_COMPLETE','STREAM_READ','RGBA','UNSIGNED_BYTE','SYNC_GPU_COMMANDS_COMPLETE','TIMEOUT_EXPIRED','ALREADY_SIGNALED','CONDITION_SATISFIED','WAIT_FAILED'].forEach((name,i) => {gl[name]=i+1;});
  const state = new Map([[gl.READ_FRAMEBUFFER_BINDING,{name:'previous'}],[gl.PIXEL_PACK_BUFFER_BINDING,{name:'previous-buffer'}],[gl.PACK_ALIGNMENT,8],[gl.PACK_ROW_LENGTH,10],[gl.PACK_SKIP_ROWS,2],[gl.PACK_SKIP_PIXELS,3]]);
  const buffers=[],syncs=[], readModes = new Map();
  gl.drawingBufferWidth=1;gl.drawingBufferHeight=2;
  gl.isContextLost=()=>false;
  gl.getParameter=(name)=>name===gl.READ_BUFFER ? (readModes.get(state.get(gl.READ_FRAMEBUFFER_BINDING)) ?? gl.BACK) : state.get(name);
  gl.bindFramebuffer=(target,value)=>{assert.equal(target,gl.READ_FRAMEBUFFER);state.set(gl.READ_FRAMEBUFFER_BINDING,value);};
  gl.bindBuffer=(target,value)=>{assert.equal(target,gl.PIXEL_PACK_BUFFER);state.set(gl.PIXEL_PACK_BUFFER_BINDING,value);};
  gl.pixelStorei=(name,value)=>state.set(name,value);
  gl.readBuffer=value=>readModes.set(state.get(gl.READ_FRAMEBUFFER_BINDING),value);
  gl.checkFramebufferStatus=()=>gl.FRAMEBUFFER_COMPLETE;
  gl.createBuffer=()=>{const value={};buffers.push(value);return value;};
  gl.bufferData=(target,size)=>{state.get(gl.PIXEL_PACK_BUFFER_BINDING).bytes=new Uint8Array(size);};
  gl.readPixels=(x,y,w,h,format,type,offset)=>{
    assert.equal(offset,0);
    assert.equal(state.get(gl.PACK_ROW_LENGTH),0);
    assert.equal(state.get(gl.PACK_SKIP_ROWS),0);
    assert.equal(state.get(gl.PACK_SKIP_PIXELS),0);
    state.get(gl.PIXEL_PACK_BUFFER_BINDING).bytes.set([0,0,255,128,255,0,0,255]);
  };
  gl.fenceSync=()=>{const sync={ready:false};syncs.push(sync);return sync;};
  gl.clientWaitSync=(sync,flags,timeout)=>{assert.equal(timeout,0);return sync.ready?gl.CONDITION_SATISFIED:gl.TIMEOUT_EXPIRED;};
  gl.getBufferSubData=(target,offset,data)=>data.set(state.get(gl.PIXEL_PACK_BUFFER_BINDING).bytes);
  gl.flush=()=>{};
  gl.deleteBuffer=buffer=>{buffer.deleted=true;};
  gl.deleteSync=sync=>{sync.deleted=true;};
  return {gl,state,buffers,syncs,readModes};
}

test('WebGL2 bounds slots, restores host state, and produces top-down bytes after a fence', () => {
  const {gl,state,buffers,syncs,readModes}=glFixture();
  const initial = new Map(state);
  const queue = new WebGL2ExportQueue({gl,slots:1});
  queue.configure(descriptor);
  assert.deepEqual(state,initial);
  const source={};readModes.set(source,123);
  let frame;
  assert.equal(queue.enqueue(source,20,value=>{frame=value;},1),true);
  assert.deepEqual(state,initial);
  assert.equal(readModes.get(source),123);
  assert.equal(queue.available,false);
  assert.equal(queue.enqueue(source,21,()=>assert.fail(),2),false);
  queue.poll();assert.equal(frame,undefined);
  syncs[0].ready=true;
  queue.poll();
  assert.deepEqual([...frame.data],topDown);
  assert.deepEqual(state,initial);
  assert.equal(queue.available,true);
  queue.close();queue.close();
  assert.ok(buffers.every(buffer=>buffer.deleted));
  assert.ok(syncs.every(sync=>sync.deleted));
});

test('WebGL2 drops old frames on configure and releases resources after read errors', () => {
  const {gl,state,buffers,syncs}=glFixture();
  const initial = new Map(state);
  const queue = new WebGL2ExportQueue({gl,slots:1});
  queue.configure(descriptor);
  queue.enqueue(null,1,()=>assert.fail('old callback'),1);
  queue.configure(descriptor);
  syncs[0].ready=true;
  queue.poll();
  assert.ok(buffers[0].deleted);
  gl.readPixels=()=>{throw Error('read failed');};
  assert.throws(()=>queue.enqueue(null,2,()=>assert.fail(),2),/read failed/);
  assert.deepEqual(state,initial);
  assert.equal(queue.available,true);
  queue.close();
  assert.ok(buffers.every(buffer=>buffer.deleted));
});

function gpuFixture() {
  const buffers=[];
  const device={limits:{maxBufferSize:1024*1024},lost:new Promise(()=>{}),pushErrorScope(){},popErrorScope(){return Promise.resolve(null);},createBuffer({size,usage}){
    assert.equal(usage,9);
    const buffer={bytes:new Uint8Array(size),mapState:'unmapped',destroyed:false,
      mapAsync(mode){assert.equal(mode,1);this.mapState='pending';return new Promise((resolve,reject)=>{this.resolve=()=>{this.mapState='mapped';resolve();};this.reject=reject;});},
      getMappedRange(){assert.equal(this.mapState,'mapped');return this.bytes.buffer;},
      unmap(){this.mapState='unmapped';},destroy(){this.destroyed=true;this.mapState='unmapped';},
    };buffers.push(buffer);return buffer;
  },createCommandEncoder(){return {copyTextureToBuffer(source,target,extent){this.copy={source,target,extent};},finish(){return this.copy;}};},
    queue:{submit(commands){for(const {source,target,extent} of commands){assert.equal(target.bytesPerRow,256);assert.equal(extent.width,1);target.buffer.bytes.set(source.texture.pixels.slice(0,4));target.buffer.bytes.set(source.texture.pixels.slice(4),256);}}},
  };
  const texture={width:1,height:2,depthOrArrayLayers:1,dimension:'2d',sampleCount:1,format:'bgra8unorm',usage:1,pixels:new Uint8Array([0,0,255,255,255,0,0,128])};
  return {device,buffers,texture};
}

const flush = async () => {await Promise.resolve();await Promise.resolve();};

test('WebGPU bounds slots and strips row padding with BGRA conversion only after mapping', async () => {
  const {device,buffers,texture}=gpuFixture();
  const queue=new WebGPUExportQueue({device,slots:1});
  queue.configure(descriptor);
  let frame;
  assert.equal(queue.enqueue(texture,30,(value,time,sequence)=>{frame=value;assert.equal(time,30);assert.equal(sequence,4);},4),true);
  assert.equal(queue.available,false);
  assert.equal(queue.enqueue(texture,31,()=>assert.fail(),5),false);
  queue.poll();assert.equal(frame,undefined);
  buffers[0].resolve();await flush();
  assert.equal(frame,undefined);
  queue.poll();
  assert.deepEqual([...frame.data],topDown);
  assert.equal(buffers[0].mapState,'unmapped');
  assert.equal(queue.available,true);
  queue.close();
  assert.ok(buffers.every(buffer=>buffer.destroyed));
});

test('WebGPU suppresses stale mappings and releases slots after mapping rejection', async () => {
  const {device,buffers,texture}=gpuFixture();
  const queue=new WebGPUExportQueue({device,slots:1});
  queue.configure(descriptor);
  queue.enqueue(texture,1,()=>assert.fail('stale callback'),1);
  queue.configure(descriptor);
  buffers[0].resolve();await flush();queue.poll();
  assert.ok(buffers[0].destroyed);
  queue.enqueue(texture,2,()=>assert.fail('failed callback'),2);
  buffers[1].reject(Error('map failed'));await flush();
  assert.throws(()=>queue.poll(),/map failed/);
  assert.equal(queue.available,true);
  queue.enqueue(texture,3,()=>assert.fail('closed callback'),3);
  queue.close();
  buffers[1].resolve();await flush();queue.poll();
  assert.ok(buffers.every(buffer=>buffer.destroyed));
});

test('WebGPU rejects unusable textures and invalid slot bounds', () => {
  const {device,texture}=gpuFixture();
  for (const slots of [0,1.2,Infinity,1000]) assert.throws(()=>new WebGPUExportQueue({device,slots}));
  const queue=new WebGPUExportQueue({device,slots:1});
  queue.configure(descriptor);
  for (const override of [{width:2},{usage:0},{sampleCount:4},{format:'rgba16float'},{dimension:'3d'},{depthOrArrayLayers:2}]) {
    assert.throws(()=>queue.enqueue({...texture,...override},1,()=>assert.fail(),1));
  }
  assert.equal(queue.available,true);
  queue.close();
});

test('WebGL2 stops polling deleted resources when a callback closes the queue', () => {
  const {gl,syncs}=glFixture();
  const queue = new WebGL2ExportQueue({gl,slots:2});
  queue.configure(descriptor);
  queue.enqueue(null,1,()=>queue.close(),1);
  queue.enqueue(null,2,()=>assert.fail('callback after close'),2);
  for (const sync of syncs) sync.ready=true;
  const wait=gl.clientWaitSync;
  gl.clientWaitSync=(sync,...args)=>{assert.equal(sync.deleted,undefined,'must not wait on a deleted fence');return wait(sync,...args);};
  queue.poll();
  assert.equal(queue.available,false);
});

test('WebGL2 detects allocation failure and releases all allocated buffers', () => {
  const {gl,buffers}=glFixture();
  gl.NO_ERROR=0;
  gl.getError=()=>1285;
  const queue = new WebGL2ExportQueue({gl,slots:2});
  assert.throws(()=>queue.configure(descriptor),/buffer/);
  assert.equal(queue.available,false);
  assert.ok(buffers.every(buffer=>buffer.deleted));
});

test('Canvas configure failure disables admission after an earlier configuration', () => {
  const {canvas}=canvasFixture();
  const queue = new CanvasExportQueue({canvas});
  queue.configure(descriptor);
  canvas.ownerDocument.createElement=()=>{throw Error('allocation failed');};
  assert.throws(()=>queue.configure(descriptor),/allocation failed/);
  assert.equal(queue.available,false);
  assert.equal(queue.enqueue(null,1,()=>assert.fail(),1),false);
});

test('WebGPU callback reconfiguration suppresses other ready frames from the old configuration', async () => {
  const {device,buffers,texture}=gpuFixture();
  const queue = new WebGPUExportQueue({device,slots:2});
  queue.configure(descriptor);
  queue.enqueue(texture,1,()=>queue.configure(descriptor),1);
  queue.enqueue(texture,2,()=>assert.fail('stale callback'),2);
  buffers[0].resolve();buffers[1].resolve();await flush();
  queue.poll();
  assert.equal(queue.available,true);
  assert.ok(buffers[0].destroyed);
  assert.ok(buffers[1].destroyed);
  queue.close();
});

test('WebGPU device loss closes the queue and suppresses mapped completions', async () => {
  const {device,buffers,texture}=gpuFixture();
  let lose;
  device.lost=new Promise(resolve=>{lose=resolve;});
  const queue = new WebGPUExportQueue({device,slots:1});
  queue.configure(descriptor);
  queue.enqueue(texture,1,()=>assert.fail('callback after device loss'),1);
  buffers[0].resolve();lose({reason:'destroyed'});await flush();
  queue.poll();
  assert.equal(queue.available,false);
  assert.ok(buffers[0].destroyed);
});

test('WebGPU rejects validation failures even when the readback buffer maps', async () => {
  const {device,buffers,texture}=gpuFixture();
  let scopes=0;
  device.pushErrorScope=()=>{scopes+=1;};
  device.popErrorScope=()=>{scopes-=1;return Promise.resolve({message:'copy validation failed'});};
  const queue = new WebGPUExportQueue({device,slots:1});
  queue.configure(descriptor);
  queue.enqueue(texture,1,()=>assert.fail('invalid copy must not produce a frame'),1);
  assert.equal(scopes,0);
  buffers[0].resolve();
  for(let i=0;i<6;i+=1) await flush();
  assert.throws(()=>queue.poll(),/copy validation failed/);
  assert.equal(queue.available,true);
  queue.close();
});

test('Queues reject daemon dimension bounds and reuse callback storage', () => {
  const queue = new RgbaExportQueue();
  for (const override of [{width:4097,height:1}, {width:1,height:4097}]) {
    assert.throws(()=>queue.configure({...descriptor,...override}));
  }
  queue.configure(descriptor);
  const source={width:1,height:2,rowStride:4,data:new Uint8Array(topDown)};
  let first;
  queue.enqueue(source,1,frame=>{first=frame.data;},1);
  source.data.fill(50);
  queue.enqueue(source,2,frame=>{assert.equal(frame.data,first);assert.deepEqual([...frame.data],[50,50,50,50,50,50,50,50]);},2);
  const {device}=gpuFixture();
  assert.throws(()=>new WebGPUExportQueue({device,slots:9}));
});

test('WebGL2 reuses one output buffer per readback slot', () => {
  const {gl,syncs}=glFixture();
  const queue = new WebGL2ExportQueue({gl,slots:1});
  queue.configure(descriptor);
  let first;
  queue.enqueue(null,1,frame=>{first=frame.data;},1);
  syncs[0].ready=true;queue.poll();
  queue.enqueue(null,2,frame=>assert.equal(frame.data,first),2);
  syncs[1].ready=true;queue.poll();
  queue.close();
});

test('WebGPU reuses one output buffer per readback slot', async () => {
  const {device,buffers,texture}=gpuFixture();
  const queue = new WebGPUExportQueue({device,slots:1});
  queue.configure(descriptor);
  let first;
  queue.enqueue(texture,1,frame=>{first=frame.data;},1);
  buffers[0].resolve();await flush();queue.poll();
  queue.enqueue(texture,2,frame=>assert.equal(frame.data,first),2);
  buffers[0].resolve();await flush();queue.poll();
  queue.close();
});

test('GPU callbacks keep borrowed storage unavailable until the callback returns', async () => {
  const {gl,syncs}=glFixture();
  const glQueue = new WebGL2ExportQueue({gl,slots:1});
  glQueue.configure(descriptor);
  glQueue.enqueue(null,1,()=>{
    assert.equal(glQueue.available,false);
    assert.equal(glQueue.enqueue(null,2,()=>assert.fail(),2),false);
    glQueue.poll();
  },1);
  syncs[0].ready=true;glQueue.poll();glQueue.close();
  const {device,buffers,texture}=gpuFixture();
  const gpuQueue = new WebGPUExportQueue({device,slots:1});
  gpuQueue.configure(descriptor);
  gpuQueue.enqueue(texture,1,()=>{
    assert.equal(gpuQueue.available,false);
    assert.equal(gpuQueue.enqueue(texture,2,()=>assert.fail(),2),false);
    gpuQueue.poll();
  },1);
  buffers[0].resolve();await flush();gpuQueue.poll();gpuQueue.close();
});
