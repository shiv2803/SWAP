import React from 'react';

export default function Footer({ version = '1.0.0', connected = false, nodeCount = '0/0' }) {
  return (
    <footer className="footer">
      <div className="footer-left">
        <span>SWAP v{version}</span>
        <span className="footer-separator">|</span>
        <span className="kbd">ESP32-SX1262</span>
        <span className="footer-separator">|</span>
        <span className="kbd">INA219×2</span>
      </div>

      <div className="footer-center">
        TACTICAL TELEMETRY INTERFACE
      </div>

      <div className="footer-right">
        <span className={`connection-indicator ${connected ? 'connected' : 'disconnected'}`} aria-hidden="true"></span>
        <span className="kbd">WS</span>
        <span className="footer-separator">|</span>
        <span className="node-count">{nodeCount}</span>
        <span className="footer-separator">|</span>
        <span className="kbd">?</span> HELP
      </div>
    </footer>
  );
}