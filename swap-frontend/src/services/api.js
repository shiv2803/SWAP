const DEFAULT_BASE_URL = 'http://localhost:8000';
const DEFAULT_WS_URL = 'ws://localhost:8000/ws/live';

export class SwapApiClient {
  constructor(baseUrl = DEFAULT_BASE_URL) {
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

export class SwapWebSocketClient {
  constructor(wsUrl = DEFAULT_WS_URL, onMessage, onStatusChange) {
    this.wsUrl = wsUrl;
    this.onMessage = onMessage;
    this.onStatusChange = onStatusChange;
    this.ws = null;
    this.reconnectTimer = null;
    this.isExplicitClose = false;
  }

  connect() {
    this.isExplicitClose = false;
    try {
      this.ws = new WebSocket(this.wsUrl);

      this.ws.onopen = () => {
        if (this.onStatusChange) this.onStatusChange('connected');
      };

      this.ws.onmessage = (event) => {
        try {
          const payload = JSON.parse(event.data);
          if (this.onMessage) this.onMessage(payload);
        } catch (err) {
          console.error('Failed to parse WebSocket frame:', err);
        }
      };

      this.ws.onerror = () => {
        if (this.onStatusChange) this.onStatusChange('error');
      };

      this.ws.onclose = () => {
        if (this.onStatusChange) this.onStatusChange('disconnected');
        if (!this.isExplicitClose) {
          this.scheduleReconnect();
        }
      };
    } catch (err) {
      if (this.onStatusChange) this.onStatusChange('error');
      this.scheduleReconnect();
    }
  }

  scheduleReconnect() {
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = setTimeout(() => {
      this.connect();
    }, 3000);
  }

  disconnect() {
    this.isExplicitClose = true;
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
  }
}
