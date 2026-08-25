/* Backend endpoint resolution.
 *
 * The backend runs on the UNO Q while the dashboard is usually served from a
 * laptop, so its address can't be derived from window.location and used to be
 * hardcoded in the bundle -- which meant every IP change needed a rebuild, and
 * a cached bundle kept talking to the old address. Resolution order, highest
 * priority first:
 *
 *   1. ?backend=192.168.0.102:8000   (persisted to localStorage, so it sticks)
 *   2. localStorage["swap-backend"]  (set once, survives reloads)
 *   3. VITE_API_BASE_URL             (build-time default, .env.local)
 *   4. the host serving this page    (works when both run on the UNO Q)
 */
const DEFAULT_PORT = 8000;
const STORAGE_KEY = 'swap-backend';

function normalizeBase(value) {
  let raw = String(value || '').trim().replace(/\/+$/, '');
  if (!raw) return null;
  if (!/^https?:\/\//i.test(raw)) raw = `http://${raw}`;
  try {
    const url = new URL(raw);
    if (!url.port) url.port = String(DEFAULT_PORT);
    return `${url.protocol}//${url.host}`;
  } catch {
    return null;
  }
}

export function setBackendOverride(value) {
  try {
    if (value) localStorage.setItem(STORAGE_KEY, value);
    else localStorage.removeItem(STORAGE_KEY);
  } catch {
    /* private mode / storage disabled */
  }
}

export function resolveBaseUrl() {
  if (typeof window === 'undefined') return `http://localhost:${DEFAULT_PORT}`;
  try {
    const fromQuery = new URLSearchParams(window.location.search).get('backend');
    if (fromQuery) {
      setBackendOverride(fromQuery);
      const normalized = normalizeBase(fromQuery);
      if (normalized) return normalized;
    }
    const stored = normalizeBase(localStorage.getItem(STORAGE_KEY));
    if (stored) return stored;
  } catch {
    /* fall through to build-time / same-host defaults */
  }
  const fromEnv = normalizeBase(import.meta.env?.VITE_API_BASE_URL);
  if (fromEnv) return fromEnv;
  const { protocol, hostname } = window.location;
  if (hostname && hostname !== 'localhost' && hostname !== '127.0.0.1') {
    return `${protocol === 'https:' ? 'https:' : 'http:'}//${hostname}:${DEFAULT_PORT}`;
  }
  return `http://localhost:${DEFAULT_PORT}`;
}

export function resolveWsUrl(baseUrl = resolveBaseUrl()) {
  const explicit = import.meta.env?.VITE_WS_URL;
  // An explicit ws:// override only wins when no runtime backend override is in
  // play -- otherwise ?backend= would move HTTP but leave the socket behind.
  let hasOverride = false;
  try {
    hasOverride = Boolean(localStorage.getItem(STORAGE_KEY));
  } catch {
    hasOverride = false;
  }
  if (explicit && !hasOverride) return explicit;
  return `${baseUrl.replace(/^http/i, 'ws')}/ws/live`;
}

export function resolveSseUrl(baseUrl = resolveBaseUrl()) {
  return `${baseUrl}/events`;
}

export class SwapApiClient {
  constructor(baseUrl = resolveBaseUrl()) {
    this.baseUrl = baseUrl.replace(/\/$/, '');
  }

  async getHealth() {
    const res = await fetch(`${this.baseUrl}/health`);
    if (!res.ok) throw new Error(`Health check failed: ${res.statusText}`);
    return res.json();
  }

  async getStatus() {
    const res = await fetch(`${this.baseUrl}/status`);
    if (!res.ok) throw new Error(`Failed to fetch status: ${res.statusText}`);
    return res.json();
  }

  async getModelInfo() {
    const res = await fetch(`${this.baseUrl}/model/info`);
    if (!res.ok) throw new Error(`Failed to fetch model info: ${res.statusText}`);
    return res.json();
  }

  async getRecentTelemetry(limit = 50) {
    const res = await fetch(`${this.baseUrl}/telemetry/recent?limit=${limit}`);
    if (!res.ok) throw new Error(`Failed to fetch telemetry: ${res.statusText}`);
    return res.json();
  }

  async decideProtocol(node = 'a') {
    const res = await fetch(`${this.baseUrl}/decide?node=${node}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
    });
    if (!res.ok) {
      const err = await res.json().catch(() => ({ detail: res.statusText }));
      throw new Error(err.detail || 'Decision endpoint failed');
    }
    return res.json();
  }

  async setAutoSwitch(enabled) {
    const res = await fetch(`${this.baseUrl}/control/auto`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ enabled: Boolean(enabled) }),
    });
    if (!res.ok) throw new Error(`Failed to set auto-switch: ${res.statusText}`);
    return res.json();
  }

  async forceProtocol(node, protocol) {
    const res = await fetch(`${this.baseUrl}/force`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ node, protocol: Number(protocol) }),
    });
    if (!res.ok) {
      const err = await res.json().catch(() => ({ detail: res.statusText }));
      throw new Error(err.detail || 'Force protocol failed');
    }
    return res.json();
  }
}

/* /status is shaped for a snapshot view, the live stream is shaped per frame.
   Reshape one into the other so the polling fallback feeds the UI identically. */
export function statusToPayload(status, node = 'a') {
  const n = status?.nodes?.[node];
  if (!n) return null;
  return {
    type: 'telemetry',
    record: { ...n, node, active_protocol: n.active_protocol_reported },
    decision:
      n.recommended_protocol == null
        ? null
        : {
            protocol: n.recommended_protocol,
            protocol_name: n.recommended_protocol_name,
            confidence: n.confidence,
            source: n.decision_source,
            raw_scores: n.raw_scores,
            probabilities: n.probabilities,
            lqi: n.lqi,
            hysteresis_guarded: n.hysteresis_guarded,
            dwell_guarded: n.dwell_guarded,
          },
  };
}

/* Live telemetry with automatic transport fallback.
 *
 * WebSocket -> SSE (/events) -> polling (/status). A WebSocket upgrade can fail
 * for reasons no retry will fix (no websockets library in the backend venv, a
 * proxy that will not upgrade), and the dashboard should not go dark when it
 * does: after WS_ATTEMPTS failed opens it drops to SSE, and if that fails too,
 * to polling.
 */
const WS_ATTEMPTS = 2;
const SSE_ATTEMPTS = 2;
const RECONNECT_MS = 3000;
const POLL_MS = 1000;

export class SwapLiveClient {
  constructor(wsUrl = resolveWsUrl(), onMessage, onStatusChange, options = {}) {
    this.wsUrl = wsUrl;
    this.baseUrl = options.baseUrl || resolveBaseUrl();
    this.onMessage = onMessage;
    this.onStatusChange = onStatusChange;
    this.onTransportChange = options.onTransportChange || null;
    this.ws = null;
    this.sse = null;
    this.timer = null;
    this.pollTimer = null;
    this.isExplicitClose = false;
    this.transport = 'websocket';
    this.wsFailures = 0;
    this.sseFailures = 0;
  }

  setStatus(status) {
    if (this.onStatusChange) this.onStatusChange(status);
  }

  setTransport(transport) {
    if (this.transport === transport) return;
    this.transport = transport;
    console.info(`[swap] live transport -> ${transport}`);
    if (this.onTransportChange) this.onTransportChange(transport);
  }

  deliver(payload) {
    // Keep-alives carry no telemetry, but they do prove the stream is alive.
    if (payload?.type === 'ping') return;
    if (payload?.type === 'hello') {
      const snapshot = statusToPayload(payload.snapshot);
      if (snapshot?.record?.active_protocol != null && this.onMessage) this.onMessage(snapshot);
      return;
    }
    if (this.onMessage) this.onMessage(payload);
  }

  connect() {
    this.isExplicitClose = false;
    if (this.transport === 'websocket') this.connectWebSocket();
    else if (this.transport === 'sse') this.connectSse();
    else this.startPolling();
  }

  connectWebSocket() {
    try {
      const ws = new WebSocket(this.wsUrl);
      this.ws = ws;
      let opened = false;

      ws.onopen = () => {
        opened = true;
        this.wsFailures = 0;
        this.setTransport('websocket');
        this.setStatus('connected');
      };
      ws.onmessage = (event) => {
        try {
          this.deliver(JSON.parse(event.data));
        } catch (err) {
          console.error('Failed to parse WebSocket frame:', err);
        }
      };
      ws.onerror = () => {
        if (!opened) this.setStatus('error');
      };
      ws.onclose = () => {
        this.ws = null;
        this.setStatus('disconnected');
        if (this.isExplicitClose) return;
        if (!opened) this.wsFailures += 1;
        if (this.wsFailures >= WS_ATTEMPTS) {
          // Never established, twice over -- stop hammering an upgrade that
          // this backend or proxy clearly will not complete.
          console.warn('[swap] WebSocket never established, falling back to SSE (/events)');
          this.setTransport('sse');
          this.scheduleReconnect(0);
        } else {
          this.scheduleReconnect();
        }
      };
    } catch {
      this.wsFailures += 1;
      this.setStatus('error');
      if (this.wsFailures >= WS_ATTEMPTS) this.setTransport('sse');
      this.scheduleReconnect();
    }
  }

  connectSse() {
    if (typeof EventSource === 'undefined') {
      this.setTransport('polling');
      this.startPolling();
      return;
    }
    try {
      const sse = new EventSource(resolveSseUrl(this.baseUrl));
      this.sse = sse;
      let opened = false;

      sse.onopen = () => {
        opened = true;
        this.sseFailures = 0;
        this.setTransport('sse');
        this.setStatus('connected');
      };
      sse.onmessage = (event) => {
        try {
          this.deliver(JSON.parse(event.data));
        } catch (err) {
          console.error('Failed to parse SSE frame:', err);
        }
      };
      sse.onerror = () => {
        sse.close();
        this.sse = null;
        this.setStatus('disconnected');
        if (this.isExplicitClose) return;
        if (!opened) this.sseFailures += 1;
        if (this.sseFailures >= SSE_ATTEMPTS) {
          console.warn('[swap] SSE unavailable, falling back to /status polling');
          this.setTransport('polling');
          this.startPolling();
          return;
        }
        this.scheduleReconnect();
      };
    } catch {
      this.sseFailures += 1;
      this.setTransport('polling');
      this.startPolling();
    }
  }

  startPolling() {
    if (this.pollTimer || this.isExplicitClose) return;
    const api = new SwapApiClient(this.baseUrl);
    const tick = async () => {
      try {
        const payload = statusToPayload(await api.getStatus());
        this.setStatus('connected');
        if (payload?.record?.active_protocol != null) this.deliver(payload);
      } catch {
        this.setStatus('disconnected');
      }
    };
    tick();
    this.pollTimer = setInterval(tick, POLL_MS);
  }

  stopPolling() {
    if (this.pollTimer) clearInterval(this.pollTimer);
    this.pollTimer = null;
  }

  scheduleReconnect(delay = RECONNECT_MS) {
    if (this.isExplicitClose) return;
    if (this.timer) clearTimeout(this.timer);
    this.timer = setTimeout(() => this.connect(), delay);
  }

  disconnect() {
    this.isExplicitClose = true;
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    this.stopPolling();
    if (this.ws) {
      this.ws.onclose = null;
      this.ws.close();
      this.ws = null;
    }
    if (this.sse) {
      this.sse.close();
      this.sse = null;
    }
  }
}

// Existing name kept so callers do not have to change.
export const SwapWebSocketClient = SwapLiveClient;
