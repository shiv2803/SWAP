import React from 'react';

function getVoltageClass(v) {
  if (v <= 0) return 'bad';
  if (v < 3.0) return 'warn';
  if (v < 4.5) return 'warn';
  return 'good';
}

function getCurrentClass(i) {
  if (i <= 0) return 'bad';
  if (i > 500) return 'warn';
  return 'good';
}

function PowerRow({ label, value, unit, className, warning }) {
  return (
    <div className="power-row">
      <span className="power-label">{label}</span>
      <span className={`power-value ${className} ${warning ? 'warning' : ''}`}>
        {value}
        {unit && <span className="power-unit">{unit}</span>}
      </span>
    </div>
  );
}

function PowerBlock({ title, address, voltage, current, power, connected }) {
  const vClass = getVoltageClass(voltage);
  const iClass = getCurrentClass(current);
  const hasData = voltage > 0 || current > 0;

  return (
    <div className={`power-block ${!hasData ? 'no-data' : ''}`}>
      <div className="power-block-title">
        <span>{title} <span className="kbd">{address}</span></span>
        <span className={`power-status ${vClass}`} aria-label={vClass}>
          {hasData ? '●' : '○'}
        </span>
      </div>

      {!hasData && !connected && (
        <div className="power-empty">
          <span className="empty-icon">◷</span>
          <span>NO DATA</span>
          <span className="empty-hint">Check I2C / Node connection</span>
        </div>
      )}

      {hasData && (
        <>
          <PowerRow label="VOLTAGE" value={voltage.toFixed(2)} unit="V" className={vClass} />
          <PowerRow label="CURRENT" value={current.toFixed(1)} unit="mA" className={iClass} />
          <PowerRow label="POWER" value={power.toFixed(1)} unit="mW" className="neutral" />
        </>
      )}
    </div>
  );
}

export default function PowerPanel({ nodeId, power, connected }) {
  const mainV = power?.mainV ?? 0;
  const mainI = power?.mainI ?? 0;
  const mainP = mainV * mainI;
  const loraV = power?.loraV ?? 0;
  const loraI = power?.loraI ?? 0;
  const loraP = loraV * loraI;

  const hasAnyData = (mainV > 0 || mainI > 0 || loraV > 0 || loraI > 0);

  return (
    <section className="panel">
      <div className="panel-title">
        <span>POWER</span>
        <span className={`badge ${hasAnyData ? 'badge-green' : 'badge-gray'}`}>
          INA219
        </span>
      </div>

      <div className="power-readings">
        <PowerBlock
          title="MAIN RAIL"
          address="0x40"
          voltage={mainV}
          current={mainI}
          power={mainP}
          connected={connected}
        />
        <PowerBlock
          title="LORA RAIL"
          address="0x41"
          voltage={loraV}
          current={loraI}
          power={loraP}
          connected={connected}
        />
      </div>

      {!hasAnyData && connected && (
        <div className="empty-state full-width">
          <span className="empty-icon">⚠</span>
          <span>NO POWER DATA</span>
          <span className="empty-hint">INA219 not responding — check I2C bus</span>
        </div>
      )}
    </section>
  );
}