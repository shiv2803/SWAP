import React from 'react';
import { ToastProvider, useToast } from './hooks/useToast.jsx';
import { SettingsProvider, useSettings } from './hooks/useSettings.jsx';
import { useWebSocket } from './hooks/useWebSocket';
import Header from './components/Header';
import ControlPanel from './components/ControlPanel';
import TelemetryPanel from './components/TelemetryPanel';
import PowerPanel from './components/PowerPanel';
import LogPanel from './components/LogPanel';
import NodesPanel from './components/NodesPanel';
import Footer from './components/Footer';
import SettingsModal from './components/SettingsModal';
import ConnectionBanner from './components/ConnectionBanner';
import NodeDetailModal from './components/NodeDetailModal';

const MODES = {
  0: { id: 0, name: 'IDLE', icon: '■', label: 'IDLE', category: 'ctrl', desc: 'Stop all hindrance activity' },
  1: { id: 1, name: 'WIFI_DEAUTH', icon: '⛬', label: 'DEAUTH', category: 'wifi', desc: '802.11 deauthentication flood' },
  2: { id: 2, name: 'WIFI_BEACON', icon: '⛭', label: 'BEACON', category: 'wifi', desc: 'Beacon frame spam' },
  3: { id: 3, name: 'WIFI_PROBE', icon: '⛯', label: 'PROBE', category: 'wifi', desc: 'Probe request spam' },
  4: { id: 4, name: 'WIFI_AUTH', icon: '⛮', label: 'AUTH', category: 'wifi', desc: 'Authentication frame flood' },
  5: { id: 5, name: 'BLE_ADV', icon: '⎈', label: 'ADV SPAM', category: 'ble', desc: 'BLE advertisement spam' },
  6: { id: 6, name: 'BLE_CONN', icon: '⎇', label: 'CONN FLD', category: 'ble', desc: 'BLE connection flood' },
  7: { id: 7, name: 'BLE_PAIR', icon: '⎘', label: 'PAIR SPM', category: 'ble', desc: 'BLE pairing spam' },
  8: { id: 8, name: 'LORA_COLLISION', icon: '⚡', label: 'COLLISION', category: 'lora', desc: 'LoRa packet collision injection' },
  9: { id: 9, name: 'LORA_PREAMBLE', icon: '≋', label: 'PREAMBLE', category: 'lora', desc: 'Long preamble jamming' },
  10: { id: 10, name: 'LORA_DUTY', icon: '☢', label: 'DUTY ABUSE', category: 'lora', desc: 'Exceed 1% duty cycle limit' },
  11: { id: 11, name: 'ALL_CHAOS', icon: '☠', label: 'CHAOS', category: 'all', desc: 'Cycle all radios simultaneously' }
};

const TARGETS = [
  { key: 'wifi', label: 'WIFI', bit: 1, color: 'var(--green)' },
  { key: 'ble', label: 'BLE', bit: 2, color: 'var(--amber)' },
  { key: 'lora', label: 'LORA', bit: 4, color: '#00BFFF' }
];

const CATEGORY_LABELS = {
  wifi: 'WiFi',
  ble: 'BLE',
  lora: 'LoRa',
  all: 'ALL',
  ctrl: 'CTRL'
};

