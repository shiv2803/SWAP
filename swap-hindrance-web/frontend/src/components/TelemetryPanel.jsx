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

export default function TelemetryPanel({ nodeId, nodeData, telemetry, currentMode, modes, connected }) {
  const nodeConnected = nodeData?.ws?.readyState === 1;
  const status = nodeData?.status || {};
  const packets = telemetry?.packets ?? status?.packets ?? 0;
  const bleAdv = telemetry?.bleAdv ?? status?.bleAdv ?? 0;
  const uptime = telemetry?.uptime ?? status?.uptime ?? 0;
  const modeLabel = modes[currentMode]?.label || 'IDLE';
  const modeDesc = modes[currentMode]?.desc || '';

  const isAll = nodeId === 'all';

  return (
    <section className="panel">
      <div className="panel-title">
        <span>TELEMETRY</span>
        <div className="panel-title-right">
          <span className={`badge ${nodeConnected ? 'badge-green' : 'badge-gray'}`}>
            {isAll ? 'ALL NODES' : nodeId}
          </span>
        </div>
      </div>

      <div className="stats-grid" role="list" aria-label="Telemetry statistics">
        <StatBlock
          label="PACKETS TX"
          value={formatNumber(packets)}
          trend={packets > 0 ? 'up' : 'neutral'}
          unit="total"
        />
        <StatBlock
          label="UPTIME"
          value={formatUptime(uptime)}
          trend="neutral"
        />
        <StatBlock
          label="BLE ADVS"
          value={formatNumber(bleAdv)}
          trend={bleAdv > 0 ? 'up' : 'neutral'}
          unit="total"
        />
        <StatBlock
          label="MODE"
          value={modeLabel}
          description={modeDesc}
          truncate={true}
        />
      </div>

      {!isAll && nodeConnected && (
        <>
          <div className="panel-title">NODE INFO</div>
          <NodeInfoGrid nodeData={nodeData} />
        </>
      )}

      {!isAll && !nodeConnected && (
        <div className="empty-state">
          <span className="empty-icon">◷</span>
          <span>NODE OFFLINE</span>
          <span className="empty-hint">Waiting for connection...</span>
        </div>
      )}

      {isAll && (
        <div className="panel-title">NODE SUMMARY</div>
      )}

      {isAll && (
        <NodeSummaryGrid nodeData={nodeData} connected={connected} />
      )}
    </section>
  );
}

function StatBlock({ label, value, trend = 'neutral', description, truncate = false, unit }) {
  return (
    <div className="stat-block" role="listitem">
      <div className="stat-label">{label}</div>
      <div className={`stat-value ${trend === 'up' ? 'trend-up' : ''} ${truncate ? 'truncate' : ''}`}>
        {value}
        {unit && <span className="stat-unit">{unit}</span>}
      </div>
      {description && <div className="stat-description">{description}</div>}
    </div>
  );
}

function NodeInfoGrid({ nodeData }) {
  const info = nodeData.info || {};
  const status = nodeData.status || {};
  const lastSeen = nodeData.lastSeen ? new Date(nodeData.lastSeen).toLocaleTimeString() : '—';

  const fields = [
    { label: 'ROLE', value: info.role || 'UNKNOWN' },
    { label: 'LAST SEEN', value: lastSeen },
    { label: 'IP', value: info.ip || '—' },
    { label: 'FIRMWARE', value: info.fw || '—' },
    { label: 'PACKETS', value: formatNumber(status.packets || 0) },
    { label: 'MODE', value: status.mode !== undefined ? String(status.mode) : '—' }
  ];

  return (
    <div className="info-grid" role="list">
      {fields.map((field, i) => (
        <div key={i} className="info-item" role="listitem">
          <span className="info-label">{field.label}</span>
          <span className="info-value">{field.value}</span>
        </div>
      ))}
    </div>
  );
}

function NodeSummaryGrid({ nodeData, connected }) {
  // For ALL nodes view, show summary cards
  // This is a placeholder - would need actual node list
  return (
    <div className="info-grid">
      <div className="info-item">
        <span className="info-label">CONNECTED</span>
        <span className="info-value good">{connected}</span>
      </div>
      <div className="info-item">
        <span className="info-label">TOTAL</span>
        <span className="info-value">{connected + 1}</span>
      </div>
    </div>
  );
}