# vm-bindcore

VM vCPU CPU-Pinning Management Tool (NUMA-aware) for openEuler KVM/libvirt environments.

Based on patent: **一种虚拟机绑核方法及计算设备** (Inventor: 张海亮)

## Features

- **NUMA-aware pinning**: Pin all vCPUs to the same NUMA node
- **Spread pinning**: Distribute vCPUs evenly across NUMA nodes
- **Compact pinning**: Pack vCPUs onto fewest physical cores (SMT-aware)
- **Manual pinning**: Precise vCPU-to-pCPU mapping
- **Persistent configuration**: Auto-restore on reboot via systemd

## Installation

```bash
yum install vm-bindcore
```

## Quick Start

```bash
# View host NUMA topology
vm-bindcore topology

# Pin with NUMA-aware strategy
vm-bindcore pin --vm my-vm --strategy numa-aware

# Pin with spread strategy
vm-bindcore pin --vm my-vm --strategy spread

# Show pinning status
vm-bindcore show --vm my-vm

# Remove pinning
vm-bindcore unpin --vm my-vm
```

## License

MulanPSL-2.0
