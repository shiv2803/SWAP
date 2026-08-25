import React from 'react';

export default function Header({
  connected,
  connecting,
  nodeList,
  selectedNode,
  onSelectNode,
  onNodeClick,
  showSettings,
  onToggleSettings,
  connectedCount,
  totalCount
}) {
  return (
    <header className="header">
      <div className="header-left">
        <div className="node-selector" role="group" aria-label="Node selection">
          {nodeList.length === 0 ? (
            <span className="node-empty">NO NODES CONNECTED</span>
          ) : (
            <>
              {nodeList.map(node => (
                <button
                  key={node.id}
                  className={`node-btn ${selectedNode === node.id ? 'active' : ''}`}
                  onClick={() => onSelectNode(node.id)}
                  onDoubleClick={() => onNodeClick?.(node.id)}
                  disabled={node.ws?.readyState !== 1}
                  aria-pressed={selectedNode === node.id}
                  aria-disabled={node.ws?.readyState !== 1}
                  title={node.ws?.readyState !== 1 ? 'Node offline' : `Select ${node.id} (${node.info?.role || 'UNKNOWN'})`}
                >
                  <span className="node-id">{node.id}</span>
                  {node.info?.role && <span className="node-role">{node.info.role}</span>}
                  <span className={`node-status-indicator ${node.ws?.readyState === 1 ? 'online' : 'offline'}`} aria-hidden="true"></span>
                </button>
              ))}
              {nodeList.length > 1 && (
                <button
                  className={`node-btn all-nodes ${selectedNode === 'all' ? 'active' : ''}`}
                  onClick={() => onSelectNode('all')}
                  aria-pressed={selectedNode === 'all'}
                  title="All nodes"
                >
                  <span className="node-id">ALL</span>
                  <span className="node-count">{connectedCount}/{totalCount}</span>
                </button>
              )}
            </>
          )}
        </div>
      </div>

      <h1 className="title">
        SWAP <span className="title-accent">HINDRANCE</span> CONTROL
      </h1>

      <div className="header-right">
        <div className="connection-status" role="status" aria-live="polite">
          <span className="status-text">
            {connecting ? 'CONNECTING...' : connected ? 'CONNECTED' : 'DISCONNECTED'}
          </span>
          <div className={`status-dot ${connected ? 'connected' : connecting ? 'connecting' : ''}`} aria-hidden="true"></div>
        </div>

        <button
          className={`icon-btn ${showSettings ? 'active' : ''}`}
          onClick={() => onToggleSettings(!showSettings)}
          aria-pressed={showSettings}
          aria-label={showSettings ? 'Close settings' : 'Open settings'}
          title="Settings (?)"
        >
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" aria-hidden="true">
            <circle cx="12" cy="12" r="3" />
            <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z" />
          </svg>
        </button>
      </div>
    </header>
  );
}