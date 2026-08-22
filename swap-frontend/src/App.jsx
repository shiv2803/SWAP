import React, { useState, useEffect, useRef } from 'react';
import { RoleProvider, useRole } from './context/RoleContext';
import { Header } from './components/Header';
import { NodeStatusCard } from './components/NodeStatusCard';
import { ProtocolController } from './components/ProtocolController';
import { TelemetryCharts } from './components/TelemetryCharts';
import { EventLog } from './components/EventLog';
import { NetworkTopology } from './components/NetworkTopology';
import { SlaMonitor } from './components/SlaMonitor';
import { SettingsModal } from './components/SettingsModal';
import { SwapApiClient, SwapWebSocketClient } from './services/api';
import { TelemetrySimulator } from './services/mockSimulator';
import { Activity, Radio, Cpu, ShieldCheck, Zap, Info, Lock } from 'lucide-react';

const PROTOCOL_NAMES = { 0: 'WiFi', 1: 'BLE', 2: 'LoRa' };

export default function AppWrapper() {
  return (
    <RoleProvider>
      <AppContent />
    </RoleProvider>
  );
}

function AppContent() {
  const { permissions, role } = useRole();

  const [baseUrl, setBaseUrl] = useState('http://localhost:8000');
  const [wsUrl, setWsUrl] = useState('ws://localhost:8000/ws/live');
  const [isDemoMode, setIsDemoMode] = useState(true);
  const [connectionStatus, setConnectionStatus] = useState('disconnected');
  const [selectedNodeFilter, setSelectedNodeFilter] = useState('all');
  const [isSettingsOpen, setIsSettingsOpen] = useState(false);

  // Live State
  const [latestNodeData, setLatestNodeData] = useState({
    a: { record: null, decision: null },
    b: { record: null, decision: null },
  });
  const [historyRecords, setHistoryRecords] = useState([]);
  const [eventLogs, setEventLogs] = useState([]);
  const [frameCount, setFrameCount] = useState(0);

  // API & WebSocket Clients
  const apiClientRef = useRef(new SwapApiClient(baseUrl));
  const wsClientRef = useRef(null);
  const simulatorRef = useRef(null);

  useEffect(() => {
    apiClientRef.current = new SwapApiClient(baseUrl);
  }, [baseUrl]);

  // Handle Incoming Telemetry Frame
  const handleIncomingFrame = (payload) => {
    const { record, decision } = payload;
    if (!record || !record.node) return;

    const nodeKey = record.node.toLowerCase();
    setFrameCount((prev) => prev + 1);

    setLatestNodeData((prev) => ({
      ...prev,
      [nodeKey]: {
        record,
        decision: decision || prev[nodeKey].decision,
      },
    }));

    setHistoryRecords((prev) => {
      const next = [...prev, record];
      if (next.length > 200) return next.slice(next.length - 200);
      return next;
    });

    if (decision && decision.protocol !== record.active_protocol) {
      setEventLogs((prev) => {
        const msg = `AI Recommended handover to ${PROTOCOL_NAMES[decision.protocol]} (${Math.round(decision.confidence * 100)}% conf)`;
        const exists = prev.length > 0 && prev[0].message === msg;
        if (exists) return prev;
        return [
          {
            id: `${Date.now()}-${Math.random()}`,
            node: nodeKey,
            type: 'ai_decision',
            timestamp: new Date().toLocaleTimeString(),
            message: msg,
            details: { source: decision.source, confidence: `${Math.round(decision.confidence * 100)}%` },
          },
          ...prev.slice(0, 49),
        ];
      });
    }
  };

  useEffect(() => {
    if (isDemoMode) {
      if (wsClientRef.current) wsClientRef.current.disconnect();
      setConnectionStatus('connected');

      simulatorRef.current = new TelemetrySimulator((payload) => {
        handleIncomingFrame(payload);
      });
      simulatorRef.current.start(400);

      setEventLogs((prev) => [
        {
          id: `${Date.now()}-init`,
          node: 'system',
          type: 'info',
          timestamp: new Date().toLocaleTimeString(),
          message: 'Client Telemetry Simulator Active (Demo Mode)',
        },
        ...prev,
      ]);

      return () => {
        if (simulatorRef.current) simulatorRef.current.stop();
      };
    } else {
      if (simulatorRef.current) simulatorRef.current.stop();

      wsClientRef.current = new SwapWebSocketClient(
        wsUrl,
        (payload) => handleIncomingFrame(payload),
        (status) => setConnectionStatus(status)
      );
      wsClientRef.current.connect();

      return () => {
        if (wsClientRef.current) wsClientRef.current.disconnect();
      };
    }
  }, [isDemoMode, wsUrl]);

  // Force Protocol Handler
  const handleForceProtocol = async (node, protocol) => {
    if (!permissions.canOverrideProtocols) {
      alert('Access Denied: Standard Users cannot force manual protocol handovers.');
      return;
    }

    let result;
    if (isDemoMode && simulatorRef.current) {
      result = simulatorRef.current.forceProtocol(node, protocol);
    } else {
      result = await apiClientRef.current.forceProtocol(node, protocol);
    }

    const protoName = PROTOCOL_NAMES[protocol];
    setEventLogs((prev) => [
      {
        id: `${Date.now()}-force`,
        node: node,
        type: 'force',
        timestamp: new Date().toLocaleTimeString(),
        message: `Forced Protocol Override -> ${protoName}`,
        details: { protocol: protoName, node: node.toUpperCase() },
      },
      ...prev.slice(0, 49),
    ]);

    return result;
  };

  const handleDecideProtocol = async (node) => {
    let result;
    if (isDemoMode && simulatorRef.current) {
      result = simulatorRef.current.decide(node);
    } else {
      result = await apiClientRef.current.decideProtocol(node);
    }

    setEventLogs((prev) => [
      {
        id: `${Date.now()}-decide`,
        node: node,
        type: 'ai_decision',
        timestamp: new Date().toLocaleTimeString(),
        message: `Requested AI Protocol Decision -> Recommended ${result.recommended_protocol_name}`,
        details: {
          recommended: result.recommended_protocol_name,
          confidence: `${Math.round((result.confidence || 0.9) * 100)}%`,
          forced: result.forced ? 'Yes' : 'No',
        },
      },
      ...prev.slice(0, 49),
    ]);

    return result;
  };

  const handleTestConnection = async () => {
    return apiClientRef.current.getHealth();
  };

  const nodeA = latestNodeData.a;
  const nodeB = latestNodeData.b;
  const avgRtt = Math.round(
    ((nodeA.record?.rtt_ms || 0) + (nodeB.record?.rtt_ms || 0)) / (nodeA.record && nodeB.record ? 2 : 1)
  );

  return (
    <div className="min-h-screen bg-[#07090e] bg-grid-pattern text-slate-100 flex flex-col font-sans pb-12">
      {/* Header */}
      <Header
        connectionStatus={connectionStatus}
        isDemoMode={isDemoMode}
        setIsDemoMode={setIsDemoMode}
        selectedNodeFilter={selectedNodeFilter}
        setSelectedNodeFilter={setSelectedNodeFilter}
        onOpenSettings={() => setIsSettingsOpen(true)}
        historyRecords={historyRecords}
      />

      {/* Role View Banner */}
      <div className="bg-slate-900/90 border-b border-slate-800 px-4 py-2 text-xs">
        <div className="max-w-7xl mx-auto flex items-center justify-between">
          <div className="flex items-center gap-2 text-slate-300">
            <Info className="w-4 h-4 text-cyan-400 shrink-0" />
            <span>
              Viewing as <strong className="text-cyan-400">{permissions.name}</strong>: {permissions.description}
            </span>
          </div>
          {!permissions.canOverrideProtocols && (
            <span className="hidden sm:flex items-center gap-1 font-mono text-[11px] text-amber-400">
              <Lock className="w-3 h-3" /> Manual Handover Controls Hidden
            </span>
          )}
        </div>
      </div>

      {/* Main Content */}
      <main className="max-w-7xl mx-auto px-4 lg:px-8 pt-6 flex-1 w-full space-y-6">
        
        {/* Top Summary Bar */}
        <div className="grid grid-cols-2 md:grid-cols-4 gap-4">
          <TopStatCard
            label="Frames Ingested"
            value={frameCount.toLocaleString()}
            sub="Live telemetry stream"
            icon={Activity}
            color="text-cyan-400"
          />
          <TopStatCard
            label="Avg Response Time"
            value={avgRtt ? `${avgRtt} ms` : '-'}
            sub="Cross-node latency"
            icon={Zap}
            color="text-emerald-400"
          />
          <TopStatCard
            label="Node A Connection"
            value={nodeA.record ? PROTOCOL_NAMES[nodeA.record.active_protocol] : '-'}
            sub={nodeA.record ? `${nodeA.record.wifi_rssi} dBm` : 'Awaiting data'}
            icon={Radio}
            color="text-purple-400"
          />
          <TopStatCard
            label="Node B Connection"
            value={nodeB.record ? PROTOCOL_NAMES[nodeB.record.active_protocol] : '-'}
            sub={nodeB.record ? `${nodeB.record.wifi_rssi} dBm` : 'Awaiting data'}
            icon={Radio}
            color="text-amber-400"
          />
        </div>

        {/* Network Topology Visualizer (Admin & Analyst) */}
        {permissions.canViewTopology && (
          <NetworkTopology nodeAData={nodeA} nodeBData={nodeB} />
        )}

        {/* Node Status Cards Section */}
        <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
          {(selectedNodeFilter === 'all' || selectedNodeFilter === 'a') && (
            <NodeStatusCard
              nodeName="a"
              telemetry={nodeA.record}
              decision={nodeA.decision}
              onQuickForce={handleForceProtocol}
            />
          )}

          {(selectedNodeFilter === 'all' || selectedNodeFilter === 'b') && (
            <NodeStatusCard
              nodeName="b"
              telemetry={nodeB.record}
              decision={nodeB.decision}
              onQuickForce={handleForceProtocol}
            />
          )}
        </div>

        {/* Role-Based Grid Section */}
        <div className="grid grid-cols-1 lg:grid-cols-12 gap-6">
          {/* Protocol Controller (Admin Only) */}
          {permissions.canOverrideProtocols && (
            <div className="lg:col-span-4">
              <ProtocolController
                onForceProtocol={handleForceProtocol}
                onDecideProtocol={handleDecideProtocol}
              />
            </div>
          )}

          {/* SLA Monitor */}
          {permissions.canViewSla && (
            <div className={permissions.canOverrideProtocols ? 'lg:col-span-4' : 'lg:col-span-6'}>
              <SlaMonitor nodeAData={nodeA} nodeBData={nodeB} />
            </div>
          )}

          {/* Audit Event Logs (Admin & Analyst) */}
          {permissions.canViewRawLogs && (
            <div className={permissions.canOverrideProtocols ? 'lg:col-span-4' : 'lg:col-span-6'}>
              <EventLog events={eventLogs} />
            </div>
          )}
        </div>

        {/* Live Telemetry Analytics Charts */}
        <TelemetryCharts
          historyRecords={historyRecords}
          selectedNodeFilter={selectedNodeFilter}
        />
      </main>

      {/* Settings Modal (Admin Only) */}
      {permissions.canConfigureSettings && (
        <SettingsModal
          isOpen={isSettingsOpen}
          onClose={() => setIsSettingsOpen(false)}
          baseUrl={baseUrl}
          setBaseUrl={setBaseUrl}
          wsUrl={wsUrl}
          setWsUrl={setWsUrl}
          isDemoMode={isDemoMode}
          setIsDemoMode={setIsDemoMode}
          onTestConnection={handleTestConnection}
        />
      )}
    </div>
  );
}

function TopStatCard({ label, value, sub, icon: Icon, color }) {
  return (
    <div className="glass-panel rounded-2xl p-4 border border-slate-800 flex items-center justify-between">
      <div>
        <div className="text-[11px] font-semibold text-slate-400 uppercase tracking-wider mb-0.5">{label}</div>
        <div className="font-mono font-bold text-xl text-slate-100">{value}</div>
        <div className="text-[10px] text-slate-500 font-mono mt-0.5">{sub}</div>
      </div>
      <div className={`p-2.5 rounded-xl bg-slate-900 border border-slate-800 ${color}`}>
        <Icon className="w-5 h-5" />
      </div>
    </div>
  );
}
