import React from 'react';

function formatUptime(ms) {
  if (!ms || ms < 0) return '0s';
  const s = Math.floor(ms / 1000);
  const m = Math.floor(s / 60);
  const h = Math.floor(m / 60);
  const d = Math.floor(h / 24);
  if (d > 0) return `${d}d ${h % 24}h`;
  if (h > 0) return `${h}h ${m % 60}m`;
  return `${s}s`;
}

function formatNumber(n) {
  if (n === null || n === undefined) return '—';
  if (n >= 1000000) return (n / 1000000).toFixed(1) + 'M';
  if (n >= 1000) return (n / 1000).toFixed(1) + 'K';
  return n.toLocaleString();
}

export default function NodeDetailModal({ nodeId, nodeData, onClose }) {
  if (!nodeData) return null;

  const info = nodeData.info || {};
  const status = nodeData.status || {};
  const telemetry = status.telemetry || {};
  const power = status.power || {};
  const connected = nodeData.ws?.readyState === 1;
  const lastSeen = nodeData.lastSeen ? new Date(nodeData.lastSeen).toLocaleString() : '—';

  const handleKeyDown = (e) => {
    if (e.key === 'Escape') onClose();
  };

  React.useEffect(() => {
    document.addEventListener('keydown', handleKeyDown);
    document.body.style.overflow = 'hidden';
    return () => {
      document.removeEventListener('keydown', handleKeyDown);
      document.body.style.overflow = '';
    };
  }, [onClose]);

  return (
    <div className="modal-overlay" onClick={onClose} role="dialog" aria-modal="true" aria-labelledby={`node-${nodeId}-title`}>
      <div className="modal modal-wide" onClick={e => e.stopPropagation()}>
        <div className="modal-header">
          <h2 id={`node-${nodeId}-title`}>
            <span className="node-badge">{nodeId}</span>
            NODE DETAIL
          </h2>
          <div className="modal-header-right">
            <span className={`status-badge ${connected ? 'online' : 'offline'}`}>
              {connected ? '● ONLINE' : '○ OFFLINE'}
            </span>
            <button className="icon-btn" onClick={onClose} aria-label="Close">
              <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                <line x1="18" y1="6" x2="6" y2="18" />
                <line x1="6" y1="6" x2="18" y2="18" />
              </svg>
            </button>
          </div>
        </div>

        <div className="modal-content">
          <div className="detail-grid">
            <section className="detail-section">
              <h3>IDENTITY</h3>
              <dl className="detail-list">
                <div><dt>NODE ID</dt><dd>{nodeId}</dd></div>
                <div><dt>ROLE</dt><dd>{info.role || 'UNKNOWN'}</dd></div>
                <div><dt>FIRMWARE</dt><dd>{info.fw || '—'}</dd></div>
                <div><dt>IP ADDRESS</dt><dd>{info.ip || '—'}</dd></div>
                <div><dt>LAST SEEN</dt><dd>{lastSeen}</dd></div>
                <div><dt>CONNECTION</dt><dd>{connected ? 'ACTIVE' : 'INACTIVE'}</dd></div>
              </dl>
            </section>

            <section className="detail-section">
              <h3>TELEMETRY</h3>
              <dl className="detail-list">
                <div><dt>PACKETS TX</dt><dd>{formatNumber(telemetry.packets ?? status.packets ?? 0)}</dd></div>
                <div><dt>BLE ADVERTISEMENTS</dt><dd>{formatNumber(telemetry.bleAdv ?? status.bleAdv ?? 0)}</dd></div>
                <div><dt>UPTIME</dt><dd>{formatUptime(telemetry.uptime ?? status.uptime)}</dd></div>
                <div><dt>CURRENT MODE</dt><dd>{status.mode !== undefined ? String(status.mode) : '—'}</dd></div>
                <div><dt>TARGETS</dt><dd>{status.targets !== undefined ? '0x' + status.targets.toString(16).toUpperCase() : '—'}</dd></div>
                <div><dt>RUNNING</dt><dd>{telemetry.running ? 'YES' : 'NO'}</dd></div>
              </dl>
            </section>

            <section className="detail-section">
              <h3>POWER (INA219)</h3>
              <dl className="detail-list">
                <div><dt>MAIN RAIL (0x40)</dt><dd>
                  {power.mainV ? `${power.mainV.toFixed(2)}V ${power.mainI.toFixed(1)}mA (${(power.mainV * power.mainI).toFixed(1)}mW)` : '—'}
                </dd></div>
                <div><dt>LORA RAIL (0x41)</dt><dd>
                  {power.loraV ? `${power.loraV.toFixed(2)}V ${power.loraI.toFixed(1)}mA (${(power.loraV * power.loraI).toFixed(1)}mW)` : '—'}
                </dd></div>
              </dl>
            </section>

            <section className="detail-section">
              <h3>RAW STATUS</h3>
              <pre className="raw-status">{JSON.stringify(status, null, 2)}</pre>
            </section>
          </div>
        </div>

        <div className="modal-footer">
          <button className="btn" onClick={onClose}>CLOSE</button>
        </div>
      </div>
    </div>
  );
}