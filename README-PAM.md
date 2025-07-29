# Smart Card ATR-based PAM Authentication

A custom PAM module that provides ATR-based smart card authentication for Linux systems.

## Files

- `pam_smartcard_atr.c` - Custom PAM module source code
- `pam_smartcard_atr.so` - Compiled PAM module
- `Makefile-pam` - Build system for PAM module
- `smartcard_login.conf` - Configuration file mapping ATR to usernames
- `gdm-password-atr` - GDM PAM configuration with smart card support
- `CLAUDE.md` - Development progress log

## Installation

1. Build the PAM module:
```bash
make -f Makefile-pam
```

2. Install the PAM module:
```bash
make -f Makefile-pam install-pam
sudo cp /lib/x86_64-linux-gnu/security/pam_smartcard_atr.so /usr/lib/security/
```

3. Install configuration:
```bash
sudo cp smartcard_login.conf /etc/
sudo cp gdm-password-atr /etc/pam.d/gdm-password
```

## Configuration

Edit `/etc/smartcard_login.conf` with your card ATR and username:
```
# Format: ATR:username
3BE700FF8131FE454430382E32203655:angel-dimitrov
```

Get your card ATR using:
```bash
pcsc_scan
```

## How it works

- When you insert your smart card at the GDM login screen
- PAM reads the card's ATR using PC/SC
- Matches ATR against `/etc/smartcard_login.conf`
- Automatically logs you in without password if match found

## Dependencies

- libpcsclite-dev
- libpam0g-dev
- pcscd