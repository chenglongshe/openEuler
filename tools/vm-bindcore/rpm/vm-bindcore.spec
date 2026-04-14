Name:           vm-bindcore
Version:        2.0.0
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
Requires:       systemd

%description
vm-bindcore provides transparent vCPU pinning optimization for KVM/QEMU
virtual machines. Based on the patent "一种优化虚拟机内业务绑核性能的方法"
(A Method for Optimizing VM In-Guest Business CPU Pinning Performance,
Inventor: 张海亮).

When a business application inside a VM uses sched_setaffinity to pin to
specific vCPUs, the guest-side interceptor (kprobe/eBPF) detects this and
notifies the VMM. The VMM then dynamically switches the corresponding vCPU
from range-pinning to 1:1 exclusive pinning on the host side, achieving
stable performance without sacrificing resource utilization.

Key features:
  - Guest-side interception via kprobe (sync) or eBPF CO-RE (async)
  - VMM-side dynamic 1:1 pinning on guest app bind notification
  - Automatic range-pinning restore on guest app unbind
  - Global CPU map with cross-VM conflict avoidance
  - Persistent state with auto-restore on reboot

%package guest
Summary:        Guest-side sched_setaffinity interceptor for vm-bindcore
Requires:       python3 >= 3.8

%description guest
Guest-side component of vm-bindcore. Installs inside the VM to intercept
sched_setaffinity calls and notify the VMM via hypercall/wrmsr.
Supports eBPF CO-RE (compile once, run everywhere) for cross-kernel
compatibility.

%prep
%setup -q

%build
# Nothing to build for a pure Python tool

%install
# Install VMM-side tool
install -D -m 0755 src/vm_bindcore.py \
    %{buildroot}%{_bindir}/vm-bindcore

# Install guest-side tool
install -D -m 0755 src/vm_bindcore_guest.py \
    %{buildroot}%{_bindir}/vm-bindcore-guest

# Install restore helper
install -D -m 0755 src/vm_bindcore_restore.py \
    %{buildroot}%{_libexecdir}/vm-bindcore/vm_bindcore_restore.py

# Install systemd service
install -D -m 0644 src/vm-bindcore-restore.service \
    %{buildroot}%{_unitdir}/vm-bindcore-restore.service

# Create config directory
install -d -m 0755 %{buildroot}%{_sysconfdir}/vm-bindcore

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
%{_mandir}/man1/vm-bindcore.1*

%files guest
%license LICENSE
%{_bindir}/vm-bindcore-guest

%changelog
* Mon Apr 14 2026 openEuler Contributors <dev@openeuler.org> - 2.0.0-1
- Rewrite to match patent: 一种优化虚拟机内业务绑核性能的方法
- Guest-side interception via kprobe/eBPF with hypercall notification
- VMM-side dynamic 1:1 pinning on guest app bind/unbind
- Global CPU map with cross-VM conflict avoidance
- Separate guest RPM subpackage (vm-bindcore-guest)

* Mon Apr 14 2026 openEuler Contributors <dev@openeuler.org> - 1.0.0-1
- Initial release
