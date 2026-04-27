# dmemcg-booster

A C-based daemon that manages device memory (dmem) cgroup controllers in Linux systems. It automatically activates the `dmem` controller and propagates device memory limits across cgroup hierarchies in response to systemd unit lifecycle events.

## Overview

`dmemcg-booster` monitors systemd for new and removed units (services, scopes, slices, sockets) and ensures that the device memory controller is properly activated and configured throughout the cgroup hierarchy. This is essential for systems using device memory management via cgroups.

## Features

- **Automatic Controller Activation**: Automatically enables the `dmem` controller in cgroup subtrees where needed
- **Systemd Integration**: Listens to DBus signals for `UnitNew` and `UnitRemoved` events
- **Hierarchical Propagation**: Properly propagates dmem settings through parent-child cgroup relationships
- **User/System Mode**: Supports both system-wide and user-session modes
- **Limit Synchronization**: Reads `dmem.capacity` and writes to `dmem.low` for proper memory limit enforcement

## Requirements

- Linux system with cgroups v2
- systemd with DBus support
- libdbus-1 development libraries
- GCC compiler

### Installing Dependencies

**Debian/Ubuntu:**
```bash
sudo apt-get install libdbus-1-dev pkg-config build-essential
```

**Fedora/RHEL:**
```bash
sudo dnf install dbus-devel pkg-config gcc make
```

**Arch Linux:**
```bash
sudo pacman -S dbus pkgconf base-devel
```

## Building

```bash
make
```

This produces the `dmemcg-booster` binary.

To clean build artifacts:
```bash
make clean
```

## Usage

### Basic Usage

Run in user session mode (default):
```bash
./dmemcg-booster
```

Run in system-wide mode:
```bash
./dmemcg-booster --use-system-bus
```

### Command Line Options

| Option | Description |
|--------|-------------|
| `--use-system-bus` | Connect to the system DBus instead of session DBus. Use this for system-wide cgroup management. |

### Running as a Service

Systemd service files are provided for both system and user modes.

**System Service:**
```bash
sudo cp dmemcg-booster-system.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now dmemcg-booster-system.service
```

**User Service:**
```bash
mkdir -p ~/.config/systemd/user/
cp dmemcg-booster-user.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now dmemcg-booster-user.service
```

## Architecture

### Source Files

- **`src/cgroup.h`**: Header file defining data structures and function prototypes
  - `CGroup`: Structure representing a cgroup path
  - `DMemLimit`: Structure for device memory limit entries
  - Core cgroup manipulation functions

- **`src/cgroup.c`**: Cgroup filesystem operations
  - Hierarchy traversal (parent, descendants)
  - Controller activation (`cgroup.subtree_control`)
  - Device memory limit parsing and writing (`dmem.capacity`, `dmem.low`)

- **`src/main.c`**: Main program entry point
  - DBus connection and signal handling
  - Systemd unit event processing
  - Coordination of cgroup operations

### How It Works

1. **Initialization**: On startup, the daemon traverses all existing cgroups and activates the `dmem` controller where appropriate.

2. **Event Monitoring**: Listens for systemd DBus signals:
   - `UnitNew`: Triggered when a new unit is created
   - `UnitRemoved`: Triggered when a unit is removed

3. **Controller Activation**: For each new unit:
   - Queries the unit's cgroup path via DBus
   - Walks up the hierarchy to ensure parent controllers are active
   - Enables the `dmem` controller in `cgroup.subtree_control`
   - Copies `dmem.capacity` values to `dmem.low` for enforcement

4. **User Isolation**: In system mode, avoids modifying cgroups belonging to user sessions to prevent privilege escalation.

## Cgroup File Operations

The daemon interacts with the following cgroup files:

| File | Purpose |
|------|---------|
| `cgroup.subtree_control` | Enable/disable controllers for child cgroups |
| `dmem.capacity` | Read device memory capacity limits |
| `dmem.low` | Write device memory low limits for enforcement |

## Safety Considerations

- **Root Privileges**: System mode typically requires root access to modify system cgroups
- **User Sessions**: The daemon respects user session boundaries and won't modify user cgroups in system mode
- **Idempotent**: Operations can be safely run multiple times without side effects

## Troubleshooting

### Common Issues

**"Failed to connect to DBus"**
- Ensure DBus is running: `systemctl status dbus`
- For user mode, ensure `DBUS_SESSION_BUS_ADDRESS` is set
- For system mode, run with appropriate privileges

**"No such file or directory" on cgroup paths**
- Verify cgroups v2 is mounted: `mount | grep cgroup`
- Check that `/sys/fs/cgroup` exists and is accessible

**Permission denied errors**
- Run with `sudo` for system mode
- Ensure user has access to cgroup filesystem

### Debugging

Build with debug symbols (default with `make`) and run with strace:
```bash
strace -f ./dmemcg-booster --use-system-bus 2>&1 | head -100
```

Check systemd journal for related messages:
```bash
journalctl -u dmemcg-booster-system.service -f
```

## License

See the [LICENSE](LICENSE) file for licensing information.

## Contributing

Contributions are welcome! Please ensure code follows the existing C style and includes appropriate error handling.