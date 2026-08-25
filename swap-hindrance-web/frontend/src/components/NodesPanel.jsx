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

export default function NodesPanel({ nodeList, selectedNode, onSelectNode, onNodeDetail }) {
  if (nodeList.length === 0) {
    return (
      <section className="panel">
        <div className="panel-title">NODES</div>
        <div className="empty-state">
          <span className="empty-icon">◷</span>
          <span>NO NODES CONNECTED</span>
          <span className="empty-hint">Connect ESP32 nodes to begin</span>
        </div>
      </section>
    );
  }

  return (
    <section className="panel">
      <div className="panel-title">
        <span>NODES</span>
        <span className="node-count-badge">{nodeList.length}</span>
      </div>

      <div className="nodes-grid" role="list">
        {nodeList.map(node => {
          const connected = node.ws?.readyState === 1;
          const status = node.status || {};
          const telemetry = status.telemetry || {};
          const power = status.power || {};
          const isSelected = selectedNode === node.id;

          return (
            <article
              key={node.id}
              className={`node-card ${isSelected ? 'selected' : ''} ${connected ? 'online' : 'offline'}`}
              role="listitem"
              onClick={() => onSelectNode(node.id)}
              onDoubleClick={() => onNodeDetail?.(node.id)}
            >
              <div className="node-card-header">
                <div className="node-identity">
                  <span className="node-id">{node.id}</span>
                  <span className={`node-status-dot ${connected ? 'online' : 'offline'}`} aria-hidden="true"></span>
                </div>
                <span className="node-role">{node.info?.role || 'UNKNOWN'}</span>
              </div>

              <div className="node-meta">
                <div className="meta-row">
                  <span className="meta-label">PACKETS</span>
                  <span className="meta-value">{formatNumber(telemetry.packets ?? status.packets ?? 0)}</span>
                </div>
                <div className="meta-row">
                  <span className="meta-label">UPTIME</span>
                  <span className="meta-value">{formatUptime(telemetry.uptime ?? status.uptime)}</span>
                </div>
                <div className="meta-row">
                  <span className="meta-label">MODE</span>
                  <span className="meta-value">{status.mode !== undefined ? String(status.mode) : '—'}</span>
                </div>
                <div className="meta-row">
                  <span className="meta-label">LAST SEEN</span>
                  <span className="meta-value">{node.lastSeen ? new Date(node.lastSeen).toLocaleTimeString() : '—'}</span>
                </div>
                {power.mainV && (
                  <div className="meta-row power-row">
                    <span className="meta-label">POWER</span>
                    <span className="meta-value">
                      {power.mainV.toFixed(1)}V {power.mainI.toFixed(0)}mA
                    </span>
                  </div>
                )}
              </div>

              <div className="node-actions">
                <button
                  className={`btn ${isSelected ? 'primary' : 'secondary'}`}
                  onClick={e => { e.stopPropagation(); onSelectNode(node.id); }}
                  disabled={isSelected}
                  aria-pressed={isSelected}
                >
                  {isSelected ? 'SELECTED' : 'SELECT'}
                </button>
                <button
                  className="btn secondary"
                  onClick={e => { e.stopPropagation(); onNodeDetail?.(node.id); }}
                  disabled={!connected}
                >
                  DETAIL
                </button>
              </div>
            </article>
          );
        })}
      </div>
    </section>
  );
}