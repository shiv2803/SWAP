import React from 'react';
import { Radio, Wifi, Bluetooth, Cpu, Server, Activity, ShieldCheck } from 'lucide-react';

const PROTOCOL_COLORS = {
  0: { stroke: '#38bdf8', glow: 'rgba(56, 189, 248, 0.4)', text: 'text-cyan-400', label: 'WiFi (2.4/5GHz)' },
  1: { stroke: '#c084fc', glow: 'rgba(192, 132, 252, 0.4)', text: 'text-purple-400', label: 'BLE (2.4GHz)' },
  2: { stroke: '#fbbf24', glow: 'rgba(251, 191, 36, 0.4)', text: 'text-amber-400', label: 'LoRa (Sub-GHz)' },
};

export function NetworkTopology({ nodeAData, nodeBData }) {
  const activeProtoA = nodeAData?.record?.active_protocol ?? 0;
  const activeProtoB = nodeBData?.record?.active_protocol ?? 0;

  const colorA = PROTOCOL_COLORS[activeProtoA] || PROTOCOL_COLORS[0];
  const colorB = PROTOCOL_COLORS[activeProtoB] || PROTOCOL_COLORS[0];

  return (
    <div className="radar-scope rounded-3xl p-6 relative overflow-hidden">
      <div className="reticle-corner tl" />
      <div className="reticle-corner tr" />
      <div className="reticle-corner bl" />
      <div className="reticle-corner br" />

      <div className="flex items-center justify-between pb-4 mb-2 relative z-10">
        <div className="flex items-center gap-3">
          <Activity className="w-4 h-4 text-cyan-400" />
          <div>
            <h3 className="font-mono uppercase tracking-[0.15em] text-slate-200 text-sm">RF Topology Scope</h3>
            <p className="text-[11px] text-slate-500">Live wireless link beam & packet transmission map</p>
          </div>
        </div>

        {/* Legend */}
        <div className="hidden sm:flex items-center gap-4 text-xs font-mono">
          <div className="flex items-center gap-1.5 text-cyan-400">
            <span className="w-2.5 h-2.5 rounded-full bg-cyan-400 animate-pulse" /> WiFi Link
          </div>
          <div className="flex items-center gap-1.5 text-purple-400">
            <span className="w-2.5 h-2.5 rounded-full bg-purple-400 animate-pulse" /> BLE Link
          </div>
          <div className="flex items-center gap-1.5 text-amber-400">
            <span className="w-2.5 h-2.5 rounded-full bg-amber-400 animate-pulse" /> LoRa Link
          </div>
        </div>
      </div>

      {/* Radar Canvas Topology Graph */}
      <div className="relative w-full h-[220px] overflow-hidden flex items-center justify-center">
        <div className="radar-rings" />
        <div className="radar-sweep" />
        <svg className="absolute inset-0 w-full h-full pointer-events-none">
          <defs>
            <filter id="glow-cyan" x="-20%" y="-20%" width="140%" height="140%">
              <feGaussianBlur stdDeviation="4" result="blur" />
              <feComposite in="SourceGraphic" in2="blur" operator="over" />
            </filter>
            <filter id="glow-purple" x="-20%" y="-20%" width="140%" height="140%">
              <feGaussianBlur stdDeviation="4" result="blur" />
              <feComposite in="SourceGraphic" in2="blur" operator="over" />
            </filter>
            <filter id="glow-amber" x="-20%" y="-20%" width="140%" height="140%">
              <feGaussianBlur stdDeviation="4" result="blur" />
              <feComposite in="SourceGraphic" in2="blur" operator="over" />
            </filter>
          </defs>

          {/* Node A to Gateway Beam */}
          <line
            x1="20%"
            y1="50%"
            x2="50%"
            y2="50%"
            stroke={colorA.stroke}
            strokeWidth="3"
            strokeDasharray="6 4"
            filter={`url(#glow-${activeProtoA === 0 ? 'cyan' : activeProtoA === 1 ? 'purple' : 'amber'})`}
            className="animate-pulse"
          />

          {/* Animated Packet Pulse A */}
          <circle r="4" fill={colorA.stroke}>
            <animateAttribute
              attributeName="cx"
              from="20%"
              to="50%"
              dur="1.8s"
              repeatCount="indefinite"
            />
            <animateAttribute attributeName="cy" value="50%" />
          </circle>

          {/* Node B to Gateway Beam */}
          <line
            x1="80%"
            y1="50%"
            x2="50%"
            y2="50%"
            stroke={colorB.stroke}
            strokeWidth="3"
            strokeDasharray="6 4"
            filter={`url(#glow-${activeProtoB === 0 ? 'cyan' : activeProtoB === 1 ? 'purple' : 'amber'})`}
            className="animate-pulse"
          />

          {/* Animated Packet Pulse B */}
          <circle r="4" fill={colorB.stroke}>
            <animateAttribute
              attributeName="cx"
              from="80%"
              to="50%"
              dur="2.1s"
              repeatCount="indefinite"
            />
            <animateAttribute attributeName="cy" value="50%" />
          </circle>
        </svg>

        {/* Node Elements */}
        <div className="relative z-10 w-full px-6 flex items-center justify-between max-w-4xl mx-auto">
          {/* Node A */}
          <TopologyNode
            name="Node A"
            activeProtocol={activeProtoA}
            rssi={nodeAData?.record?.wifi_rssi}
            rtt={nodeAData?.record?.rtt_ms}
            colorClass={colorA.text}
          />

          {/* SWAP Gateway Center */}
          <div className="flex flex-col items-center">
            <div className="w-16 h-16 rounded-2xl bg-gradient-to-tr from-cyan-500 via-indigo-600 to-purple-600 p-0.5 shadow-xl shadow-cyan-500/20">
              <div className="w-full h-full bg-slate-950 rounded-[14px] flex items-center justify-center">
                <Server className="w-8 h-8 text-cyan-400 animate-pulse" />
              </div>
            </div>
            <div className="mt-2 text-center">
              <span className="font-bold text-xs text-slate-100 block">SWAP Gateway</span>
              <span className="text-[10px] font-mono text-emerald-400 flex items-center justify-center gap-1">
                <span className="w-1.5 h-1.5 rounded-full bg-emerald-400 animate-pulse-live" /> Active Receiver
              </span>
            </div>
          </div>

          {/* Node B */}
          <TopologyNode
            name="Node B"
            activeProtocol={activeProtoB}
            rssi={nodeBData?.record?.wifi_rssi}
            rtt={nodeBData?.record?.rtt_ms}
            colorClass={colorB.text}
          />
        </div>
      </div>
    </div>
  );
}

function TopologyNode({ name, activeProtocol, rssi, rtt, colorClass }) {
  const names = ['WiFi', 'BLE', 'LoRa'];
  return (
    <div className="flex flex-col items-center">
      <div className="w-14 h-14 rounded-2xl bg-slate-900 border border-slate-800 flex items-center justify-center shadow-lg">
        <Radio className={`w-6 h-6 ${colorClass}`} />
      </div>
      <div className="mt-2 text-center">
        <span className="font-bold text-xs text-slate-100 block">{name}</span>
        <span className={`text-[10px] font-mono font-bold ${colorClass}`}>
          {names[activeProtocol] || 'WiFi'} • {rssi ? `${rssi}dBm` : '-'}
        </span>
        <span className="text-[9px] font-mono text-slate-500 block">
          {rtt ? `${rtt}ms RTT` : ''}
        </span>
      </div>
    </div>
  );
}
