export const SYNC_SDK_VERSION: '0.2.0';
export const SYNC_DEFAULT_ENDPOINT: 'http://127.0.0.1:53979';
export type ColorSpace = 'srgb' | 'display-p3';
export type AlphaMode = 'opaque' | 'straight' | 'premultiplied';
export interface FrameDescriptor {
  width: number;
  height: number;
  format: 'rgba8unorm';
  colorSpace: ColorSpace;
  alphaMode: AlphaMode;
  fps: number;
}
export interface RgbaSource {
  width: number;
  height: number;
  rowStride: number;
  data: ArrayBuffer | ArrayBufferView;
}
export interface ExportedFrame extends FrameDescriptor {
  rowStride: number;
  /** The queue owns these bytes. Use them before the callback returns. */
  data: Uint8Array;
}
export interface CloseOptions { backendLost?: boolean }
export type FrameCallback = (frame: ExportedFrame, timestamp: number, sequence: number) => void;
export interface ExportQueue<Source = unknown> {
  readonly available: boolean;
  configure(descriptor: FrameDescriptor): void;
  enqueue(source: Source, timestamp: number, onFrame: FrameCallback, sequence: number): boolean;
  poll(): void;
  close(options?: CloseOptions): void;
}
export class RgbaExportQueue implements ExportQueue<RgbaSource> {
  readonly available: boolean;
  configure(descriptor: FrameDescriptor): void;
  enqueue(source: RgbaSource, timestamp: number, onFrame: FrameCallback, sequence: number): boolean;
  poll(): void;
  close(options?: CloseOptions): void;
}
export class CanvasExportQueue implements ExportQueue<unknown> {
  constructor(options: { canvas: HTMLCanvasElement | OffscreenCanvas });
  readonly available: boolean;
  configure(descriptor: FrameDescriptor): void;
  enqueue(source: unknown, timestamp: number, onFrame: FrameCallback, sequence: number): boolean;
  poll(): void;
  close(options?: CloseOptions): void;
}
export class WebGL2ExportQueue implements ExportQueue<WebGLFramebuffer | null> {
  constructor(options: { gl: WebGL2RenderingContext; slots?: number });
  readonly available: boolean;
  configure(descriptor: FrameDescriptor): void;
  enqueue(source: WebGLFramebuffer | null, timestamp: number, onFrame: FrameCallback, sequence: number): boolean;
  poll(): void;
  close(options?: CloseOptions): void;
}
/** Accepts a browser GPUDevice. This declaration needs no external GPU type package. */
export class WebGPUExportQueue<Texture extends object = object> implements ExportQueue<Texture> {
  constructor(options: { device: object; slots?: number });
  readonly available: boolean;
  configure(descriptor: FrameDescriptor): void;
  enqueue(source: Texture, timestamp: number, onFrame: FrameCallback, sequence: number): boolean;
  poll(): void;
  close(options?: CloseOptions): void;
}
export interface LocalStats {
  accepted: number;
  sent: number;
  droppedBusy: number;
  droppedBackpressure: number;
  failed: number;
}
export interface NativeStats {
  accepted: number;
  dropped: number;
  rejected: number;
  failed: number;
  lastSequence: number;
  lastPresentationTimeUs: number;
  /** A lowercase hexadecimal string with 16 digits. */
  checksum: string;
}
export interface SenderOptions<Source = unknown> {
  exportQueue: ExportQueue<Source>;
  maxBufferedBytes?: number;
  maxBufferedFrames?: number;
  clock?: Pick<Performance, 'timeOrigin'>;
}
export type RgbaSenderOptions = Omit<SenderOptions<RgbaSource>, 'exportQueue'>;
export interface Sender<Source = unknown> {
  readonly id: string;
  readonly stats: LocalStats;
  readonly closed: Promise<{ type: 'senderClosed'; id: string }>;
  configure(descriptor: FrameDescriptor): void;
  submit(source: Source, timestamp: number): boolean;
  getStats(): Promise<NativeStats>;
  close(options?: CloseOptions): void;
}
export interface FrameSocket {
  readonly readyState: number;
  readonly bufferedAmount: number;
  send(data: ArrayBuffer | ArrayBufferView): void;
  close(): void;
}
export class SyncFrameSink<Source = unknown> {
  constructor(options: SenderOptions<Source> & { socket: FrameSocket });
  readonly stats: LocalStats;
  configure(descriptor: FrameDescriptor): void;
  submit(source: Source, timestamp: number): boolean;
  close(options?: CloseOptions): void;
}
export interface ProviderCapability {
  id: string;
  direction: 'send' | 'receive';
  available: boolean;
  selected: boolean;
}
export interface Capabilities {
  send: boolean;
  receive: boolean;
  providers: ProviderCapability[];
}
export interface Health {
  product: 'Sync';
  status: 'ok';
  version: string;
  protocolVersions: number[];
  instanceId: string;
  capabilities: Capabilities;
}
export interface Welcome {
  type: 'welcome';
  protocolVersion: 1;
  version: string;
  instanceId: string;
  capabilities: Capabilities;
}
export interface PermissionsLike {
  query(descriptor: { name: string }): PromiseLike<{ state: string }>;
}
export interface ClientOptions {
  endpoint?: string;
  token?: string;
  fetch?: typeof globalThis.fetch;
  WebSocket?: typeof globalThis.WebSocket;
  permissions?: PermissionsLike;
  timeoutMs?: number;
  pairingTimeoutMs?: number;
  maxHealthBytes?: number;
  maxControlMessageBytes?: number;
}
export class SyncBridgeClient {
  constructor(options?: ClientOptions);
  readonly connected: boolean;
  readonly welcome: Welcome | null;
  probe(): Promise<{ available: true; health: Health } | { available: false; code: SyncErrorCode; message: string }>;
  pair(name: string): Promise<{ token: string; protocolVersion: 1 }>;
  connect(): Promise<Welcome>;
  createSender<Source>(name: string, options: SenderOptions<Source>): Promise<Sender<Source>>;
  createRgbaSender(name: string, options?: RgbaSenderOptions): Promise<Sender<RgbaSource>>;
  close(): void;
}
export type SyncErrorCode =
  | 'SYNC_UNAVAILABLE' | 'SYNC_TIMEOUT' | 'SYNC_AUTHENTICATION' | 'SYNC_PROTOCOL'
  | 'SYNC_CAPABILITY' | 'SYNC_LIFECYCLE' | 'SYNC_CONFIGURATION'
  | 'SYNC_PERMISSION_REQUIRED' | 'SYNC_PERMISSION_DENIED' | 'SYNC_PAIRING_DENIED'
  | 'SYNC_PAIRING_BUSY' | 'SYNC_PAIRING_STORE' | 'SYNC_PAIRING_DURABILITY'
  | 'SYNC_PAIRING_ORIGIN_LIMIT' | 'SYNC_SENDER_LOST';
