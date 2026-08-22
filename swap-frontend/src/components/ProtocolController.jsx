import React, { useState } from 'react';
import { Radio, Wifi, Bluetooth, Cpu, Send, CheckCircle2, AlertCircle, RefreshCw } from 'lucide-react';

export function ProtocolController({ onForceProtocol, onDecideProtocol }) {
  const [selectedNode, setSelectedNode] = useState('a');
  const [selectedProtocol, setSelectedProtocol] = useState(0);
  const [isSubmittingForce, setIsSubmittingForce] = useState(false);
  const [isSubmittingDecide, setIsSubmittingDecide] = useState(false);
  const [actionFeedback, setActionFeedback] = useState(null);

  const handleForce = async () => {
    setIsSubmittingForce(true);
    setActionFeedback(null);
    try {
      const res = await onForceProtocol(selectedNode, selectedProtocol);
      setActionFeedback({
        type: 'success',
        message: `Node ${selectedNode.toUpperCase()} protocol forced to ${res.protocol_name || ['WiFi', 'BLE', 'LoRa'][selectedProtocol]}`,
      });
    } catch (err) {
      setActionFeedback({
        type: 'error',
        message: err.message || 'Failed to force protocol',
      });
    } finally {
      setIsSubmittingForce(false);
    }
  };

  const handleDecide = async () => {
    setIsSubmittingDecide(true);
    setActionFeedback(null);
    try {
      const res = await onDecideProtocol(selectedNode);
      setActionFeedback({
        type: 'info',
        message: `AI Evaluation complete for Node ${selectedNode.toUpperCase()}. Recommendation: ${res.recommended_protocol_name || ['WiFi', 'BLE', 'LoRa'][res.recommended_protocol]} (${Math.round((res.confidence || 0.9) * 100)}% conf)`,
      });
    } catch (err) {
      setActionFeedback({
        type: 'error',
        message: err.message || 'AI evaluation failed',
      });
    } finally {
      setIsSubmittingDecide(false);
    }
  };

  return (
    <div className="glass-panel rounded-2xl p-6 border border-slate-800">
      <div className="flex items-center gap-3 mb-5 pb-3 border-b border-slate-800/80">
        <div className="p-2 rounded-xl bg-cyan-500/10 border border-cyan-500/30 text-cyan-400">
          <Send className="w-5 h-5" />
        </div>
        <div>
          <h3 className="font-bold text-slate-100 text-base">Protocol Control Center</h3>
          <p className="text-xs text-slate-400">Manual Handover Override & AI Trigger (`/force` & `/decide`)</p>
        </div>
      </div>

      <div className="space-y-5">
        {/* Step 1: Select Node */}
        <div>
          <label className="text-xs font-semibold text-slate-300 block mb-2">Target Node Endpoint</label>
          <div className="grid grid-cols-2 gap-3">
            {['a', 'b'].map((nodeId) => (
              <button
                key={nodeId}
                type="button"
                onClick={() => setSelectedNode(nodeId)}
                className={`py-2.5 px-4 rounded-xl text-xs font-bold font-mono border transition-all flex items-center justify-center gap-2 ${
                  selectedNode === nodeId
                    ? 'bg-slate-800 border-cyan-500 text-cyan-400 shadow-md shadow-cyan-500/10'
                    : 'bg-slate-900/60 border-slate-800 text-slate-400 hover:text-slate-200'
                }`}
              >
                <div className={`w-2 h-2 rounded-full ${selectedNode === nodeId ? 'bg-cyan-400' : 'bg-slate-600'}`} />
                NODE {nodeId.toUpperCase()}
              </button>
            ))}
          </div>
        </div>

        {/* Step 2: Select Protocol */}
        <div>
          <label className="text-xs font-semibold text-slate-300 block mb-2">Override Wireless Protocol</label>
          <div className="grid grid-cols-3 gap-2.5">
            {[
              { id: 0, label: 'WiFi', sub: '2.4 / 5 GHz', icon: Wifi, activeColor: 'border-cyan-500 text-cyan-400 bg-cyan-500/10' },
              { id: 1, label: 'BLE', sub: 'Bluetooth LE', icon: Bluetooth, activeColor: 'border-purple-500 text-purple-400 bg-purple-500/10' },
              { id: 2, label: 'LoRa', sub: 'Sub-GHz RF', icon: Radio, activeColor: 'border-amber-500 text-amber-400 bg-amber-500/10' },
            ].map((p) => {
              const Icon = p.icon;
              const isSelected = selectedProtocol === p.id;
              return (
                <button
                  key={p.id}
                  type="button"
                  onClick={() => setSelectedProtocol(p.id)}
                  className={`p-3 rounded-xl border transition-all flex flex-col items-center justify-center gap-1.5 text-center ${
                    isSelected
                      ? p.activeColor
                      : 'bg-slate-900/60 border-slate-800 text-slate-400 hover:text-slate-200'
                  }`}
                >
                  <Icon className="w-5 h-5" />
                  <span className="font-bold text-xs font-mono">{p.label}</span>
                  <span className="text-[10px] text-slate-500">{p.sub}</span>
                </button>
              );
            })}
          </div>
        </div>

        {/* Step 3: Action Buttons */}
        <div className="grid grid-cols-1 sm:grid-cols-2 gap-3 pt-2">
          {/* Force Protocol Button */}
          <button
            onClick={handleForce}
            disabled={isSubmittingForce}
            className="w-full py-3 px-4 rounded-xl bg-gradient-to-r from-cyan-500 to-blue-600 hover:from-cyan-400 hover:to-blue-500 text-white font-bold text-xs shadow-lg shadow-cyan-500/20 active:scale-98 transition-all flex items-center justify-center gap-2 disabled:opacity-50"
          >
            {isSubmittingForce ? (
              <RefreshCw className="w-4 h-4 animate-spin" />
            ) : (
              <Send className="w-4 h-4" />
            )}
            Force Protocol Override
          </button>

          {/* AI Decision Request Button */}
          <button
            onClick={handleDecide}
            disabled={isSubmittingDecide}
            className="w-full py-3 px-4 rounded-xl bg-slate-900 border border-slate-700 hover:border-cyan-500/50 text-slate-200 hover:text-cyan-400 font-bold text-xs active:scale-98 transition-all flex items-center justify-center gap-2 disabled:opacity-50"
          >
            {isSubmittingDecide ? (
              <RefreshCw className="w-4 h-4 animate-spin" />
            ) : (
              <Cpu className="w-4 h-4 text-cyan-400" />
            )}
            Request AI Recommendation
          </button>
        </div>

        {/* Action Feedback Banner */}
        {actionFeedback && (
          <div
            className={`p-3 rounded-xl border text-xs font-mono flex items-center gap-2 animate-fadeIn ${
              actionFeedback.type === 'error'
                ? 'bg-rose-500/10 border-rose-500/30 text-rose-400'
                : actionFeedback.type === 'success'
                ? 'bg-emerald-500/10 border-emerald-500/30 text-emerald-400'
                : 'bg-cyan-500/10 border-cyan-500/30 text-cyan-300'
            }`}
          >
            {actionFeedback.type === 'error' ? (
              <AlertCircle className="w-4 h-4 shrink-0 text-rose-400" />
            ) : (
              <CheckCircle2 className="w-4 h-4 shrink-0 text-emerald-400" />
            )}
            <span>{actionFeedback.message}</span>
          </div>
        )}
      </div>
    </div>
  );
}
