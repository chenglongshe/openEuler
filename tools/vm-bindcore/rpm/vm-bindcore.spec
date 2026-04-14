Name:           vm-bindcore
Version:        1.0.0
Release:        1%{?dist}
Summary:        VM vCPU CPU-Pinning Management Tool (NUMA-aware)
License:        MulanPSL-2.0
URL:            https://gitee.com/openeuler/vm-bindcore
Source0:        %{name}-%{version}.tar.gz

BuildArch:      noarch
BuildRequires:  python3-devel
BuildRequires:  systemd

Requires:       python3 >= 3.8
Requires:       libvirt-python3 >= 6.0.0
Requires:       libvirt-daemon >= 6.0.0
Requires:       qemu-kvm
Requires:       systemd

%description
vm-bindcore is a NUMA-aware vCPU pinning management tool for KVM/libvirt
virtual machines. Based on the patent "一种虚拟机绑核方法及计算设备"
(A Virtual Machine CPU Pinning Method and Computing Device, Inventor: 张海亮),
it provides automatic and manual vCPU-to-pCPU pinning with the following
strategies:

  - numa-aware: Pin all vCPUs to the same NUMA node to reduce cross-node
                memory access latency.
  - spread:     Distribute vCPUs evenly across NUMA nodes to maximize
                memory bandwidth utilization.
  - compact:    Pack vCPUs onto fewest physical cores using SMT siblings
                to minimize CPU resource footprint.

%prep
%setup -q

%build
# Nothing to build for a pure Python tool

%install
# Install main tool
install -D -m 0755 src/vm_bindcore.py \
    %{buildroot}%{_bindir}/vm-bindcore

# Install restore helper
install -D -m 0755 src/vm_bindcore_restore.py \
    %{buildroot}%{_libexecdir}/vm-bindcore/vm_bindcore_restore.py

# Install systemd service
install -D -m 0644 src/vm-bindcore-restore.service \
    %{buildroot}%{_unitdir}/vm-bindcore-restore.service

# Create config directory
install -d -m 0755 %{buildroot}%{_sysconfdir}/vm-bindcore/pinning.d

# Install man page
install -d -m 0755 %{buildroot}%{_mandir}/man1
install -D -m 0644 docs/vm-bindcore.1 \
    %{buildroot}%{_mandir}/man1/vm-bindcore.1

%post
%systemd_post vm-bindcore-restore.service

%preun
%systemd_preun vm-bindcore-restore.service

%postun
%systemd_postun_with_restart vm-bindcore-restore.service

%files
%license LICENSE
%doc README.md docs/SR.md docs/US.md
%{_bindir}/vm-bindcore
%{_libexecdir}/vm-bindcore/vm_bindcore_restore.py
%{_unitdir}/vm-bindcore-restore.service
%dir %{_sysconfdir}/vm-bindcore
%dir %{_sysconfdir}/vm-bindcore/pinning.d
%{_mandir}/man1/vm-bindcore.1*

%changelog
* Mon Apr 14 2026 openEuler Contributors <dev@openeuler.org> - 1.0.0-1
- Initial release
- NUMA-aware vCPU pinning (numa-aware, spread, compact strategies)
- Manual vCPU-to-pCPU pinning support
- Pinning status query and management
- Persistent pinning configuration with auto-restore on reboot
- Systemd service for automatic pinning restoration
