import React, { useState } from 'react';
import { Terminal, Shield, ArrowRightLeft, Radio, CheckCircle, AlertTriangle } from 'lucide-react';

export function EventLog({ events }) {
  const [logFilter, setLogFilter] = useState('all');

  const filteredEvents = events.filter((e) => {
    if (logFilter === 'handover') return e.type === 'handover' || e.type === 'force';
    if (logFilter === 'ai') return e.type === 'ai_decision';
    return true;
  });

  return (
    <div className="glass-panel rounded-2xl p-6 border border-slate-800 flex flex-col h-full">
      <div className="flex items-center justify-between gap-3 mb-4 pb-3 border-b border-slate-800/80">
        <div className="flex items-center gap-3">
          <div className="p-2 rounded-xl bg-cyan-500/10 border border-cyan-500/30 text-cyan-400">
            <Terminal className="w-5 h-5" />
          </div>
          <div>
            <h3 className="font-bold text-slate-100 text-base">Real-Time Event Stream</h3>
            <p className="text-xs text-slate-400">Chronological telemetry audit trail</p>
          </div>
        </div>

        {/* Filter Pills */}
        <div className="flex items-center bg-slate-900 p-1 rounded-xl border border-slate-800 text-xs font-mono">
          {[
            { id: 'all', label: 'All' },
            { id: 'handover', label: 'Handovers' },
            { id: 'ai', label: 'AI Decisions' },
          ].map((tab) => (
            <button
              key={tab.id}
              onClick={() => setLogFilter(tab.id)}
              className={`px-2.5 py-1 rounded-lg transition-all ${
                logFilter === tab.id
                  ? 'bg-slate-800 text-cyan-400 font-bold border border-slate-700'
                  : 'text-slate-400 hover:text-slate-200'
              }`}
            >
              {tab.label}
            </button>
          ))}
        </div>
      </div>

      {/* Log Terminal List */}
      <div className="flex-1 min-h-[220px] max-h-[360px] overflow-y-auto space-y-2 font-mono text-xs pr-1">
        {filteredEvents.length === 0 ? (
          <div className="h-full flex items-center justify-center text-slate-500 text-xs py-8">
            No logged events yet...
          </div>
        ) : (
          filteredEvents.map((evt, idx) => (
            <div
              key={evt.id ? `${evt.id}-${idx}` : idx}
              className={`p-2.5 rounded-xl border transition-all flex items-start gap-3 ${
                evt.type === 'force' || evt.type === 'handover'
                  ? 'bg-purple-500/10 border-purple-500/30 text-purple-300'
                  : evt.type === 'ai_decision'
                  ? 'bg-cyan-500/10 border-cyan-500/30 text-cyan-300'
                  : 'bg-slate-900/60 border-slate-800/80 text-slate-300'
              }`}
            >
              <div className="shrink-0 pt-0.5">
                {evt.type === 'force' || evt.type === 'handover' ? (
                  <ArrowRightLeft className="w-4 h-4 text-purple-400" />
                ) : evt.type === 'ai_decision' ? (
                  <Shield className="w-4 h-4 text-cyan-400" />
                ) : (
                  <Radio className="w-4 h-4 text-slate-500" />
                )}
              </div>

              <div className="flex-1 min-w-0">
                <div className="flex items-center justify-between gap-2">
                  <span className="font-bold text-[11px] uppercase tracking-wider text-slate-200">
                    Node {evt.node?.toUpperCase()}
                  </span>
                  <span className="text-[10px] text-slate-500">{evt.timestamp}</span>
                </div>
                <p className="text-xs text-slate-300 mt-0.5 break-words">{evt.message}</p>
                {evt.details && (
                  <div className="text-[10px] text-slate-400 mt-1 flex flex-wrap gap-x-3 gap-y-0.5">
                    {Object.entries(evt.details).map(([k, v]) => (
                      <span key={k}>
                        <strong className="text-slate-500">{k}:</strong> {String(v)}
                      </span>
                    ))}
                  </div>
                )}
              </div>
            </div>
          ))
        )}
      </div>
    </div>
  );
}
