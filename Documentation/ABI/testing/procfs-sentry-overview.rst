.. SPDX-License-Identifier: GPL-2.0

======================================
Sentry System Proc Interfaces Overview
======================================

This document provides an overview of all proc interfaces for the Sentry system.

System Event Reporter Module
----------------------------
See: procfs-sentry-reporter

- Directory: /proc/sentry_reporter/
- Reports kernel events to userspace
- Handles OOM, power off, and UB memory fault events
- Integrates with kernel notification chains

Remote Reporter Module
----------------------
See: procfs-sentry-remote-reporter

- Directory: /proc/sentry_remote_reporter/
- Reports system events (panic/reboot) to remote nodes
- Supports both URMA and UVB communication modes
- Configurable timeout values for different event types

URMA Communication Module
-------------------------
See: procfs-sentry-urma-comm

- Directory: /proc/sentry_urma_comm/
- Manages sentry URMA communication infrastructure
- Handles heartbeat monitoring and connection management

UVB Communication Module
------------------------
See: procfs-sentry-uvb-comm

- Directory: /proc/sentry_uvb_comm/
- Manages sentry UVB communication
- Configures server CNA addresses for broadcast and unicast

System Architecture
-------------------

The Sentry system consists of four main components that work together:

1. **System Event Reporter** (sentry_reporter)
   - Monitors and reports kernel-level events
   - Integrates with kernel notification chains (OOM, power management)
   - Handles memory errors(oom) and system shutdown events(power off)

2. **System Remote Event Reporter** (sentry_remote_reporter)
   - Coordinates system event reporting
   - Supports both UVB and URMA transport layers
   - Provides configurable timeout and enable/disable controls


3. **URMA Communication** (sentry_urma_comm)
   - Provides URMA-based high-performance communication
   - Manages jetty endpoints and heartbeat monitoring
   - Handles connection recovery and multi-die support

4. **UVB Communication** (sentry_uvb_comm)
   - Provides UVB-based message transport
   - Manages server CNA configuration
   - Supports both unicast and broadcast modes

Configuration Workflow
----------------------

Typical configuration sequence:

1. Configure UVB server CNAs:
echo "100;200;300" > /proc/sentry_uvb_comm/server_cna

2. Configure URMA endpoints:
echo "2001:0db8:85a3:0000:0000:8a2e:0370:7334,2001:0db8:85a3:0000: \
0000:8a2e:0370:7335;2001:0db8:85a3:0000:0000:8a2e:0370:7336,2001:0db8: \
85a3:0000:0000:8a2e:0370:7337 100" > /proc/sentry_urma_comm/client_info

3. Configure local identification:
echo "100" > /proc/sentry_remote_reporter/cna
echo "2001:0db8:85a3:0000:0000:8a2e:0370:7334;2001:0db8:85a3:0000: \
0000:8a2e:0370:7335" > /proc/sentry_remote_reporter/eid

4. Enable event reporting:
echo "on" > /proc/sentry_remote_reporter/panic
echo "on" > /proc/sentry_remote_reporter/kernel_reboot
echo "on" > /proc/sentry_reporter/oom
echo "on" > /proc/sentry_reporter/power_off

5. Configure communication preferences:
echo "on" > /proc/sentry_remote_reporter/uvb_comm
echo "on" > /proc/sentry_remote_reporter/urma_comm

Inter-module Dependencies
-------------------------

- Remote Reporter depends on both UVB and URMA communication modules
- URMA module requires proper EID configuration from Remote Reporter
- UVB module requires CNA configuration from Remote Reporter
- System Event Reporter module operates independently but uses the same messaging infrastructure