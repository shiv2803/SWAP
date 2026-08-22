import React from 'react';
import { CheckCircle2, AlertCircle, Info, X, Zap } from 'lucide-react';

export function ToastContainer({ toasts, onDismiss }) {
  if (!toasts || toasts.length === 0) return null;

  return (
    <div className="fixed top-20 right-4 z-50 flex flex-col gap-2 max-w-sm w-full pointer-events-none">
      {toasts.map((toast) => (
        <div
          key={toast.id}
          className={`pointer-events-auto glass-panel p-3.5 rounded-2xl border shadow-2xl transition-all duration-300 animate-slideInRight flex items-start gap-3 ${
            toast.type === 'error'
              ? 'border-rose-500/40 bg-rose-950/80 text-rose-200'
              : toast.type === 'success'
              ? 'border-emerald-500/40 bg-emerald-950/80 text-emerald-200'
              : 'border-cyan-500/40 bg-slate-900/90 text-cyan-200'
          }`}
        >
          <div className="shrink-0 pt-0.5">
            {toast.type === 'error' ? (
              <AlertCircle className="w-5 h-5 text-rose-400" />
            ) : toast.type === 'success' ? (
              <Zap className="w-5 h-5 text-emerald-400 animate-pulse" />
            ) : (
              <Info className="w-5 h-5 text-cyan-400" />
            )}
          </div>

          <div className="flex-1 min-w-0">
            <div className="font-bold text-xs text-slate-100 flex items-center justify-between">
              <span>{toast.title || 'System Notification'}</span>
              <button
                onClick={() => onDismiss(toast.id)}
                className="text-slate-400 hover:text-slate-200 p-0.5"
              >
                <X className="w-3.5 h-3.5" />
              </button>
            </div>
            <p className="text-xs text-slate-300 mt-0.5 break-words font-mono">{toast.message}</p>
          </div>
        </div>
      ))}
    </div>
  );
}
