import React from 'react';

export default class ErrorBoundary extends React.Component<
  { children: React.ReactNode },
  { error: Error | null }
> {
  constructor(props: { children: React.ReactNode }) {
    super(props);
    this.state = { error: null };
  }

  static getDerivedStateFromError(error: Error) {
    return { error };
  }

  render() {
    if (this.state.error) {
      return (
        <div style={{ minHeight: '100vh', display: 'grid', placeItems: 'center', padding: 24, background: '#090f12', color: '#e8edf2', fontFamily: "'DM Sans', sans-serif" }}>
          <div style={{ maxWidth: 480 }}>
            <h1 style={{ fontFamily: "'Fraunces', serif", fontSize: 28, margin: '0 0 10px' }}>The console stopped responding.</h1>
            <p style={{ color: '#7b8a94', fontSize: 13, lineHeight: 1.6, margin: '0 0 18px' }}>
              Something in the dashboard failed to render. Reload to reconnect to the telemetry stream.
            </p>
            <pre style={{ background: '#111a1d', border: '1px solid #26353a', borderRadius: 10, padding: 12, fontSize: 11, color: '#e5573d', overflowX: 'auto' }}>
              {String(this.state.error?.message || this.state.error)}
            </pre>
            <button
              onClick={() => window.location.reload()}
              style={{ marginTop: 16, border: 0, borderRadius: 8, background: '#e8edf2', color: '#090f12', padding: '11px 18px', fontSize: 12, cursor: 'pointer' }}
            >
              Reload
            </button>
          </div>
        </div>
      );
    }
    return this.props.children;
  }
}
