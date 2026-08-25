import React from 'react';

export default function ConnectionBanner({ connected, connecting, error, onRetry, onDismiss }) {
  if (connected && !error) return null;

  return (
    <div className="connection-banner" role="alert" aria-live="assertive">
      <div className="banner-content">
        <span className="banner-icon" aria-hidden="true">
          {connecting ? '◷' : error ? '✗' : '⚠'}
        </span>
        <span className="banner-text">
          {connecting && 'Connecting to backend...'}
          {error && `Connection error: ${error}`}
          {!connecting && !error && 'Disconnected from backend'}
        </span>
      </div>
      <div className="banner-actions">
        {connecting && (
          <button className="btn secondary" onClick={onDismiss} disabled>
            <span className="spinner" aria-hidden="true"></span>
            CONNECTING...
          </button>
        )}
        {!connecting && (
          <>
            <button className="btn primary" onClick={onRetry}>
              {error ? 'RETRY' : 'CONNECT'}
            </button>
            {error && (
              <button className="btn secondary" onClick={onDismiss}>
                DISMISS
              </button>
            )}
          </>
        )}
      </div>
    </div>
  );
}