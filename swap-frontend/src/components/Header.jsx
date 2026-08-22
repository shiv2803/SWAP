import React from 'react';
import { Activity, Radio, Cpu, Settings, Download, ShieldCheck, UserCheck } from 'lucide-react';
import { useRole } from '../context/RoleContext';
import { exportTelemetryCsv } from '../utils/csvExporter';

export function Header({
  connectionStatus,
  isDemoMode,
  setIsDemoMode,
  selectedNodeFilter,
  setSelectedNodeFilter,
  onOpenSettings,
  historyRecords,
}) {
  const { role, setRole, permissions, ROLES, ROLE_PERMISSIONS } = useRole();

  const handleExport = () => {
    exportTelemetryCsv(historyRecords, `swap_telemetry_export_${Date.now()}.csv`);
  };

  return (
    <header className="sticky top-0 z-40 w-full glass-panel border-b border-slate-800/80 px-4 lg:px-8 py-3">
      <div className="max-w-7xl mx-auto flex flex-col md:flex-row items-center justify-between gap-4">
        
        {/* Brand / Logo */}
        <div className="flex items-center gap-3 w-full md:w-auto justify-between md:justify-start">
          <div className="flex items-center gap-3">
            <div className="relative flex items-center justify-center w-10 h-10 rounded-xl bg-gradient-to-tr from-cyan-500 via-indigo-500 to-purple-600 p-0.5 shadow-lg shadow-cyan-500/20">
              <div className="w-full h-full bg-slate-950 rounded-[10px] flex items-center justify-center">
                <Radio className="w-5 h-5 text-cyan-400 animate-pulse" />
              </div>
            </div>
            <div>
              <div className="flex items-center gap-2">
                <h1 className="font-bold text-lg tracking-tight bg-gradient-to-r from-slate-100 via-cyan-200 to-cyan-400 bg-clip-text text-transparent">
                  SWAP Platform
                </h1>
                <span className="text-[10px] font-mono px-2 py-0.5 rounded-md bg-cyan-500/10 text-cyan-400 border border-cyan-500/30">
                  {permissions.name} View
                </span>
              </div>
              <p className="text-xs text-slate-400">Smart Wireless Access Protocol Dashboard</p>
            </div>
          </div>

          {/* Mobile status */}
          <div className="md:hidden flex items-center gap-2">
            <StatusBadge connectionStatus={connectionStatus} isDemoMode={isDemoMode} />
          </div>
        </div>

        {/* Center: Persona Role Switcher & Node Filter */}
        <div className="flex items-center gap-3">
          {/* Persona Role Dropdown */}
          <div className="flex items-center bg-slate-900/90 p-1 rounded-xl border border-slate-800 text-xs">
            {Object.entries(ROLE_PERMISSIONS).map(([key, item]) => (
              <button
                key={key}
                onClick={() => setRole(key)}
                className={`px-3 py-1.5 rounded-lg font-medium transition-all flex items-center gap-1.5 ${
                  role === key
                    ? 'bg-slate-800 text-cyan-400 font-bold border border-slate-700 shadow-sm'
                    : 'text-slate-400 hover:text-slate-200'
                }`}
                title={item.description}
              >
                <span>{item.icon}</span>
                <span className="hidden sm:inline">{item.name}</span>
              </button>
            ))}
          </div>

          {/* Node Filter Tabs */}
          <div className="hidden sm:flex items-center bg-slate-900/90 p-1 rounded-xl border border-slate-800/80">
            {[
              { id: 'all', label: 'All' },
              { id: 'a', label: 'Node A' },
              { id: 'b', label: 'Node B' },
            ].map((tab) => (
              <button
                key={tab.id}
                onClick={() => setSelectedNodeFilter(tab.id)}
                className={`px-2.5 py-1 rounded-lg text-xs font-medium transition-all ${
                  selectedNodeFilter === tab.id
                    ? 'bg-gradient-to-r from-cyan-500 to-blue-600 text-white font-bold'
                    : 'text-slate-400 hover:text-slate-200'
                }`}
              >
                {tab.label}
              </button>
            ))}
          </div>
        </div>

        {/* Right Actions (Role Gated) */}
        <div className="hidden md:flex items-center gap-3">
          {/* Export CSV (Analyst / Admin) */}
          {permissions.canExportCsv && (
            <button
              onClick={handleExport}
              className="flex items-center gap-1.5 px-3 py-1.5 rounded-xl bg-slate-900 border border-slate-800 hover:border-cyan-500/40 text-xs font-semibold text-slate-300 hover:text-cyan-400 transition-all shadow-sm"
              title="Download collected telemetry CSV"
            >
              <Download className="w-3.5 h-3.5" />
              Export CSV
            </button>
          )}

          {/* Demo Mode Toggle (Admin / Analyst) */}
          {permissions.canConfigureSettings && (
            <button
              onClick={() => setIsDemoMode(!isDemoMode)}
              className={`flex items-center gap-2 px-3 py-1.5 rounded-xl text-xs font-semibold border transition-all ${
                isDemoMode
                  ? 'bg-amber-500/10 border-amber-500/40 text-amber-400 hover:bg-amber-500/20'
                  : 'bg-slate-800/60 border-slate-700 text-slate-300 hover:bg-slate-800'
              }`}
            >
              <Cpu className={`w-3.5 h-3.5 ${isDemoMode ? 'text-amber-400 animate-spin' : ''}`} />
              {isDemoMode ? 'Demo' : 'Live'}
            </button>
          )}

          {/* Connection Status Badge */}
          <StatusBadge connectionStatus={connectionStatus} isDemoMode={isDemoMode} />

          {/* Settings Modal Button (Admin Only) */}
          {permissions.canConfigureSettings && (
            <button
              onClick={onOpenSettings}
              className="p-2 rounded-xl bg-slate-900 border border-slate-800 text-slate-400 hover:text-cyan-400 hover:border-cyan-500/40 transition-all"
              title="Server Configuration"
            >
              <Settings className="w-4 h-4" />
            </button>
          )}
        </div>
      </div>
    </header>
  );
}

function StatusBadge({ connectionStatus, isDemoMode }) {
  if (isDemoMode) {
    return (
      <div className="flex items-center gap-2 px-3 py-1 rounded-full bg-amber-500/10 border border-amber-500/30 text-amber-400 text-xs font-mono">
        <span className="w-2 h-2 rounded-full bg-amber-400 animate-pulse" />
        Demo Stream
      </div>
    );
  }

  const isConnected = connectionStatus === 'connected';
  const isError = connectionStatus === 'error' || connectionStatus === 'disconnected';

  return (
    <div
      className={`flex items-center gap-2 px-3 py-1 rounded-full border text-xs font-mono transition-all ${
        isConnected
          ? 'bg-emerald-500/10 border-emerald-500/30 text-emerald-400'
          : isError
          ? 'bg-rose-500/10 border-rose-500/30 text-rose-400'
          : 'bg-slate-800 border-slate-700 text-slate-400'
      }`}
    >
      <span
        className={`w-2 h-2 rounded-full ${
          isConnected ? 'bg-emerald-400 animate-pulse-live' : 'bg-rose-500'
        }`}
      />
      {isConnected ? 'Connected' : 'Connecting...'}
    </div>
  );
}
