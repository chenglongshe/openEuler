Name:           vm-bindcore
Version:        3.0.0
Release:        1%{?dist}
Summary:        VMM-side Transparent vCPU Pinning Optimization Tool
License:        MulanPSL-2.0
URL:            https://gitee.com/openeuler/vm-bindcore
Source0:        %{name}-%{version}.tar.gz

BuildArch:      noarch
BuildRequires:  python3-devel
BuildRequires:  systemd

Requires:       python3 >= 3.8
Requires:       qemu-kvm
Requires:       libvirt
Requires:       systemd

%description
vm-bindcore provides transparent vCPU pinning optimization for KVM/QEMU
virtual machines. Based on the patent "一种优化虚拟机内业务绑核性能的方法"
(A Method for Optimizing VM In-Guest Business CPU Pinning Performance,
Inventor: 张海亮).

When a business application inside a VM uses sched_setaffinity to pin to
specific vCPUs, the guest-side eBPF interceptor detects this and notifies
the host via VSOCK / virtio-serial.  The host-side listener daemon then
dynamically switches the corresponding vCPU from range-pinning to 1:1
exclusive pinning, achieving stable performance without sacrificing
resource utilization.

This package (vm-bindcore) installs on the **host** and provides:
  - Notification listener daemon (VSOCK)
  - Global CPU map management with cross-VM conflict avoidance
  - Dynamic 1:1 pinning via virsh vcpupin
  - Automatic range-pinning restore on guest app unbind
  - Persistent state with auto-restore on reboot
  - Event log for auditing

%package guest
Summary:        Guest-side eBPF sched_setaffinity interceptor for vm-bindcore
Requires:       python3 >= 3.8

%description guest
Guest-side component of vm-bindcore.  Installs inside the VM to intercept
sched_setaffinity calls via an eBPF CO-RE program and notify the host-side
vm-bindcore listener through VSOCK or virtio-serial.

This package (vm-bindcore-guest) installs **inside the virtual machine**.
  - eBPF CO-RE program for sched_setaffinity tracepoint interception
  - User-space agent for async ring buffer processing
  - Independent notification module (VSOCK / virtio-serial transport)
  - systemd service for automatic startup

%prep
%setup -q

%build
# Nothing to build for pure Python tools

%install
# --- Host-side (vm-bindcore) ---

# Main CLI + listener
install -D -m 0755 src/vm_bindcore.py \
    %{buildroot}%{_bindir}/vm-bindcore

# Restore helper
install -D -m 0755 src/vm_bindcore_restore.py \
    %{buildroot}%{_libexecdir}/vm-bindcore/vm_bindcore_restore.py

# Systemd services (host)
install -D -m 0644 src/vm-bindcore-listener.service \
    %{buildroot}%{_unitdir}/vm-bindcore-listener.service
install -D -m 0644 src/vm-bindcore-restore.service \
    %{buildroot}%{_unitdir}/vm-bindcore-restore.service

# Config directory
install -d -m 0755 %{buildroot}%{_sysconfdir}/vm-bindcore

# Man page
install -d -m 0755 %{buildroot}%{_mandir}/man1
install -D -m 0644 docs/vm-bindcore.1 \
    %{buildroot}%{_mandir}/man1/vm-bindcore.1

# --- Guest-side (vm-bindcore-guest) ---

# Guest agent
install -D -m 0755 src/vm_bindcore_guest.py \
    %{buildroot}%{_bindir}/vm-bindcore-guest

# eBPF C source (reference; production builds .bpf.o at package build time)
install -D -m 0644 src/bpf/bindcore_intercept.bpf.c \
    %{buildroot}%{_datadir}/vm-bindcore-guest/bpf/bindcore_intercept.bpf.c

# Systemd service (guest)
install -D -m 0644 src/vm-bindcore-guest.service \
    %{buildroot}%{_unitdir}/vm-bindcore-guest.service

# --- Host-side scriptlets ---

%post
%systemd_post vm-bindcore-listener.service
%systemd_post vm-bindcore-restore.service

%preun
%systemd_preun vm-bindcore-listener.service
%systemd_preun vm-bindcore-restore.service

%postun
%systemd_postun_with_restart vm-bindcore-listener.service
%systemd_postun_with_restart vm-bindcore-restore.service

# --- Guest-side scriptlets ---

%post guest
%systemd_post vm-bindcore-guest.service

%preun guest
%systemd_preun vm-bindcore-guest.service

%postun guest
%systemd_postun_with_restart vm-bindcore-guest.service

# --- File lists ---

%files
%license LICENSE
%doc README.md docs/SR.md docs/US.md
%{_bindir}/vm-bindcore
%{_libexecdir}/vm-bindcore/vm_bindcore_restore.py
%{_unitdir}/vm-bindcore-listener.service
%{_unitdir}/vm-bindcore-restore.service
%dir %{_sysconfdir}/vm-bindcore
%{_mandir}/man1/vm-bindcore.1*

%files guest
%license LICENSE
%{_bindir}/vm-bindcore-guest
%{_datadir}/vm-bindcore-guest/bpf/bindcore_intercept.bpf.c
%{_unitdir}/vm-bindcore-guest.service

%changelog
* Tue Apr 15 2026 openEuler Contributors <dev@openeuler.org> - 3.0.0-1
- Rewrite: two clear RPM packages (host + guest)
- Guest: eBPF CO-RE tracepoint interception + async ring buffer + notification agent
- Guest: pluggable transport (VSOCK / virtio-serial / simulated)
- Guest: systemd service (vm-bindcore-guest.service)
- Host: VSOCK listener daemon for receiving guest notifications
- Host: PinExecutor with virsh vcpupin integration
- Host: event log for audit trail
- Host: systemd services (vm-bindcore-listener + vm-bindcore-restore)
- eBPF C source: src/bpf/bindcore_intercept.bpf.c

* Mon Apr 14 2026 openEuler Contributors <dev@openeuler.org> - 2.0.0-1
- Rewrite to match patent: 一种优化虚拟机内业务绑核性能的方法
- Guest-side interception via kprobe/eBPF with hypercall notification
- VMM-side dynamic 1:1 pinning on guest app bind/unbind
- Global CPU map with cross-VM conflict avoidance
- Separate guest RPM subpackage (vm-bindcore-guest)

* Mon Apr 14 2026 openEuler Contributors <dev@openeuler.org> - 1.0.0-1
- Initial release
