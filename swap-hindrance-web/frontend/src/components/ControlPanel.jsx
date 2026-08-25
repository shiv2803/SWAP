import React from 'react';

export default function ControlPanel({
  modes,
  currentMode,
  onModeSelect,
  targets,
  activeTargets,
  onTargetToggle,
  onStop,
  onStatus,
  onReset,
  hindranceRunning,
  lastAction,
  categories
}) {
  const modeList = React.useMemo(() => Object.values(modes).sort((a, b) => a.id - b.id), [modes]);

  const selectedMode = modes[currentMode];

  return (
    <>
      <div className="panel-title">
        <span>MODE SELECTION</span>
        <div className="panel-title-right">
          <span className={`badge ${currentMode === 0 ? 'badge-idle' : 'badge-active'}`}>
            {selectedMode?.label || 'IDLE'}
          </span>
          {lastAction && lastAction.type === 'mode' && (
            <span className="last-action" aria-live="polite">
              SET: {lastAction.value}
            </span>
          )}
        </div>
      </div>

      <div className="mode-grid" role="group" aria-label="Hindrance modes">
        {modeList.map(mode => {
          const isSelected = currentMode === mode.id;
          const isIdle = mode.id === 0;
          return (
            <button
              key={mode.id}
              className={`mode-btn category-${mode.category} ${isSelected ? 'active' : ''} ${!hindranceRunning && !isIdle ? 'disabled' : ''}`}
              onClick={() => onModeSelect(mode.id)}
              disabled={!hindranceRunning && !isIdle}
              aria-pressed={isSelected}
              aria-label={`${categories[mode.category] || mode.category}: ${mode.label} - ${mode.desc}`}
              title={`${mode.name} (${mode.desc})`}
            >
              <span className="mode-icon" aria-hidden="true">{mode.icon}</span>
              <span className="mode-label">{mode.label}</span>
              <span className="mode-category">{categories[mode.category] || mode.category}</span>
              {isSelected && <span className="mode-selected-indicator" aria-hidden="true"></span>}
            </button>
          );
        })}
      </div>

      <div className="panel-title">
        <span>ACTIVE TARGETS</span>
        <span className="target-count">
          {Object.values(targets).filter(t => activeTargets & t.bit).length} / {targets.length}
        </span>
      </div>

      <div className="targets-row" role="group" aria-label="Protocol targets">
        {targets.map(t => (
          <button
            key={t.key}
            className={`target-toggle ${t.key} ${(activeTargets & t.bit) ? 'active' : ''}`}
            onClick={() => onTargetToggle(t.key)}
            aria-pressed={!!(activeTargets & t.bit)}
            aria-label={`${t.label} target ${(activeTargets & t.bit) ? 'enabled' : 'disabled'}`}
            style={{ '--target-color': t.color }}
          >
            <span className="led" aria-hidden="true"></span>
            <span className="target-label">{t.label}</span>
            <span className="target-hint" aria-hidden="true">
              {['w', 'b', 'l'][Object.keys(targets).indexOf(t.key)]?.toUpperCase()}
            </span>
          </button>
        ))}
      </div>

      <div className="panel-title">COMMAND</div>
      <div className="control-row">
        <button className="btn danger" onClick={onStop} aria-label="Stop all hindrance (S)">
          <span className="btn-icon" aria-hidden="true">■</span>
          STOP ALL
          <kbd className="btn-shortcut">S</kbd>
        </button>
        <button className="btn" onClick={onStatus} aria-label="Request status">
          <span className="btn-icon" aria-hidden="true">◷</span>
          STATUS
        </button>
        <button className="btn" onClick={onReset} aria-label="Reset counters (R)">
          <span className="btn-icon" aria-hidden="true">↻</span>
          RESET CNT
          <kbd className="btn-shortcut">R</kbd>
        </button>
      </div>

      <div className="keyboard-hints" aria-hidden="true">
        <kbd>1-9,0</kbd> Modes · <kbd>W</kbd> WiFi · <kbd>B</kbd> BLE · <kbd>L</kbd> LoRa · <kbd>A</kbd> All · <kbd>?</kbd> Settings
      </div>
    </>
  );
}