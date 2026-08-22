import React, { useState } from 'react';
import { X, Server, Wifi, Cpu, Check, RefreshCw } from 'lucide-react';

export function SettingsModal({
  isOpen,
  onClose,
  baseUrl,
  setBaseUrl,
  wsUrl,
  setWsUrl,
  isDemoMode,
  setIsDemoMode,
  onTestConnection,
}) {
  const [testResult, setTestResult] = useState(null);
  const [isTesting, setIsTesting] = useState(false);

  if (!isOpen) return null;

  const handleTest = async () => {
    setIsTesting(true);
    setTestResult(null);
    try {
      const result = await onTestConnection();
      setTestResult({ success: true, message: `Server online! ${JSON.stringify(result)}` });
    } catch (err) {
      setTestResult({ success: false, message: `Connection failed: ${err.message}` });
    } finally {
      setIsTesting(false);
    }
  };

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center p-4 bg-slate-950/80 backdrop-blur-md animate-fadeIn">
      <div className="glass-panel w-full max-w-lg rounded-2xl border border-slate-800 p-6 shadow-2xl relative">
        {/* Header */}
        <div className="flex items-center justify-between pb-4 border-b border-slate-800">
          <div className="flex items-center gap-3">
            <div className="p-2 rounded-xl bg-cyan-500/10 border border-cyan-500/30 text-cyan-400">
              <Server className="w-5 h-5" />
            </div>
            <div>
              <h3 className="font-bold text-slate-100 text-base">SWAP Server Settings</h3>
              <p className="text-xs text-slate-400">Configure API endpoints & simulation preferences</p>
            </div>
          </div>
          <button
            onClick={onClose}
            className="p-1.5 rounded-lg text-slate-400 hover:text-slate-200 hover:bg-slate-800"
          >
            <X className="w-5 h-5" />
          </button>
        </div>

        {/* Content */}
        <div className="space-y-4 py-5">
          {/* Mode Switcher */}
          <div className="p-3.5 rounded-xl bg-slate-900/80 border border-slate-800 flex items-center justify-between">
            <div className="flex items-center gap-3">
              <Cpu className="w-5 h-5 text-amber-400" />
              <div>
                <div className="text-xs font-bold text-slate-200">Interactive Demo Simulator</div>
                <div className="text-[11px] text-slate-400">
                  Generate synthetic telemetry if local server is offline
                </div>
              </div>
            </div>
            <button
              type="button"
              onClick={() => setIsDemoMode(!isDemoMode)}
              className={`w-12 h-6 rounded-full transition-all relative ${
                isDemoMode ? 'bg-amber-500' : 'bg-slate-700'
              }`}
            >
              <span
                className={`absolute top-1 left-1 w-4 h-4 rounded-full bg-white transition-all ${
                  isDemoMode ? 'translate-x-6' : 'translate-x-0'
                }`}
              />
            </button>
          </div>

          {/* REST API URL */}
          <div>
            <label className="text-xs font-semibold text-slate-300 block mb-1">REST API Base URL</label>
            <input
              type="text"
              value={baseUrl}
              onChange={(e) => setBaseUrl(e.target.value)}
              className="w-full px-3.5 py-2.5 rounded-xl bg-slate-900 border border-slate-800 font-mono text-xs text-slate-100 focus:border-cyan-500 focus:outline-none"
              placeholder="http://localhost:8000"
            />
          </div>

          {/* WebSocket URL */}
          <div>
            <label className="text-xs font-semibold text-slate-300 block mb-1">WebSocket Live Endpoint</label>
            <input
              type="text"
              value={wsUrl}
              onChange={(e) => setWsUrl(e.target.value)}
              className="w-full px-3.5 py-2.5 rounded-xl bg-slate-900 border border-slate-800 font-mono text-xs text-slate-100 focus:border-cyan-500 focus:outline-none"
              placeholder="ws://localhost:8000/ws/live"
            />
          </div>

          {/* Test connection results */}
          {testResult && (
            <div
              className={`p-3 rounded-xl border text-xs font-mono ${
                testResult.success
                  ? 'bg-emerald-500/10 border-emerald-500/30 text-emerald-400'
                  : 'bg-rose-500/10 border-rose-500/30 text-rose-400'
              }`}
            >
              {testResult.message}
            </div>
          )}
        </div>

        {/* Footer */}
        <div className="flex items-center justify-between pt-4 border-t border-slate-800">
          <button
            onClick={handleTest}
            disabled={isTesting}
            className="px-4 py-2 rounded-xl bg-slate-900 border border-slate-700 hover:border-cyan-500/50 text-xs font-bold text-slate-300 hover:text-cyan-400 flex items-center gap-2"
          >
            {isTesting ? <RefreshCw className="w-3.5 h-3.5 animate-spin" /> : <Wifi className="w-3.5 h-3.5" />}
            Test Health Endpoint
          </button>

          <button
            onClick={onClose}
            className="px-5 py-2 rounded-xl bg-gradient-to-r from-cyan-500 to-blue-600 text-white text-xs font-bold shadow-md shadow-cyan-500/20 hover:opacity-95"
          >
            Save & Close
          </button>
        </div>
      </div>
    </div>
  );
}