export const SYNC_ERROR_CODE: Readonly<Record<
  'UNAVAILABLE' | 'TIMEOUT' | 'AUTHENTICATION' | 'PROTOCOL' | 'CAPABILITY' |
  'LIFECYCLE' | 'CONFIGURATION' | 'PERMISSION_REQUIRED' | 'PERMISSION_DENIED' |
  'PAIRING_DENIED' | 'PAIRING_BUSY' | 'PAIRING_STORE' | 'PAIRING_DURABILITY' |
  'PAIRING_ORIGIN_LIMIT' | 'SENDER_LOST', SyncErrorCode>>;
export interface BridgeErrorOptions { code?: SyncErrorCode; cause?: unknown; daemonCode?: string }
export class SyncBridgeError extends Error {
  constructor(message?: string, options?: BridgeErrorOptions);
  readonly code: SyncErrorCode;
  readonly daemonCode?: string;
}
export class SyncUnavailableError extends SyncBridgeError {}
export class SyncTimeoutError extends SyncBridgeError {}
export class SyncAuthenticationError extends SyncBridgeError {}
export class SyncProtocolError extends SyncBridgeError {}
export class SyncCapabilityError extends SyncBridgeError {}
export class SyncLifecycleError extends SyncBridgeError {}
export class SyncConfigurationError extends SyncBridgeError {}
export class SyncPermissionRequiredError extends SyncBridgeError {}
export class SyncPermissionDeniedError extends SyncBridgeError {}
export class SyncPairingDeniedError extends SyncBridgeError {}
export class SyncPairingBusyError extends SyncBridgeError {}
export class SyncPairingStoreError extends SyncBridgeError {}
export class SyncPairingDurabilityError extends SyncBridgeError {}
export class SyncPairingOriginLimitError extends SyncBridgeError {}
export class SyncSenderLostError extends SyncBridgeError {
  constructor(message?: string, options?: { closeCode?: number | null; closeReason?: string; cause?: unknown });
  readonly closeCode: number | null;
  readonly closeReason: string;
}
export const PIXEL_FORMAT: Readonly<{ RGBA8_UNORM: 1 }>;
export const COLOR_SPACE: Readonly<{ SRGB: 1; DISPLAY_P3: 2 }>;
export const ALPHA_MODE: Readonly<{ OPAQUE: 1; STRAIGHT: 2; PREMULTIPLIED: 3 }>;
export interface FrameMetadata {
  width: number;
  height: number;
  rowStride: number;
  sequence: number;
  presentationTimeUs: number;
  pixelFormat: 1;
  colorSpace: 1 | 2;
  alphaMode: 1 | 2 | 3;
}
export function encodeFrameV1(metadata: FrameMetadata, data: ArrayBuffer | ArrayBufferView): ArrayBuffer;
export function encodeFrameV1(metadata: FrameMetadata, data: ArrayBuffer | ArrayBufferView, into: ArrayBuffer): Uint8Array;
export function encodeFrameV1(metadata: FrameMetadata, data: ArrayBuffer | ArrayBufferView, into: undefined): ArrayBuffer;
export function encodeFrameV1(metadata: FrameMetadata, data: ArrayBuffer | ArrayBufferView, into: ArrayBuffer | undefined): ArrayBuffer | Uint8Array;
export interface FrameHeader extends FrameMetadata {
  version: 1;
  headerBytes: 64;
  flags: 1;
  payloadBytes: number;
}
export function decodeFrameHeaderV1(data: ArrayBuffer | ArrayBufferView): FrameHeader;
export interface DiagnosticSnapshot {
  schemaVersion: 1;
  sdkVersion: string;
  daemonVersion: string | null;
  protocolVersion: 1 | null;
  connected: boolean;
  providers: ProviderCapability[];
  frame: { [Key in keyof FrameDescriptor]: FrameDescriptor[Key] | null } | null;
  local: { [Key in keyof LocalStats]: number | null } | null;
  native: { [Key in keyof NativeStats]: NativeStats[Key] | null } | null;
  nativeError: SyncErrorCode | null;
  error: SyncErrorCode | null;
}
export function createDiagnosticSnapshot(options?: {
  client?: SyncBridgeClient;
  sender?: Pick<Sender, 'stats' | 'getStats'>;
  descriptor?: FrameDescriptor;
  error?: unknown;
}): Promise<DiagnosticSnapshot>;
