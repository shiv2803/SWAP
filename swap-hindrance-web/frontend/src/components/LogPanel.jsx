import React from 'react';

const LEVEL_STYLES = {
  ok: { label: 'OK', color: 'var(--green)' },
  success: { label: 'OK', color: 'var(--green)' },
  warn: { label: 'WARN', color: 'var(--amber)' },
  warning: { label: 'WARN', color: 'var(--amber)' },
  error: { label: 'ERR', color: 'var(--accent)' },
  info: { label: 'INFO', color: 'var(--fg)' },
  debug: { label: 'DBG', color: 'var(--fg-muted)' }
};

const LEVEL_ORDER = ['error', 'warn', 'warning', 'ok', 'success', 'info', 'debug'];

export default function LogPanel({ logs, filter, onFilterChange, maxLogs = 500 }) {
  const [autoScroll, setAutoScroll] = React.useState(true);
  const logContainerRef = React.useRef(null);
  const [expandedLog, setExpandedLog] = React.useState(null);

  const filteredLogs = React.useMemo(() => {
    if (filter === 'all') return logs.slice(0, maxLogs);
    return logs.filter(l => (l.level || 'info') === filter).slice(0, maxLogs);
  }, [logs, filter, maxLogs]);

  const levelCounts = React.useMemo(() => {
    const counts = {};
    logs.forEach(l => {
      const level = l.level || 'info';
      counts[level] = (counts[level] || 0) + 1;
    });
    return counts;
  }, [logs]);

  const handleScroll = () => {
    if (!logContainerRef.current) return;
    const { scrollTop, scrollHeight, clientHeight } = logContainerRef.current;
    const atBottom = scrollHeight - scrollTop - clientHeight < 50;
    setAutoScroll(atBottom);
  };

  // Auto-scroll when new logs arrive
  React.useEffect(() => {
    if (autoScroll && logContainerRef.current) {
      logContainerRef.current.scrollTop = logContainerRef.current.scrollHeight;
    }
  }, [filteredLogs.length, autoScroll]);

  if (filteredLogs.length === 0) {
    return (
      <section className="panel">
        <div className="panel-title">
          <span>EVENT LOG</span>
          <LogFilterBar
            filter={filter}
            onFilterChange={onFilterChange}
            levelCounts={levelCounts}
            total={logs.length}
          />
        </div>
        <div className="log-container empty-state">
          <span className="empty-icon">📋</span>
          <span>NO EVENTS</span>
          <span className="empty-hint">Waiting for node activity...</span>
        </div>
      </section>
    );
  }

  return (
    <section className="panel">
      <div className="panel-title">
        <span>EVENT LOG</span>
        <div className="panel-title-right">
          <LogFilterBar
            filter={filter}
            onFilterChange={onFilterChange}
            levelCounts={levelCounts}
            total={logs.length}
          />
          <label className="auto-scroll-toggle">
            <input
              type="checkbox"
              checked={autoScroll}
              onChange={e => setAutoScroll(e.target.checked)}
              aria-label="Auto-scroll"
            />
            <span className="toggle-slider"></span>
            <span className="toggle-label">AUTO</span>
          </label>
          <button
            className="icon-btn clear-logs"
            onClick={() => {}}
            aria-label="Clear logs"
            title="Clear logs"
          >
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
              <polyline points="3 6 5 6 21 6" />
              <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
            </svg>
          </button>
        </div>
      </div>

      <div
        className="log-container"
        ref={logContainerRef}
        onScroll={handleScroll}
        role="log"
        aria-live="polite"
        aria-label="Event log"
      >
        {filteredLogs.map((log, idx) => {
          const level = log.level || 'info';
          const style = LEVEL_STYLES[level] || LEVEL_STYLES.info;
          const timestamp = log.timestamp ? new Date(log.timestamp).toLocaleTimeString() : '';
          const nodeId = log.nodeId && log.nodeId !== 'all' ? log.nodeId : null;
          const message = log.message || log.data || JSON.stringify(log);
          const isExpanded = expandedLog === idx;

          return (
            <div
              key={idx}
              className={`log-entry ${level} ${isExpanded ? 'expanded' : ''}`}
              onClick={() => setExpandedLog(isExpanded ? null : idx)}
            >
              <span className="log-time" title={log.timestamp ? new Date(log.timestamp).toISOString() : ''}>
                {timestamp}
              </span>
              {nodeId && (
                <span className="log-node" style={{ '--node-color': getNodeColor(nodeId) }}>
                  [{nodeId}]
                </span>
              )}
              <span
                className="log-type"
                style={{ color: style.color, borderColor: style.color }}
              >
                [{style.label}]
              </span>
              <span className="log-msg">{message}</span>
              {isExpanded && log.timestamp && (
                <div className="log-expanded">
                  <div className="log-meta">
                    <span>Timestamp: {new Date(log.timestamp).toISOString()}</span>
                    {nodeId && <span>Node: {nodeId}</span>}
                    <span>Level: {level.toUpperCase()}</span>
                  </div>
                </div>
              )}
            </div>
          );
        })}
      </div>
    </section>
  );
}

function LogFilterBar({ filter, onFilterChange, levelCounts, total }) {
  const levels = ['all', 'error', 'warn', 'ok', 'info'];

  return (
    <div className="log-filter-bar" role="group" aria-label="Log level filter">
      {levels.map(level => {
        const count = level === 'all' ? total : (levelCounts[level] || 0);
        const isActive = filter === level;
        return (
          <button
            key={level}
            className={`filter-btn ${isActive ? 'active' : ''} ${level}`}
            onClick={() => onFilterChange(level)}
            aria-pressed={isActive}
          >
            <span className="filter-label">{level.toUpperCase()}</span>
            {count > 0 && <span className="filter-count">{count}</span>}
          </button>
        );
      })}
    </div>
  );
}

function getNodeColor(nodeId) {
  const colors = ['var(--green)', 'var(--amber)', '#00BFFF', 'var(--accent)', '#FF00FF', '#00FFFF'];
  let hash = 0;
  for (let i = 0; i < nodeId.length; i++) {
    hash = nodeId.charCodeAt(i) + ((hash << 5) - hash);
  }
  return colors[Math.abs(hash) % colors.length];
}