function AppContent() {
  const { showToast } = useToast();
  const { settings } = useSettings();
  const {
    connected,
    connecting,
    connectionError,
    nodes,
    nodeList,
    modes,
    targetsConfig,
    logs,
    selectedNode,
    setSelectedNode,
    sendCommand,
    setMode,
    setTargets,
    requestStatus,
    connect,
    disconnect
  } = useWebSocket();

  const allModes = React.useMemo(() => ({ ...MODES, ...modes }), [modes]);
  const targetBits = React.useMemo(() => ({ ...TARGETS.reduce((acc, t) => ({ ...acc, [t.key]: t.bit }), {}) }), []);

  const [currentMode, setCurrentMode] = React.useState(0);
  const [activeTargets, setActiveTargets] = React.useState(7);
  const [hindranceRunning, setHindranceRunning] = React.useState(false);
  const [showSettings, setShowSettings] = React.useState(false);
  const [showNodeDetail, setShowNodeDetail] = React.useState(null);
  const [logFilter, setLogFilter] = React.useState('all');
  const [lastAction, setLastAction] = React.useState(null);

  // Keyboard shortcuts
  React.useEffect(() => {
    const handleKeyDown = (e) => {
      if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') return;

      const modeKeys = { '1': 1, '2': 2, '3': 3, '4': 4, '5': 5, '6': 6, '7': 7, '8': 8, '9': 9, '0': 0 };
      
      if (modeKeys[e.key] !== undefined) {
        e.preventDefault();
        handleModeSelect(modeKeys[e.key]);
        return;
      }

      switch (e.key.toLowerCase()) {
        case 's': handleStop(); break;
        case 'r': handleReset(); break;
        case 'w': handleTargetToggle('wifi'); break;
        case 'b': handleTargetToggle('ble'); break;
        case 'l': handleTargetToggle('lora'); break;
        case 'a': handleTargetToggleAll(); break;
        case '?': setShowSettings(true); break;
        case 'escape': setShowSettings(false); setShowNodeDetail(null); break;
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [selectedNode, activeTargets, hindranceRunning]);

  // Auto-request status periodically
  React.useEffect(() => {
    if (!connected) return;
    const interval = setInterval(() => requestStatus(), settings.heartbeatInterval * 2);
    return () => clearInterval(interval);
  }, [connected, requestStatus, settings.heartbeatInterval]);

  // Notify on connection changes
  React.useEffect(() => {
    if (connected) {
      showToast('Connected to backend', 'success', 2000);
    }
  }, [connected, showToast]);

  React.useEffect(() => {
    if (connectionError) {
      showToast(connectionError, 'error', 0);
    }
  }, [connectionError, showToast]);

  const handleModeSelect = (modeId) => {
    setCurrentMode(modeId);
    setHindranceRunning(modeId !== 0);
    const success = setMode(selectedNode, modeId, activeTargets);
    if (!success) {
      showToast('Failed to send mode command - not connected', 'error');
    } else {
      const modeName = allModes[modeId]?.label || 'UNKNOWN';
      setLastAction({ type: 'mode', value: modeName });
      showToast(`Mode set: ${modeName}`, 'success');
    }
  };

  const handleTargetToggle = (key) => {
    const bit = targetBits[key] || TARGETS.find(t => t.key === key)?.bit || 0;
    setActiveTargets(prev => {
      const next = prev & bit ? prev & ~bit : prev | bit;
      setTargets(selectedNode, next);
      return next;
    });
    const targetName = TARGETS.find(t => t.key === key)?.label || key;
    const enabled = (activeTargets & bit) === 0;
    showToast(`${targetName} ${enabled ? 'enabled' : 'disabled'}`, enabled ? 'success' : 'warning');
  };

  const handleTargetToggleAll = () => {
    const next = activeTargets === 7 ? 0 : 7;
    setActiveTargets(next);
    setTargets(selectedNode, next);
    showToast(next === 7 ? 'All targets enabled' : 'All targets disabled', next === 7 ? 'success' : 'warning');
  };

  const handleStop = () => {
    setCurrentMode(0);
    setHindranceRunning(false);
    sendCommand(selectedNode, '0');
    showToast('All hindrance stopped', 'warning');
  };

  const handleReset = () => {
    sendCommand(selectedNode, 'r');
    showToast('Counters reset', 'info');
  };

  const handleStatus = () => {
    requestStatus();
    showToast('Status requested', 'info');
  };

  const handleNodeClick = (nodeId) => {
    setShowNodeDetail(nodeId);
  };

  const filteredLogs = React.useMemo(() => {
    if (logFilter === 'all') return logs;
    return logs.filter(l => (l.level || 'info') === logFilter);
  }, [logs, logFilter]);

  // Get selected node's status
  const selectedNodeData = nodes[selectedNode] || {};
  const nodeStatus = selectedNodeData.status || {};
  const telemetry = nodeStatus.telemetry || {};
  const power = nodeStatus.power || {};

  const connectedNodes = nodeList.filter(n => n.ws?.readyState === 1).length;
  const totalNodes = nodeList.length;

  return (
    <div className="app">
      <ConnectionBanner
        connected={connected}
        connecting={connecting}
        error={connectionError}
        onRetry={connect}
        onDismiss={() => {}}
      />

      <Header
        connected={connected}
        connecting={connecting}
        nodes={nodes}
        nodeList={nodeList}
        selectedNode={selectedNode}
        onSelectNode={setSelectedNode}
        onNodeClick={handleNodeClick}
        showSettings={showSettings}
        onToggleSettings={setShowSettings}
        connectedCount={connectedNodes}
        totalCount={totalNodes}
      />

      <main className="main">
        <section className="panel" id="control-panel">
          <ControlPanel
            modes={allModes}
            currentMode={currentMode}
            onModeSelect={handleModeSelect}
            targets={TARGETS}
            activeTargets={activeTargets}
            onTargetToggle={handleTargetToggle}
            onStop={handleStop}
            onStatus={handleStatus}
            onReset={handleReset}
            hindranceRunning={hindranceRunning}
            lastAction={lastAction}
            categories={CATEGORY_LABELS}
          />
        </section>

        <aside>
          <TelemetryPanel
            nodeId={selectedNode}
            nodeData={selectedNodeData}
            telemetry={telemetry}
            currentMode={currentMode}
            modes={allModes}
            connected={connected}
          />

          <PowerPanel
            nodeId={selectedNode}
            power={power}
            connected={connected}
          />

          <LogPanel
            logs={filteredLogs}
            filter={logFilter}
            onFilterChange={setLogFilter}
            maxLogs={settings.maxLogs}
          />
        </aside>
      </main>

      <Footer
        version="1.0.0"
        connected={connected}
        nodeCount={`${connectedNodes}/${totalNodes}`}
      />

      {showSettings && (
        <SettingsModal
          isOpen={showSettings}
          onClose={() => setShowSettings(false)}
        />
      )}

      {showNodeDetail && (
        <NodeDetailModal
          nodeId={showNodeDetail}
          nodeData={nodes[showNodeDetail]}
          onClose={() => setShowNodeDetail(null)}
        />
      )}
    </div>
  );
}

function App() {
  return (
    <SettingsProvider>
      <ToastProvider>
        <AppContent />
      </ToastProvider>
    </SettingsProvider>
  );
}

export default App;