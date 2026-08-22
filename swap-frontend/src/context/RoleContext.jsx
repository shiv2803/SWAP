import React, { createContext, useContext, useState } from 'react';

const RoleContext = createContext();

export const ROLES = {
  USER: 'user',       // Standard User / Field Operator
  ADMIN: 'admin',     // Network Administrator
  ANALYST: 'analyst', // AI & Data Science Analyst
};

export const ROLE_PERMISSIONS = {
  [ROLES.USER]: {
    name: 'Standard User',
    icon: '👤',
    description: 'Simplified monitoring view with intuitive link health & alerts.',
    canOverrideProtocols: false,
    canViewTopology: false,
    canViewSla: true,
    canViewRawLogs: false,
    canViewAiDetails: false,
    canExportCsv: false,
    canConfigureSettings: false,
  },
  [ROLES.ADMIN]: {
    name: 'Network Admin',
    icon: '⚡',
    description: 'Full network command, protocol overrides, topology & server settings.',
    canOverrideProtocols: true,
    canViewTopology: true,
    canViewSla: true,
    canViewRawLogs: true,
    canViewAiDetails: true,
    canExportCsv: true,
    canConfigureSettings: true,
  },
  [ROLES.ANALYST]: {
    name: 'AI & ML Analyst',
    icon: '📊',
    description: 'Model feature windows, probability breakdown, and CSV data export.',
    canOverrideProtocols: false,
    canViewTopology: true,
    canViewSla: true,
    canViewRawLogs: true,
    canViewAiDetails: true,
    canExportCsv: true,
    canConfigureSettings: false,
  },
};

export function RoleProvider({ children }) {
  const [role, setRole] = useState(ROLES.ADMIN); // Default to Admin for easy initial testing

  const permissions = ROLE_PERMISSIONS[role] || ROLE_PERMISSIONS[ROLES.USER];

  return (
    <RoleContext.Provider value={{ role, setRole, permissions, ROLES, ROLE_PERMISSIONS }}>
      {children}
    </RoleContext.Provider>
  );
}

export function useRole() {
  const context = useContext(RoleContext);
  if (!context) {
    throw new Error('useRole must be used within a RoleProvider');
  }
  return context;
}
