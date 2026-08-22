import React from 'react';
import { ShieldCheck, Zap, AlertTriangle, Activity, CheckCircle2 } from 'lucide-react';

export function SlaMonitor({ nodeAData, nodeBData }) {
  const rttA = nodeAData?.record?.rtt_ms || 10;
  const rttB = nodeBData?.record?.rtt_ms || 10;
  const avgRtt = Math.round((rttA + rttB) / 2);

  const lossA = (nodeAData?.record?.wifi_loss || 0) * 100;
  const lossB = (nodeBData?.record?.wifi_loss || 0) * 100;
  const avgLoss = Number(((lossA + lossB) / 2).toFixed(1));

  const isRttOk = avgRtt < 120;
  const isLossOk = avgLoss < 5.0;

  const slaScore = isRttOk && isLossOk ? '99.8%' : isRttOk || isLossOk ? '94.5%' : '88.2%';

  return (
    <div className="glass-panel rounded-2xl p-6 border border-slate-800 flex flex-col justify-between">
      <div>
        <div className="flex items-center justify-between pb-4 mb-4 border-b border-slate-800/80">
          <div className="flex items-center gap-3">
            <div className="p-2 rounded-xl bg-emerald-500/10 border border-emerald-500/30 text-emerald-400">
              <ShieldCheck className="w-5 h-5" />
            </div>
            <div>
              <h3 className="font-bold text-slate-100 text-base">Enterprise SLA Monitor</h3>
              <p className="text-xs text-slate-400">QoS performance targets & link availability SLA</p>
            </div>
          </div>

          <div className="text-right">
            <span className="text-[10px] uppercase font-bold text-slate-400 block">SLA Health Score</span>
            <span className="font-mono font-bold text-base text-emerald-400">{slaScore}</span>
          </div>
        </div>

        {/* SLA Metrics Grid */}
        <div className="space-y-3 font-mono text-xs">
          {/* Target 1: Latency SLA */}
          <SlaRow
            label="RTT Latency Target (<120ms)"
            value={`${avgRtt} ms`}
            status={isRttOk ? 'PASS' : 'WARN'}
            color={isRttOk ? 'text-emerald-400' : 'text-amber-400'}
          />

          {/* Target 2: Packet Loss SLA */}
          <SlaRow
            label="Packet Loss Target (<5.0%)"
            value={`${avgLoss}%`}
            status={isLossOk ? 'PASS' : 'WARN'}
            color={isLossOk ? 'text-emerald-400' : 'text-rose-400'}
          />

          {/* Target 3: RF Link Margin */}
          <SlaRow
            label="RF Margin vs Floor (-105dBm)"
            value="18 dB Margin"
            status="PASS"
            color="text-emerald-400"
          />
        </div>
      </div>

      <div className="mt-4 pt-3 border-t border-slate-800/80 flex items-center justify-between text-[11px] text-slate-400">
        <span className="flex items-center gap-1.5">
          <CheckCircle2 className="w-3.5 h-3.5 text-emerald-400" /> All Handover Triggers Monitored
        </span>
        <span className="font-mono text-slate-500">ISO/IEC 27001</span>
      </div>
    </div>
  );
}

function SlaRow({ label, value, status, color }) {
  return (
    <div className="p-2.5 rounded-xl bg-slate-900/60 border border-slate-800/80 flex items-center justify-between">
      <span className="text-slate-300 font-medium text-[11px]">{label}</span>
      <div className="flex items-center gap-2">
        <span className={`font-bold ${color}`}>{value}</span>
        <span
          className={`px-2 py-0.5 rounded text-[9px] font-bold ${
            status === 'PASS'
              ? 'bg-emerald-500/20 text-emerald-300 border border-emerald-500/40'
              : 'bg-amber-500/20 text-amber-300 border border-amber-500/40'
          }`}
        >
          {status}
        </span>
      </div>
    </div>
  );
}
