Name:           vmca
Version:        3.0.0
Release:        1%{?dist}
Summary:        VMCoreAffinity — Transparent vCPU Affinity Orchestration Framework
License:        MulanPSL-2.0
URL:            https://gitee.com/openeuler/vmca
Source0:        %{name}-%{version}.tar.gz

BuildArch:      noarch
BuildRequires:  python3-devel
BuildRequires:  systemd

Requires:       python3 >= 3.8
Requires:       qemu-kvm
Requires:       libvirt
Requires:       systemd

%description
vmca provides transparent vCPU pinning optimization for KVM/QEMU
virtual machines. Based on the patent "一种优化虚拟机内业务绑核性能的方法"
(A Method for Optimizing VM In-Guest Business CPU Pinning Performance,
Inventor: 张海亮).

When a business application inside a VM uses sched_setaffinity to pin to
specific vCPUs, the guest-side eBPF interceptor detects this and notifies
the host via VSOCK / virtio-serial.  The host-side listener daemon then
dynamically switches the corresponding vCPU from range-pinning to 1:1
exclusive pinning, achieving stable performance without sacrificing
resource utilization.

This package (vmca) installs on the **host** and provides:
  - Notification listener daemon (VSOCK)
  - Global CPU map management with cross-VM conflict avoidance
  - Dynamic 1:1 pinning via virsh vcpupin
  - Automatic range-pinning restore on guest app unbind
  - Persistent state with auto-restore on reboot
  - Event log for auditing

%package guest
Summary:        Guest-side eBPF sched_setaffinity interceptor for vmca
Requires:       python3 >= 3.8

%description guest
Guest-side component of vmca.  Installs inside the VM to intercept
sched_setaffinity calls via an eBPF CO-RE program and notify the host-side
vmca listener through VSOCK or virtio-serial.

This package (vmca-guest) installs **inside the virtual machine**.
  - eBPF CO-RE program for sched_setaffinity tracepoint interception
  - User-space agent for async ring buffer processing
  - Independent notification module (VSOCK / virtio-serial transport)
  - systemd service for automatic startup

%prep
%setup -q

%build
# Nothing to build for pure Python tools

%install
# --- Host-side (vmca) ---

# Main CLI + listener
install -D -m 0755 src/vmca.py \
    %{buildroot}%{_bindir}/vmca

# Restore helper
install -D -m 0755 src/vmca_restore.py \
    %{buildroot}%{_libexecdir}/vmca/vmca_restore.py

# Systemd services (host)
install -D -m 0644 src/vmca-listener.service \
    %{buildroot}%{_unitdir}/vmca-listener.service
install -D -m 0644 src/vmca-restore.service \
    %{buildroot}%{_unitdir}/vmca-restore.service

# Config directory
install -d -m 0755 %{buildroot}%{_sysconfdir}/vmca

# Man page
install -d -m 0755 %{buildroot}%{_mandir}/man1
install -D -m 0644 docs/vmca.1 \
    %{buildroot}%{_mandir}/man1/vmca.1

# --- Guest-side (vmca-guest) ---

# Guest agent
install -D -m 0755 src/vmca_guest.py \
    %{buildroot}%{_bindir}/vmca-guest

# eBPF C source (reference; production builds .bpf.o at package build time)
install -D -m 0644 src/bpf/vmca_intercept.bpf.c \
    %{buildroot}%{_datadir}/vmca-guest/bpf/vmca_intercept.bpf.c

# Systemd service (guest)
install -D -m 0644 src/vmca-guest.service \
    %{buildroot}%{_unitdir}/vmca-guest.service

# --- Host-side scriptlets ---

%post
%systemd_post vmca-listener.service
%systemd_post vmca-restore.service

%preun
%systemd_preun vmca-listener.service
%systemd_preun vmca-restore.service

%postun
%systemd_postun_with_restart vmca-listener.service
%systemd_postun_with_restart vmca-restore.service

# --- Guest-side scriptlets ---

%post guest
%systemd_post vmca-guest.service

%preun guest
%systemd_preun vmca-guest.service

%postun guest
%systemd_postun_with_restart vmca-guest.service

# --- File lists ---

%files
%license LICENSE
%doc README.md docs/SR.md docs/US.md
%{_bindir}/vmca
%{_libexecdir}/vmca/vmca_restore.py
%{_unitdir}/vmca-listener.service
%{_unitdir}/vmca-restore.service
%dir %{_sysconfdir}/vmca
%{_mandir}/man1/vmca.1*

%files guest
%license LICENSE
%{_bindir}/vmca-guest
%{_datadir}/vmca-guest/bpf/vmca_intercept.bpf.c
%{_unitdir}/vmca-guest.service

%changelog
* Tue Apr 15 2026 openEuler Contributors <dev@openeuler.org> - 3.0.0-1
- Rewrite: two clear RPM packages (host + guest)
- Guest: eBPF CO-RE tracepoint interception + async ring buffer + notification agent
- Guest: pluggable transport (VSOCK / virtio-serial / simulated)
- Guest: systemd service (vmca-guest.service)
- Host: VSOCK listener daemon for receiving guest notifications
- Host: PinExecutor with virsh vcpupin integration
- Host: event log for audit trail
- Host: systemd services (vmca-listener + vmca-restore)
- eBPF C source: src/bpf/vmca_intercept.bpf.c

* Mon Apr 14 2026 openEuler Contributors <dev@openeuler.org> - 2.0.0-1
- Rewrite to match patent: 一种优化虚拟机内业务绑核性能的方法
- Guest-side interception via kprobe/eBPF with hypercall notification
- VMM-side dynamic 1:1 pinning on guest app bind/unbind
- Global CPU map with cross-VM conflict avoidance
- Separate guest RPM subpackage (vmca-guest)

* Mon Apr 14 2026 openEuler Contributors <dev@openeuler.org> - 1.0.0-1
- Initial release
