import React from 'react';
import { useSettings } from '../hooks/useSettings';

export default function SettingsModal({ isOpen, onClose }) {
  const { settings, updateSetting, resetSettings, exportSettings, importSettings, initialized } = useSettings();

  if (!isOpen) return null;

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
    <div className="modal-overlay" onClick={onClose} role="dialog" aria-modal="true" aria-labelledby="settings-title">
      <div className="modal" onClick={e => e.stopPropagation()}>
        <div className="modal-header">
          <h2 id="settings-title">SETTINGS</h2>
          <button className="icon-btn" onClick={onClose} aria-label="Close settings">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
              <line x1="18" y1="6" x2="6" y2="18" />
              <line x1="6" y1="6" x2="18" y2="18" />
            </svg>
          </button>
        </div>

        <div className="modal-content">
          <section className="settings-section">
            <h3>DISPLAY</h3>
            <div className="setting-row">
              <label>
                <span>SCANLINES</span>
                <input
                  type="checkbox"
                  checked={settings.showScanlines}
                  onChange={e => updateSetting('showScanlines', e.target.checked)}
                />
              </label>
            </div>
            <div className="setting-row">
              <label>
                <span>NOISE TEXTURE</span>
                <input
                  type="checkbox"
                  checked={settings.showNoise}
                  onChange={e => updateSetting('showNoise', e.target.checked)}
                />
              </label>
            </div>
            <div className="setting-row">
              <label>
                <span>COMPACT MODE</span>
                <input
                  type="checkbox"
                  checked={settings.compactMode}
                  onChange={e => updateSetting('compactMode', e.target.checked)}
                />
              </label>
            </div>
          </section>

          <section className="settings-section">
            <h3>CONNECTION</h3>
            <div className="setting-row">
              <label>
                <span>AUTO RECONNECT</span>
                <input
                  type="checkbox"
                  checked={settings.autoReconnect}
                  onChange={e => updateSetting('autoReconnect', e.target.checked)}
                />
              </label>
            </div>
            <div className="setting-row">
              <label>
                <span>HEARTBEAT INTERVAL</span>
                <select
                  value={settings.heartbeatInterval}
                  onChange={e => updateSetting('heartbeatInterval', parseInt(e.target.value))}
                >
                  <option value={2000}>2s</option>
                  <option value={5000}>5s</option>
                  <option value={10000}>10s</option>
                  <option value={30000}>30s</option>
                </select>
              </label>
            </div>
          </section>

          <section className="settings-section">
            <h3>LOGGING</h3>
            <div className="setting-row">
              <label>
                <span>MAX LOG ENTRIES</span>
                <input
                  type="number"
                  min="100"
                  max="2000"
                  step="100"
                  value={settings.maxLogs}
                  onChange={e => updateSetting('maxLogs', parseInt(e.target.value))}
                />
              </label>
            </div>
            <div className="setting-row">
              <label>
                <span>DEFAULT LOG LEVEL</span>
                <select
                  value={settings.logLevel}
                  onChange={e => updateSetting('logLevel', e.target.value)}
                >
                  <option value="all">ALL</option>
                  <option value="error">ERROR ONLY</option>
                  <option value="warn">WARN+</option>
                  <option value="info">INFO+</option>
                </select>
              </label>
            </div>
          </section>

          <section className="settings-section">
            <h3>DATA</h3>
            <div className="setting-actions">
              <button className="btn secondary" onClick={() => {
                const json = exportSettings();
                navigator.clipboard.writeText(json);
              }}>
                EXPORT SETTINGS
              </button>
              <input
                type="file"
                accept=".json"
                onChange={e => {
                  const file = e.target.files[0];
                  if (file) {
                    const reader = new FileReader();
                    reader.onload = (evt) => importSettings(evt.target.result);
                    reader.readAsText(file);
                  }
                }}
                style={{ display: 'none' }}
                id="import-settings"
              />
              <label htmlFor="import-settings" className="btn secondary">
                IMPORT SETTINGS
              </label>
              <button className="btn danger" onClick={resetSettings}>
                RESET TO DEFAULTS
              </button>
            </div>
          </section>
        </div>

        <div className="modal-footer">
          <button className="btn" onClick={onClose}>DONE</button>
        </div>
      </div>
    </div>
  );
}