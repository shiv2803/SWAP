import React from 'react';
import { Wifi, Bluetooth, Radio, Cpu, Zap, ArrowUpRight, ShieldCheck, Activity, BarChart2, CheckCircle2, HelpCircle } from 'lucide-react';
import { useRole } from '../context/RoleContext';

const PROTOCOL_CONFIG = {
  0: { name: 'WiFi', icon: Wifi, color: 'text-cyan-400', badgeClass: 'badge-wifi glow-wifi', borderClass: 'border-cyan-500/30' },
  1: { name: 'BLE', icon: Bluetooth, color: 'text-purple-400', badgeClass: 'badge-ble glow-ble', borderClass: 'border-purple-500/30' },
  2: { name: 'LoRa', icon: Radio, color: 'text-amber-400', badgeClass: 'badge-lora glow-lora', borderClass: 'border-amber-500/30' },
};

export function NodeStatusCard({ nodeName, telemetry, decision, onQuickForce }) {
  const { permissions } = useRole();

  const activeProtocol = telemetry?.active_protocol ?? 0;
  const activeConfig = PROTOCOL_CONFIG[activeProtocol] || PROTOCOL_CONFIG[0];
  const ActiveIcon = activeConfig.icon;

  const recProtocol = decision?.protocol ?? activeProtocol;
  const recConfig = PROTOCOL_CONFIG[recProtocol] || PROTOCOL_CONFIG[0];
  const RecIcon = recConfig.icon;

  const confidencePct = decision?.confidence ? Math.round(decision.confidence * 100) : 0;
  const isMismatch = activeProtocol !== recProtocol;
  const lqi = decision?.lqi ?? Math.max(10, Math.min(100, Math.round(100 - (telemetry?.wifi_loss || 0) * 100)));

  const scores = decision?.raw_scores || { wifi: -70, ble: -80, lora: -90 };
  const minScore = -140;
  const maxScore = -30;
  const normalizeScore = (val) => Math.max(5, Math.min(100, Math.round(((val - minScore) / (maxScore - minScore)) * 100)));

  // Friendly descriptions for standard users
  const getRssiStatus = (val) => {
    if (!val) return 'No Signal';
    if (val > -65) return 'Excellent Signal';
    if (val > -78) return 'Good Signal';
    return 'Fair Signal';
  };

  const getRttStatus = (val) => {
    if (!val) return 'Measuring...';
    if (val < 30) return 'Ultra-Fast (<30ms)';
    if (val < 100) return 'Normal (<100ms)';
    return 'High Latency';
  };

  return (
    <div className={`glass-panel-interactive rounded-2xl p-6 relative overflow-hidden border ${activeConfig.borderClass}`}>
      {/* Dynamic Background Glow */}
      <div
        className={`absolute -right-16 -top-16 w-44 h-44 rounded-full blur-3xl opacity-20 pointer-events-none ${
          activeProtocol === 0 ? 'bg-cyan-500' : activeProtocol === 1 ? 'bg-purple-500' : 'bg-amber-500'
        }`}
      />

      {/* Top Banner */}
      <div className="flex items-center justify-between pb-4 mb-5 border-b border-slate-800/80">
        <div className="flex items-center gap-3">
          <div className="w-10 h-10 rounded-xl bg-slate-900 border border-slate-800 flex items-center justify-center font-mono font-bold text-lg text-slate-100 shadow-inner">
            {nodeName.toUpperCase()}
          </div>
          <div>
            <h3 className="font-bold text-base text-slate-100 flex items-center gap-2">
              Node {nodeName.toUpperCase()}
              <span className="w-2 h-2 rounded-full bg-emerald-400 animate-pulse-live" />
            </h3>
            <p className="text-xs text-slate-400 font-mono">
              Status: {telemetry ? 'Online & Streaming' : 'Connecting...'}
            </p>
          </div>
        </div>

        {/* Active Protocol Badge */}
        <div className="text-right">
          <span className="text-[10px] uppercase font-bold tracking-wider text-slate-400 block mb-1">
            Active Connection
          </span>
          <div className={`inline-flex items-center gap-2 px-3.5 py-1.5 rounded-full text-xs font-bold font-mono ${activeConfig.badgeClass}`}>
            <ActiveIcon className="w-3.5 h-3.5" />
            {activeConfig.name}
          </div>
        </div>
      </div>

      {/* Link Quality Index (LQI) Meter */}
      <div className="mb-5 p-3 rounded-xl bg-slate-900/80 border border-slate-800">
        <div className="flex items-center justify-between text-xs mb-1.5">
          <div className="flex items-center gap-2">
            <Activity className="w-4 h-4 text-emerald-400" />
            <span className="font-semibold text-slate-300">Connection Health Score</span>
          </div>
          <span className="font-mono font-bold text-emerald-400">{lqi}%</span>
        </div>
        <div className="w-full h-2 rounded-full bg-slate-800 overflow-hidden">
          <div
            className={`h-full rounded-full transition-all duration-500 ${
              lqi > 75 ? 'bg-gradient-to-r from-emerald-500 to-teal-400' : lqi > 40 ? 'bg-gradient-to-r from-amber-500 to-yellow-400' : 'bg-gradient-to-r from-rose-500 to-red-400'
            }`}
            style={{ width: `${lqi}%` }}
          />
        </div>
      </div>

      {/* AI Recommendation Banner */}
      <div className={`rounded-xl p-3.5 mb-5 border transition-all ${
        isMismatch
          ? 'bg-amber-500/10 border-amber-500/30'
          : decision?.hysteresis_guarded
          ? 'bg-indigo-500/10 border-indigo-500/30'
          : 'bg-slate-900/80 border-slate-800'
      }`}>
        <div className="flex items-center justify-between text-xs">
          <div className="flex items-center gap-2">
            <Cpu className="w-4 h-4 text-cyan-400" />
            <span className="text-slate-300 font-semibold">AI Link Optimizer:</span>
          </div>

          {permissions.canViewAiDetails && (
            <div className="flex items-center gap-2">
              <span className="font-mono text-slate-400 text-[11px]">
                {decision?.decision_source || 'rf_model_windowed'}
              </span>
              <span className={`font-mono text-[10px] px-2 py-0.5 rounded-md font-bold ${
                confidencePct > 80
                  ? 'bg-emerald-500/20 text-emerald-300 border border-emerald-500/40'
                  : 'bg-amber-500/20 text-amber-300 border border-amber-500/40'
              }`}>
                {confidencePct}% Conf
              </span>
            </div>
          )}
        </div>

        <div className="mt-2.5 flex items-center justify-between">
          <div className="flex items-center gap-2">
            <RecIcon className={`w-4 h-4 ${recConfig.color}`} />
            <span className={`font-mono font-bold text-sm ${recConfig.color}`}>
              {recConfig.name}
            </span>

            {isMismatch ? (
              <span className="text-[11px] px-2 py-0.5 rounded bg-amber-500/20 text-amber-300 border border-amber-500/40 font-semibold animate-pulse">
                Handover Recommended
              </span>
            ) : decision?.hysteresis_guarded ? (
              <span className="text-[11px] px-2 py-0.5 rounded bg-indigo-500/20 text-indigo-300 border border-indigo-500/40 font-semibold flex items-center gap-1">
                <ShieldCheck className="w-3 h-3 text-indigo-400" /> Guarded
              </span>
            ) : (
              <span className="text-[11px] px-2 py-0.5 rounded bg-emerald-500/20 text-emerald-300 border border-emerald-500/40 font-semibold">
                Optimal
              </span>
            )}
          </div>

          {isMismatch && permissions.canOverrideProtocols && onQuickForce && (
            <button
              onClick={() => onQuickForce(nodeName, recProtocol)}
              className="flex items-center gap-1.5 px-3 py-1 rounded-lg text-xs font-semibold bg-gradient-to-r from-cyan-500 to-blue-600 hover:from-cyan-400 hover:to-blue-500 text-white shadow-md shadow-cyan-500/20 transition-all active:scale-95"
            >
              Switch Now <ArrowUpRight className="w-3.5 h-3.5" />
            </button>
          )}
        </div>

        {/* Score Breakdown (Admin / Analyst Only) */}
        {permissions.canViewAiDetails && (
          <div className="mt-3.5 pt-3 border-t border-slate-800/80 space-y-2">
            <div className="flex items-center justify-between text-[11px] font-semibold text-slate-400">
              <span className="flex items-center gap-1">
                <BarChart2 className="w-3 h-3 text-slate-500" /> Candidate Protocol Utility
              </span>
              <span className="font-mono text-[10px]">WiFi / BLE / LoRa</span>
            </div>

            <ScoreBar label="WiFi" value={scores.wifi} percent={normalizeScore(scores.wifi)} color="bg-cyan-400" isActive={activeProtocol === 0} />
            <ScoreBar label="BLE" value={scores.ble} percent={normalizeScore(scores.ble)} color="bg-purple-400" isActive={activeProtocol === 1} />
            <ScoreBar label="LoRa" value={scores.lora} percent={normalizeScore(scores.lora)} color="bg-amber-400" isActive={activeProtocol === 2} />
          </div>
        )}
      </div>

      {/* Metrics Grid (Adapted per role) */}
      <div className="grid grid-cols-2 sm:grid-cols-4 gap-3 mb-5">
        <MetricBox
          label="WiFi Signal"
          value={telemetry ? `${telemetry.wifi_rssi} dBm` : '-'}
          subValue={getRssiStatus(telemetry?.wifi_rssi)}
          icon={Wifi}
          color="text-cyan-400"
        />
        <MetricBox
          label="BLE Signal"
          value={telemetry ? `${telemetry.ble_rssi} dBm` : '-'}
          subValue={getRssiStatus(telemetry?.ble_rssi)}
          icon={Bluetooth}
          color="text-purple-400"
        />
        <MetricBox
          label="LoRa Signal"
          value={telemetry ? `${telemetry.lora_rssi} dBm` : '-'}
          subValue={telemetry ? `SNR: ${telemetry.lora_snr} dB` : ''}
          icon={Radio}
          color="text-amber-400"
        />
        <MetricBox
          label="Response Delay"
          value={telemetry ? `${telemetry.rtt_ms} ms` : '-'}
          subValue={getRttStatus(telemetry?.rtt_ms)}
          icon={Zap}
          color={telemetry?.rtt_ms < 100 ? 'text-emerald-400' : 'text-rose-400'}
        />
      </div>

      {/* Quick Force Handover Bar (Admin Only) */}
      {permissions.canOverrideProtocols && (
        <div className="pt-3 border-t border-slate-800/80 flex items-center justify-between gap-2 text-xs">
          <span className="text-slate-400 font-medium">Override Protocol:</span>
          <div className="flex items-center gap-1.5">
            {[
              { id: 0, label: 'WiFi', color: 'hover:border-cyan-500 hover:text-cyan-400' },
              { id: 1, label: 'BLE', color: 'hover:border-purple-500 hover:text-purple-400' },
              { id: 2, label: 'LoRa', color: 'hover:border-amber-500 hover:text-amber-400' },
            ].map((proto) => (
              <button
                key={proto.id}
                onClick={() => onQuickForce(nodeName, proto.id)}
                disabled={activeProtocol === proto.id}
                className={`px-2.5 py-1 rounded-md border font-mono transition-all text-xs ${
                  activeProtocol === proto.id
                    ? 'bg-slate-800 border-slate-700 text-slate-500 cursor-not-allowed'
                    : `bg-slate-900 border-slate-800 text-slate-300 ${proto.color}`
                }`}
              >
                {proto.label}
              </button>
            ))}
          </div>
        </div>
      )}
    </div>
  );
}

function ScoreBar({ label, value, percent, color, isActive }) {
  return (
    <div className="flex items-center gap-2 text-[11px] font-mono">
      <span className={`w-10 text-slate-400 ${isActive ? 'font-bold text-slate-200' : ''}`}>{label}</span>
      <div className="flex-1 h-1.5 rounded-full bg-slate-800 overflow-hidden">
        <div className={`h-full rounded-full transition-all duration-300 ${color}`} style={{ width: `${percent}%` }} />
      </div>
      <span className="w-12 text-right text-slate-400 text-[10px]">{value}</span>
    </div>
  );
}

function MetricBox({ label, value, subValue, icon: Icon, color }) {
  return (
    <div className="bg-slate-900/60 rounded-xl p-3 border border-slate-800/90 flex flex-col justify-between">
      <div className="flex items-center justify-between text-slate-400 text-[11px] font-medium mb-1">
        <span>{label}</span>
        <Icon className={`w-3.5 h-3.5 ${color}`} />
      </div>
      <div className="font-mono font-bold text-slate-100 text-sm tracking-tight">{value}</div>
      {subValue && <div className="text-[10px] font-mono text-slate-500 mt-1">{subValue}</div>}
    </div>
  );
}
