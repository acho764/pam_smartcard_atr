# PAM Smart Card ATR Authentication

A complete smart card authentication system for Linux using ATR (Answer To Reset) based identification. This system provides passwordless login and automatic screen locking when smart cards are removed.

## Features

- **ATR-based Authentication** - Works with any PC/SC compatible smart card (no PKCS#11 certificates required)
- **PAM Integration** - Seamless integration with GDM and Linux authentication stack
- **Multi-user Support** - Single card can authenticate multiple users
- **Automatic Screen Locking** - Locks screen when smart card is removed
- **Event-based Monitoring** - Real-time card insertion/removal detection
- **Systemd Integration** - Proper service management and auto-start

## Components

### 1. PAM Module (`pam_smartcard_atr`)
- Custom PAM module for ATR-based authentication
- Reads smart card ATR using PC/SC interface
- Maps ATR to usernames via configuration file
- Provides passwordless login at GDM

### 2. Screen Lock Service (`smartcard_lock`)
- C++ systemd service for monitoring card removal
- Automatically locks user sessions when card is removed
- Multi-user aware - only locks sessions for card owners
- Event-based monitoring using pcscd

## Installation

### Prerequisites

```bash
sudo apt-get update
sudo apt-get install -y pcscd libpcsclite-dev libpam0g-dev build-essential
```

### 1. Build and Install PAM Module

```bash
# Build PAM module
make -f Makefile-pam

# Install PAM module
make -f Makefile-pam install-pam
sudo cp /lib/x86_64-linux-gnu/security/pam_smartcard_atr.so /usr/lib/security/
```

### 2. Build and Install Lock Service

```bash
# Build lock service
make -f Makefile-lock

# Install lock service
make -f Makefile-lock install-lock
```

### 3. Configure Smart Card Mappings

Get your smart card's ATR:
```bash
pcsc_scan
```

Edit the configuration file:
```bash
sudo nano /etc/smartcard_login.conf
```

Add your mappings:
```
# Format: ATR:username
3BE700FF8131FE454430382E32203655:alice
3BE700FF8131FE454430382E32203655:bob
4A9B7C02905E1DB100000407040002000000007B:charlie
```

### 4. Configure GDM PAM

```bash
# Backup original configuration
sudo cp /etc/pam.d/gdm-password /etc/pam.d/gdm-password.bak

# Install new configuration with smart card support
sudo cp gdm-password-atr /etc/pam.d/gdm-password
```

### 5. Start Services

```bash
# Start the lock service
sudo systemctl start smartcard-lock.service
sudo systemctl enable smartcard-lock.service

# Restart GDM to load new PAM configuration
sudo systemctl restart gdm3
```

## Usage

### Login Process
1. **Log out** from your current session
2. **Insert smart card** at the GDM login screen
3. **Automatic login** occurs if ATR matches configured user
4. **Multi-user cards** will show user selection dialog

### Screen Locking
- **Remove smart card** → Screen locks automatically
- **Insert smart card** → Shows login screen for authentication

### Multiple Users
- Same card can authenticate multiple users
- Configuration supports multiple ATR:username mappings
- GDM presents user selection for multi-user cards

## Configuration Files

### `/etc/smartcard_login.conf`
```bash
# Smart card ATR to username mapping
# Format: ATR:username
# Comments start with #

# Example entries:
3BE700FF8131FE454430382E32203655:alice
3BE700FF8131FE454430382E32203655:bob
4A9B7C02905E1DB100000407040002000000007B:charlie
```

### `/etc/pam.d/gdm-password`
```pam
#%PAM-1.0
auth    requisite       pam_nologin.so
auth    required        pam_succeed_if.so user != root quiet_success
# Smart card ATR authentication
auth    sufficient      pam_smartcard_atr.so
@include common-auth
# ... rest of configuration
```

## Architecture

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   Smart Card    │────│   PC/SC Daemon   │────│   PAM Module    │
│      (ATR)      │    │     (pcscd)      │    │ (pam_smartcard_ │
└─────────────────┘    └──────────────────┘    │      atr)       │
                                               └─────────────────┘
                                                        │
                                               ┌─────────────────┐
                                               │      GDM        │
                                               │  (Login Mgr)    │
                                               └─────────────────┘

┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   Smart Card    │────│   PC/SC Daemon   │────│  Lock Service   │
│   (Removal)     │    │     (pcscd)      │    │ (smartcard_lock)│
└─────────────────┘    └──────────────────┘    └─────────────────┘
                                                        │
                                               ┌─────────────────┐
                                               │    loginctl     │
                                               │ (Session Lock)  │
                                               └─────────────────┘
```

## Files Structure

```
├── pam_smartcard_atr.c         # PAM module source
├── pam_smartcard_atr.so        # Compiled PAM module
├── Makefile-pam               # PAM module build system
├── smartcard_lock.cpp         # Lock service source
├── smartcard_lock             # Compiled lock service
├── Makefile-lock             # Lock service build system
├── smartcard-lock.service    # Systemd service file
├── smartcard_login.conf      # ATR to username mapping
├── gdm-password-atr         # GDM PAM configuration
├── CLAUDE.md               # Development progress
└── README.md              # This file
```

## Troubleshooting

### Check PAM Module
```bash
# Test PAM authentication
sudo pamtester gdm-password username authenticate

# Check PAM module loading
sudo grep "pam_smartcard_atr" /var/log/auth.log
```

### Check Lock Service
```bash
# Check service status
sudo systemctl status smartcard-lock.service

# View service logs
journalctl -u smartcard-lock.service -f
```

### Check Smart Card
```bash
# List card readers
pcsc_scan

# Check PC/SC daemon
sudo systemctl status pcscd
```

### Common Issues

**"Module is unknown"**: PAM module not found
- Ensure module is in both `/lib/x86_64-linux-gnu/security/` and `/usr/lib/security/`

**"No smart card found"**: PC/SC issues
- Check `pcscd` service is running
- Verify card reader is detected with `pcsc_scan`

**Authentication fails**: Configuration issues
- Verify ATR in `/etc/smartcard_login.conf` matches `pcsc_scan` output
- Check file permissions on configuration file

## Security Considerations

- **Root privileges**: Both services run as root for system-level access
- **Configuration permissions**: `/etc/smartcard_login.conf` should have restrictive permissions (600)
- **Physical security**: System security depends on physical card possession
- **Session isolation**: Each user's session is isolated and properly managed

## Development

### Building from Source
```bash
# Clone repository
git clone https://github.com/acho764/pam_smartcard_atr.git
cd pam_smartcard_atr

# Build PAM module
make -f Makefile-pam

# Build lock service
make -f Makefile-lock
```

### Contributing
1. Fork the repository
2. Create feature branch
3. Test thoroughly on your system
4. Submit pull request

## License

This project is provided as-is for educational and practical use. Ensure compliance with your organization's security policies before deployment.

## Author

Developed with Claude Code assistant for secure smart card authentication on Linux systems.